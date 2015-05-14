#include <sys/socket.h>
#include <poll.h>
#include <errno.h>
#include <stdlib.h>
#include <list>
#include <queue>
#include <vector>
#include <algorithm>

#include "ConnectionPool.h"
#include "Utility.h"
#include "Keywords.h"
#include "Parser.h"

using std::vector;

using douban::mc::keywords::kCRLF;
using douban::mc::keywords::kSPACE;
using douban::mc::keywords::k_NOREPLY;

using douban::mc::hashkit::KetamaSelector;

using douban::mc::types::RetrievalResult;
using douban::mc::types::retrieval_result_t;

namespace douban {
namespace mc {

ConnectionPool::ConnectionPool()
  : m_nActiveConn(0), m_nInvalidKey(0), m_conns(NULL), m_nConns(0) {
}


int ConnectionPool::s_pollTimeout = MC_DEFAULT_POLL_TIMEOUT;


ConnectionPool::~ConnectionPool() {
  delete[] m_conns;
}


void ConnectionPool::setHashFunction(hash_function_options_t fn_opt) {
  switch (fn_opt) {
    case OPT_HASH_MD5:
      m_connSelector.setHashFunction(&douban::mc::hashkit::hash_md5);
      break;
    case OPT_HASH_FNV1_32:
      m_connSelector.setHashFunction(&douban::mc::hashkit::hash_fnv1_32);
      break;
    case OPT_HASH_FNV1A_32:
      m_connSelector.setHashFunction(&douban::mc::hashkit::hash_fnv1a_32);
      break;
    case OPT_HASH_CRC_32:
      m_connSelector.setHashFunction(&douban::mc::hashkit::hash_crc_32);
      break;
    default:
      NOT_REACHED();
      break;
  }
}


int ConnectionPool::init(const char* const * hosts, const uint32_t* ports, const size_t n,
                         const uint32_t* weights) {
  delete[] m_conns;
  m_connSelector.reset();
  int rv = 0;
  m_nConns = n;
  m_conns = new Connection[m_nConns];
  for (size_t i = 0; i < m_nConns; i++) {
    rv += m_conns[i].init(hosts[i], ports[i]);
  }
  m_connSelector.addServers(m_conns, m_nConns);
  return rv;
}


const char* ConnectionPool::getServerAddressByKey(const char* key, size_t keyLen) {
  bool check_alive = false;
  Connection* conn = m_connSelector.getConn(key, keyLen, check_alive);
  if (conn == NULL) {
    return NULL;
  }
  return conn->name();
}


void ConnectionPool::enableConsistentFailover() {
  m_connSelector.enableFailover();
}


void ConnectionPool::disableConsistentFailover() {
  m_connSelector.disableFailover();
}


void ConnectionPool::dispatchStorage(op_code_t op,
                                      const char* const* keys, const size_t* keyLens,
                                      const types::flags_t* flags, const types::exptime_t exptime,
                                      const types::cas_unique_t* cas_uniques, const bool noreply,
                                      const char* const* vals, const size_t* val_lens,
                                      size_t nItems) {

  size_t i = 0, idx = 0;

  for (; i < nItems; ++i) {
    if (!utility::isValidKey(keys[i], keyLens[i])) {
      m_nInvalidKey += 1;
      continue;
    }
    Connection* conn = m_connSelector.getConn(keys[i], keyLens[i]);
    if (conn == NULL) {
      continue;
    }
    switch (op) {
      case SET_OP:
        conn->takeBuffer(keywords::kSET_, 4);
        break;
      case ADD_OP:
        conn->takeBuffer(keywords::kADD_, 4);
        break;
      case REPLACE_OP:
        conn->takeBuffer(keywords::kREPLACE_, 8);
        break;
      case APPEND_OP:
        conn->takeBuffer(keywords::kAPPEND_, 7);
        break;
      case PREPEND_OP:
        conn->takeBuffer(keywords::kPREPEND_, 8);
        break;
      case CAS_OP:
        conn->takeBuffer(keywords::kCAS_, 4);
        break;
      default:
        NOT_REACHED();
        break;
    }

    conn->takeBuffer(keys[i], keyLens[i]);
    conn->takeBuffer(kSPACE, 1);
    conn->takeNumber(flags[i]);
    conn->takeBuffer(kSPACE, 1);
    conn->takeNumber(exptime);
    conn->takeBuffer(kSPACE, 1);
    conn->takeNumber(val_lens[i]);
    if (op == CAS_OP) {
      conn->takeBuffer(kSPACE, 1);
      conn->takeNumber(cas_uniques[i]);
    }
    if (noreply) {
      conn->takeBuffer(k_NOREPLY, 8);
    } else {
      conn->addRequestKey(keys[i], keyLens[i]);
    }
    ++conn->m_counter;
    conn->takeBuffer(kCRLF, 2);
    conn->takeBuffer(vals[i], val_lens[i]);
    conn->takeBuffer(kCRLF, 2);
  }

  for (idx = 0; idx < m_nConns; idx++) {
    Connection* conn = m_conns + idx;
    if (conn->m_counter > 0) {
      conn->setParserMode(MODE_COUNTING);
      m_nActiveConn += 1;
      m_activeConns.push_back(conn);
    }
    // for ignore noreply
    conn->m_counter = conn->requestKeyCount();
    if (conn->m_counter > 0) {
      conn->getMessageResults()->reserve(conn->m_counter);
    }
  }
}


void ConnectionPool::dispatchRetrieval(op_code_t op, const char* const* keys,
                                  const size_t* keyLens, size_t n_keys) {
  size_t i = 0, idx = 0;

  double beforeLoop = utility::getCPUTime();
  for (; i < n_keys; ++i) {
    double inLoop = utility::getCPUTime();
    if (inLoop - beforeLoop > 0.1) {
      log_warn(
        "probe dispatchRetrieval timeout %.6f s, i: %zu, keys[i - 1]: %.*s",
        inLoop - beforeLoop, i, static_cast<int>(keyLens[i - 1]), keys[i - 1]
      );
    }

    const char* key = keys[i];
    const size_t len = keyLens[i];
    if (!utility::isValidKey(key, len)) {
      m_nInvalidKey += 1;
      continue;
    }
    double t0 = utility::getCPUTime();
    Connection* conn = m_connSelector.getConn(key, len);
    double t1 = utility::getCPUTime();
    if (t1 - t0 > 0.1) {
      char* conn_nm = NULL;
      if (conn != NULL) {
        conn_nm = const_cast<char*>(conn->name());
      }
      log_warn("probe getConn timeout. key: %s, srv: %s, to: %.6f, conn_poll_to: %d",
               key, conn_nm, t1 - t0, Connection::getConnectTimeout());
    }
    if (conn == NULL) {
      continue;
    }
    if (++conn->m_counter == 1) {
      switch (op) {
        case GET_OP:
          conn->takeBuffer(keywords::kGET, 3);
          break;
        case GETS_OP:
          conn->takeBuffer(keywords::kGETS, 4);
          break;
        default:
          NOT_REACHED();
          break;
      }
    }
    conn->takeBuffer(kSPACE, 1);
    conn->takeBuffer(key, len);
    conn->addRequestKey(key, len);
  }
  double afterLoop = utility::getCPUTime();
  if (afterLoop - beforeLoop > 0.1) {
    log_warn(
      "probe dispatchRetrieval timeout %.6f s",
      (afterLoop - beforeLoop)
    );
  }

  for (idx = 0; idx < m_nConns; idx++) {
    Connection* conn = m_conns + idx;
    if (conn->m_counter > 0) {
      conn->takeBuffer(kCRLF, 2);
      conn->setParserMode(MODE_END_STATE);
      m_nActiveConn += 1;
      m_activeConns.push_back(conn);
      conn->getRetrievalResults()->reserve(conn->m_counter);
    }
  }
  // debug("after dispatchRetrieval: m_nActiveConn: %d", this->m_nActiveConn);
}


void ConnectionPool::dispatchDeletion(const char* const* keys, const size_t* keyLens,
                                     const bool noreply, size_t nItems) {

  size_t i = 0, idx = 0;
  for (; i < nItems; ++i) {
    if (!utility::isValidKey(keys[i], keyLens[i])) {
      m_nInvalidKey += 1;
      continue;
    }
    Connection* conn = m_connSelector.getConn(keys[i], keyLens[i]);
    if (conn == NULL) {
      continue;
    }

    conn->takeBuffer(keywords::kDELETE_, 7);
    conn->takeBuffer(keys[i], keyLens[i]);
    if (noreply) {
      conn->takeBuffer(k_NOREPLY, 8);
    } else {
      conn->addRequestKey(keys[i], keyLens[i]);
    }
    ++conn->m_counter;
    conn->takeBuffer(kCRLF, 2);
  }

  for (idx = 0; idx < m_nConns; idx++) {
    Connection* conn = m_conns + idx;
    if (conn->m_counter > 0) {
      conn->setParserMode(MODE_COUNTING);
      m_nActiveConn += 1;
      m_activeConns.push_back(conn);
    }
    // for ignore noreply
    conn->m_counter = conn->requestKeyCount();
    if (conn->m_counter > 0) {
      conn->getMessageResults()->reserve(conn->m_counter);
    }
  }
}


void ConnectionPool::dispatchTouch(
    const char* const* keys, const size_t* keyLens,
    const types::exptime_t exptime, const bool noreply, size_t nItems) {

  size_t i = 0, idx = 0;
  for (; i < nItems; ++i) {
    if (!utility::isValidKey(keys[i], keyLens[i])) {
      m_nInvalidKey += 1;
      continue;
    }
    Connection* conn = m_connSelector.getConn(keys[i], keyLens[i]);
    if (conn == NULL) {
      continue;
    }

    conn->takeBuffer(keywords::kTOUCH_, 6);
    conn->takeBuffer(keys[i], keyLens[i]);
    conn->takeBuffer(kSPACE, 1);
    conn->takeNumber(exptime);
    if (noreply) {
      conn->takeBuffer(k_NOREPLY, 8);
    } else {
      conn->addRequestKey(keys[i], keyLens[i]);
    }
    ++conn->m_counter;
    conn->takeBuffer(kCRLF, 2);
  }

  for (idx = 0; idx < m_nConns; idx++) {
    Connection* conn = m_conns + idx;
    if (conn->m_counter > 0) {
      conn->setParserMode(MODE_COUNTING);
      m_nActiveConn += 1;
      m_activeConns.push_back(conn);
    }
    // for ignore noreply
    conn->m_counter = conn->requestKeyCount();
    if (conn->m_counter > 0) {
      conn->getMessageResults()->reserve(conn->m_counter);
    }
  }
}


void ConnectionPool::dispatchIncrDecr(op_code_t op, const char* key, const size_t keyLen,
                                      const uint64_t delta, const bool noreply) {
  if (!utility::isValidKey(key, keyLen)) {
    m_nInvalidKey += 1;
    return;
  }
  Connection* conn = m_connSelector.getConn(key, keyLen);
  if (conn == NULL) {
    return;
  }
  switch (op) {
    case INCR_OP:
      conn->takeBuffer(keywords::kINCR_, 5);
      break;
    case DECR_OP:
      conn->takeBuffer(keywords::kDECR_, 5);
      break;
    default:
      NOT_REACHED();
      break;
  }
  conn->takeBuffer(key, keyLen);
  conn->takeBuffer(kSPACE, 1);
  conn->takeNumber(delta);
  if (noreply) {
    conn->takeBuffer(k_NOREPLY, 8);
  } else {
    conn->addRequestKey(key, keyLen);
  }
  ++conn->m_counter;
  conn->takeBuffer(kCRLF, 2);

  conn->setParserMode(MODE_COUNTING);
  m_nActiveConn += 1;
  m_activeConns.push_back(conn);

  // for ignore noreply
  // before the below line, conn->m_counter is a counter regarding how many packet to *send*
  conn->m_counter = conn->requestKeyCount();
  // after the upper line, conn->m_counter is a counter regarding how many packet to *recv*
}


void ConnectionPool::broadcastCommand(const char * const cmd, const size_t cmdLens) {
  for (size_t idx = 0; idx < m_nConns; ++idx) {
    Connection* conn = m_conns + idx;
    if (!conn->alive()) {
      if (!conn->tryReconnect()) {
        continue;
      }
    }
    conn->takeBuffer(cmd, cmdLens);
    ++conn->m_counter;
    conn->takeBuffer(kCRLF, 2);
    conn->setParserMode(MODE_END_STATE);
    m_nActiveConn += 1;
    m_activeConns.push_back(conn);
  }
}

err_code_t ConnectionPool::waitPoll() {
  if (m_nActiveConn == 0) {
    if (m_nInvalidKey > 0) {
      return RET_INVALID_KEY_ERR;
    } else {
      // hard server error
      return RET_MC_SERVER_ERR;
    }
  }
  nfds_t n_fds = m_nActiveConn;
  pollfd_t pollfds[n_fds];

  Connection* fd2conn[n_fds];

  pollfd_t* pollfd_ptr = NULL;
  nfds_t fd_idx = 0;

  for (std::vector<Connection*>::iterator it = m_activeConns.begin(); it != m_activeConns.end();
       ++it, ++fd_idx) {
    Connection* conn = *it;
    pollfd_ptr = &pollfds[fd_idx];
    pollfd_ptr->fd = conn->socketFd();
    pollfd_ptr->events = POLLOUT;
    fd2conn[fd_idx] = conn;
  }

  err_code_t ret_code = RET_OK;

  double beforeLoop = utility::getCPUTime();
  double t1 = 0;
  double t0 = 0;
  double maxPollElapse = 0.0;
  double maxSendElapse = 0.0;
  double maxRecvElapse = 0.0;
  double totalPollElapse = 0.0;
  double totalSendElapse = 0.0;
  double totalRecvElapse = 0.0;
  int poll_count = 0;
  int send_count = 0;
  int recv_count = 0;

  while (m_nActiveConn) {
    t0 = utility::getCPUTime();
    if (t0 - beforeLoop > 1.0) {
      log_warn(
          "probe time_elasped (%.6f s). n_conns: %zu, "
          "poll_count: %d, send_count: %d, recv_count: %d, "
          "max_poll_elapsed: %.6f, max_send_elasped: %.6f, max_recv_elasped: %.6f, "
          "total_poll_elapsed: %.6f, total_send_elasped: %.6f, total_recv_elasped: %.6f, "
          "s_pollTimeout: %d",
          t0 - beforeLoop, m_activeConns.size(),
          poll_count, send_count, recv_count,
          maxPollElapse, maxSendElapse, maxRecvElapse,
          totalPollElapse, totalSendElapse, totalRecvElapse,
          s_pollTimeout
      );
    }

    int rv = poll(pollfds, n_fds, s_pollTimeout);
    ++poll_count;
    t1 = utility::getCPUTime();
    double pollElapse = t1 - t0;
    ASSERT(pollElapse * 1000 < s_pollTimeout * 1.5);
    maxPollElapse = std::max(pollElapse, maxPollElapse);
    totalPollElapse += pollElapse;

    if (rv == -1) {
      if (errno == EINTR) {
        log_warn(
          "probe poll error (%.6f s). n_conns: %zu, "
          "poll_count: %d, send_count: %d, recv_count: %d, "
          "max_poll_elapsed: %.6f, max_send_elasped: %.6f, max_recv_elasped: %.6f, "
          "total_poll_elapsed: %.6f, total_send_elasped: %.6f, total_recv_elasped: %.6f "
          "s_pollTimeout: %d",
          t1 - beforeLoop, m_activeConns.size(),
          poll_count, send_count, recv_count,
          maxPollElapse, maxSendElapse, maxRecvElapse,
          totalPollElapse, totalSendElapse, totalRecvElapse,
          s_pollTimeout
        );
      }
      markDeadAll(pollfds, keywords::kPOLL_ERROR, 0);
      ret_code = RET_POLL_ERR;
      break;
    } else if (rv == 0) {
      log_warn("poll timeout. (m_nActiveConn: %d)", m_nActiveConn);
      // NOTE: MUST reset all active TCP connections after timeout.
      markDeadAll(pollfds, keywords::kPOLL_TIMEOUT, 0);
      ret_code = RET_POLL_TIMEOUT_ERR;
      break;
    } else {
      err_code_t err;
      for (fd_idx = 0; fd_idx < n_fds; fd_idx++) {
        pollfd_ptr = &pollfds[fd_idx];
        Connection* conn = fd2conn[fd_idx];

        if (pollfd_ptr->revents & (POLLERR | POLLHUP | POLLNVAL)) {
          markDeadConn(conn, keywords::kCONN_POLL_ERROR, pollfd_ptr, Connection::getRetryTimeout());
          ret_code = RET_CONN_POLL_ERR;
          m_nActiveConn -= 1;
          goto next_fd;
        }

        // send
        if (pollfd_ptr->revents & POLLOUT) {
          // POLLOUT send
          t0 = utility::getCPUTime();
          ssize_t nToSend = conn->send();
          t1 = utility::getCPUTime();
          double sendElapse = t1 - t0;
          maxSendElapse = std::max(sendElapse, maxSendElapse);
          totalSendElapse += sendElapse;
          ++send_count;
          if (nToSend == -1) {
            markDeadConn(conn, keywords::kSEND_ERROR, pollfd_ptr, 0);
            ret_code = RET_SEND_ERR;
            m_nActiveConn -= 1;
            goto next_fd;
          } else {
            // start to recv if any data is sent
            pollfd_ptr->events |= POLLIN;

            if (nToSend == 0) {
              // debug("[%d] all sent", pollfd_ptr->fd);
              pollfd_ptr->events &= ~POLLOUT;
              if (conn->m_counter == 0) {
                // just send, no recv for noreply
                --this->m_nActiveConn;
              }
            }
          }
        }

        // recv
        if (pollfd_ptr->revents & POLLIN) {
          // POLLIN recv
          t0 = utility::getCPUTime();
          ssize_t nRecv = conn->recv();
          t1 = utility::getCPUTime();
          double recvElapse = t1 - t0;
          maxRecvElapse = std::max(recvElapse, maxRecvElapse);
          totalRecvElapse += recvElapse;
          ++recv_count;
          if (nRecv == -1 || nRecv == 0) {
            markDeadConn(conn, keywords::kRECV_ERROR, pollfd_ptr, 0);
            ret_code = RET_RECV_ERR;
            m_nActiveConn -= 1;
            goto next_fd;
          }

          conn->process(err);
          switch (err) {
            case RET_OK:
              pollfd_ptr->events &= ~POLLIN;
              --m_nActiveConn;
              break;
            case RET_INCOMPLETE_BUFFER_ERR:
              break;
            case RET_PROGRAMMING_ERR:
              markDeadConn(conn, keywords::kPROGRAMMING_ERROR, pollfd_ptr, Connection::getRetryTimeout());
              ret_code = RET_PROGRAMMING_ERR;
              m_nActiveConn -= 1;
              goto next_fd;
              break;
            case RET_MC_SERVER_ERR:
              // soft server error
              markDeadConn(conn, keywords::kSERVER_ERROR, pollfd_ptr, 0);
              ret_code = RET_MC_SERVER_ERR;
              m_nActiveConn -= 1;
              goto next_fd;
              break;
            default:
              NOT_REACHED();
              break;
          }
        }

next_fd: {}
      } // end for
    }
  }

  double afterLoop = utility::getCPUTime();
  double timeElapse = afterLoop - beforeLoop;
  if (timeElapse > 1.0) {
    log_warn(
      "probe timeout (%.6f s). n_conns: %zu, "
      "poll_count: %d, send_count: %d, recv_count: %d, "
      "max_poll_elapsed: %.6f, max_send_elasped: %.6f, max_recv_elasped: %.6f, "
      "total_poll_elapsed: %.6f, total_send_elasped: %.6f, total_recv_elasped: %.6f, "
      "s_pollTimeout: %d",
      timeElapse, m_activeConns.size(),
      poll_count, send_count, recv_count,
      maxPollElapse, maxSendElapse, maxRecvElapse,
      totalPollElapse, totalSendElapse, totalRecvElapse,
      s_pollTimeout
    );
    for (std::vector<Connection*>::iterator it = m_activeConns.begin();
         it != m_activeConns.end(); ++it) {
      Connection* conn = *it;
      std::queue<struct iovec>* q = conn->getRequestKeys();
      if (!q->empty()) {
        log_warn(
            "probe key %s :(%.6f s): first request key: %.*s, total: %zu",
            conn->name(),
            timeElapse,
            static_cast<int>(q->front().iov_len),
            static_cast<char*>(q->front().iov_base),
            q->size()
        );
      }
    }
  }

  return ret_code;
}


void ConnectionPool::collectRetrievalResult(std::vector<types::retrieval_result_t*>& results) {
  for (std::vector<Connection*>::iterator it = m_activeConns.begin();
       it != m_activeConns.end(); ++it) {
    types::RetrievalResultList* rst = (*it)->getRetrievalResults();

    for (types::RetrievalResultList::iterator it2 = rst->begin(); it2 != rst->end(); ++it2) {
      RetrievalResult& r1 = *it2;
      if (r1.bytesRemain > 0) {
        // This may be triggered on get_multi when data_block
        // of one retrieval result is not complete yet.
        continue;
      }
      results.push_back(r1.inner());
    }
  }
}


void ConnectionPool::collectMessageResult(std::vector<types::message_result_t*>& results) {
  for (std::vector<Connection*>::iterator it = m_activeConns.begin();
       it != m_activeConns.end(); ++it) {
    types::MessageResultList* rst = (*it)->getMessageResults();

    for (types::MessageResultList::iterator it2 = rst->begin(); it2 != rst->end(); ++it2) {
      results.push_back(&(*it2));
    }
  }
}


void ConnectionPool::collectBroadcastResult(std::vector<types::broadcast_result_t>& results) {
  results.resize(m_nConns);
  for (size_t i = 0; i < m_nConns; ++i) {
    Connection* conn = m_conns + i;
    types::broadcast_result_t* conn_result = &results[i];
    conn_result->host = const_cast<char*>(conn->name());
    types::LineResultList* rst = conn->getLineResults();
    conn_result->len = rst->size();

    if (conn_result->len == 0) {
      conn_result->lines = NULL;
      conn_result->line_lens = NULL;
      continue;
    }
    conn_result->lines = new char*[conn_result->len];
    conn_result->line_lens = new size_t[conn_result->len];

    int j = 0;
    for (types::LineResultList::iterator it2 = rst->begin(); it2 != rst->end(); ++it2, ++j) {
      types::LineResult* r1 = &(*it2);
      conn_result->lines[j] = r1->inner(conn_result->line_lens[j]);
    }
  }
}


void ConnectionPool::collectUnsignedResult(std::vector<types::unsigned_result_t*>& results) {
  if (m_activeConns.size() == 1) {
    types::UnsignedResultList* numericRst =  m_activeConns.front()->getUnsignedResults();
    types::MessageResultList* msgRst = m_activeConns.front()->getMessageResults();

    if (numericRst->size() == 1) {
      results.push_back(&numericRst->front());
    } else if (msgRst->size() == 1) {
      ASSERT(msgRst->front().type == types::MSG_NOT_FOUND);
      results.push_back(NULL);
    }
  }
}


void ConnectionPool::reset() {
  for (std::vector<Connection*>::iterator it = m_activeConns.begin();
       it != m_activeConns.end(); ++it) {
    (*it)->reset();
  }
  m_nActiveConn = 0;
  m_nInvalidKey = 0;
  m_activeConns.clear();
}


void ConnectionPool::setPollTimeout(int timeout) {
  s_pollTimeout = timeout;
}


void ConnectionPool::markDeadAll(pollfd_t* pollfds, const char* reason, int delay) {

  nfds_t fd_idx = 0;
  for (std::vector<Connection*>::iterator it = m_activeConns.begin();
      it != m_activeConns.end();
      ++it, ++fd_idx) {
    Connection* conn = *it;
    pollfd_t* pollfd_ptr = &pollfds[fd_idx];
    if (pollfd_ptr->events & (POLLOUT | POLLIN)) {
      conn->markDead(reason, delay);
    }
  }
}


void ConnectionPool::markDeadConn(Connection* conn, const char* reason, pollfd_t* fd_ptr, int delay) {
  conn->markDead(reason, delay);
  fd_ptr->events = ~POLLOUT & ~POLLIN;
  fd_ptr->fd = conn->socketFd();
}


} // namespace mc
} // namespace douban
