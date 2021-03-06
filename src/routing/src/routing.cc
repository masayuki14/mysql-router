/*
  Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "mysqlrouter/routing.h"
#include "mysqlrouter/utils.h"
#include "config.h"
#include "logger.h"
#include "utils.h"

#include <cstring>
#include <climits>

#ifndef _WIN32
# ifdef __sun
#  include <fcntl.h>
# else
#  include <sys/fcntl.h>
# endif
# include <netdb.h>
# include <netinet/tcp.h>
# include <sys/socket.h>
# include <poll.h>
#else
# define WIN32_LEAN_AND_MEAN
# include <windows.h>
# include <winsock2.h>
# include <ws2tcpip.h>
#endif

using mysqlrouter::to_string;
using mysqlrouter::string_format;
using mysqlrouter::TCPAddress;

namespace routing {

const int kDefaultWaitTimeout = 0; // 0 = no timeout used
const int kDefaultMaxConnections = 512;
const std::chrono::seconds kDefaultDestinationConnectionTimeout { 1 };
const std::string kDefaultBindAddress = "127.0.0.1";
const unsigned int kDefaultNetBufferLength = 16384;  // Default defined in latest MySQL Server
const unsigned long long kDefaultMaxConnectErrors = 100;  // Similar to MySQL Server
const std::chrono::seconds kDefaultClientConnectTimeout { 9 }; // Default connect_timeout MySQL Server minus 1

// unused constant
// const int kMaxConnectTimeout = INT_MAX / 1000;


const char* const kAccessModeNames[] = {
  nullptr, "read-write", "read-only"
};

constexpr size_t kAccessModeCount =
    sizeof(kAccessModeNames)/sizeof(*kAccessModeNames);

AccessMode get_access_mode(const std::string& value) {
  for (unsigned int i = 1 ; i < kAccessModeCount ; ++i)
    if (strcmp(kAccessModeNames[i], value.c_str()) == 0)
      return static_cast<AccessMode>(i);
  return AccessMode::kUndefined;
}

void get_access_mode_names(std::string* valid) {
  unsigned int i = 1;
  while (i < kAccessModeCount) {
    valid->append(kAccessModeNames[i]);
    if (++i < kAccessModeCount)
      valid->append(", ");
  }
}

std::string get_access_mode_name(AccessMode access_mode) noexcept {
  return kAccessModeNames[static_cast<int>(access_mode)];
}

void set_socket_blocking(int sock, bool blocking) {

  assert(!(sock < 0));
#ifndef _WIN32
  auto flags = fcntl(sock, F_GETFL, nullptr);
  assert(flags >= 0);
  if (blocking) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |= O_NONBLOCK;
  }
  fcntl(sock, F_SETFL, flags);
#else
  u_long mode = blocking ? 0 : 1;
  ioctlsocket(sock, FIONBIO, &mode);
#endif
}

SocketOperations* SocketOperations::instance() {
  static SocketOperations instance_;
  return &instance_;
}

int SocketOperations::poll(struct pollfd *fds, nfds_t nfds, std::chrono::milliseconds timeout_ms) {
#ifdef _WIN32
  return ::WSAPoll(fds, nfds, timeout_ms.count());
#else
  return ::poll(fds, nfds, timeout_ms.count());
#endif
}

int SocketOperations::connect_non_blocking_wait(int sock, std::chrono::milliseconds timeout_ms) {
  struct pollfd fds[] = {
    { sock, POLLOUT, 0 },
  };

  int res = poll(fds, sizeof(fds) / sizeof(fds[0]), timeout_ms);

  if (0 == res) {
    // timeout
    this->set_errno(ETIMEDOUT);
    return -1;
  } else if (res < 0) {
    // some error
    return -1;
  }

  bool connect_writable = (fds[0].revents & POLLOUT) != 0;

  if (!connect_writable) {
    // this should not happen
    this->set_errno(EINVAL);
    return -1;
  }

  return 0;
}

int SocketOperations::connect_non_blocking_status(int sock, int &so_error) {
  socklen_t error_len = static_cast<socklen_t>(sizeof(so_error));

  if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&so_error), &error_len) == -1) {
    so_error = get_errno();
    return -1;
  }

  if (so_error) {
    return -1;
  }

  return 0;
}

int SocketOperations::get_mysql_socket(TCPAddress addr, std::chrono::milliseconds connect_timeout_ms, bool log) noexcept {
  struct addrinfo *servinfo, *info, hints;

  memset(&hints, 0, sizeof hints);
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  bool timeout_expired = false;

  int err;
  if ((err = ::getaddrinfo(addr.addr.c_str(), to_string(addr.port).c_str(), &hints, &servinfo)) != 0) {
    if (log) {
#ifndef _WIN32
      std::string errstr{(err == EAI_SYSTEM) ? get_message_error(get_errno()) : gai_strerror(err)};
#else
      std::string errstr = get_message_error(err);
#endif
      log_debug("Failed getting address information for '%s' (%s)", addr.addr.c_str(), errstr.c_str());
    }
    return -1;
  }

  std::shared_ptr<void> exit_guard(nullptr, [&](void*){if (servinfo) freeaddrinfo(servinfo);});

  int sock = routing::kInvalidSocket;

  for (info = servinfo; info != nullptr; info = info->ai_next) {
    if ((sock = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol)) == -1) {
      log_error("Failed opening socket: %s", get_message_error(get_errno()).c_str());
    } else {
      bool connection_is_good = true;

      set_socket_blocking(sock, false);

      if (::connect(sock, info->ai_addr, info->ai_addrlen) < 0) {
        switch (this->get_errno()) {
#ifdef _WIN32
          case WSAEINPROGRESS:
          case WSAEWOULDBLOCK:
#else
          case EINPROGRESS:
#endif
            if (0 != connect_non_blocking_wait(sock, connect_timeout_ms)) {
              log_warning("Timeout reached trying to connect to MySQL Server %s: %s", addr.str().c_str(), get_message_error(get_errno()).c_str());
              connection_is_good = false;
              timeout_expired = (get_errno() == ETIMEDOUT);
              break;
            }

            {
              int so_error = 0;
              if (0 != connect_non_blocking_status(sock, so_error)) {
                connection_is_good = false;
                break;
              }
            }

            // success, we can continue
            break;
          default:
            log_debug("Failed connect() to %s: %s", addr.str().c_str(), get_message_error(get_errno()).c_str());
            connection_is_good = false;
            break;
        }
      } else {
        // everything is fine, we are connected
      }

      if (connection_is_good) {
        break;
      }

      // some error, close the socket again and try the next one
      this->close(sock);
    }
  }

  if (info == nullptr) {
    // all connects failed.
    return timeout_expired ? -2 : -1;
  }

  // set blocking; MySQL protocol is blocking and we do not take advantage of
  // any non-blocking possibilities
  set_socket_blocking(sock, true);

  int opt_nodelay = 1;
  if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&opt_nodelay), // cast keeps Windows happy (const void* on Unix)
                 static_cast<socklen_t>(sizeof(int))) == -1) {
    log_debug("Failed setting TCP_NODELAY on client socket");
    this->close(sock);

    return -1;
  }

  return sock;
}

ssize_t SocketOperations::write(int fd, void *buffer, size_t nbyte) {
#ifndef _WIN32
  return ::write(fd, buffer, nbyte);
#else
  return ::send(fd, reinterpret_cast<const char *>(buffer), nbyte, 0);
#endif
}

ssize_t SocketOperations::read(int fd, void *buffer, size_t nbyte) {
#ifndef _WIN32
  return ::read(fd, buffer, nbyte);
#else
  return ::recv(fd, reinterpret_cast<char *>(buffer), nbyte, 0);
#endif
}

void SocketOperations::close(int fd) {
#ifndef _WIN32
  ::close(fd);
#else
  ::closesocket(fd);
#endif
}

void SocketOperations::shutdown(int fd) {
#ifndef _WIN32
  ::shutdown(fd, SHUT_RDWR);
#else
  ::shutdown(fd, SD_BOTH);
#endif
}

void SocketOperations::freeaddrinfo(addrinfo *ai) {
  return ::freeaddrinfo(ai);
}

int SocketOperations::getaddrinfo(const char *node, const char *service,
                                  const addrinfo *hints, addrinfo **res) {
  return ::getaddrinfo(node, service, hints, res);
}

int SocketOperations::bind(int fd, const struct sockaddr *addr, socklen_t len) {
  return ::bind(fd, addr, len);
}

int SocketOperations::socket(int domain, int type, int protocol) {
  return ::socket(domain, type, protocol);
}

int SocketOperations::setsockopt(int fd, int level, int optname,
                                 const void *optval, socklen_t optlen) {
#ifndef _WIN32
  return ::setsockopt(fd, level, optname, optval, optlen);
#else
  return ::setsockopt(fd, level, optname, reinterpret_cast<const char*>(optval), optlen);
#endif
}

int SocketOperations::listen(int fd, int n) {
  return ::listen(fd, n);
}

} // routing
