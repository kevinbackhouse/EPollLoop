// Copyright 2021-2024 Kevin Backhouse.
//
// This file is part of EPollLoop.
//
// EPollLoop is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// EPollLoop is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with EPollLoop.  If not, see <https://www.gnu.org/licenses/>.

#include "EPollLoop.hpp"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

EPollLoop::EPollLoop() : epollfd_(epoll_create1(0)), numHandlers_(0) {
  if (epollfd_ < 0) {
    throw CreateError();
  }
}

EPollLoop::~EPollLoop() { close(epollfd_); }

EPollLoop::CreateError::CreateError() noexcept : err_(errno) {}

int EPollLoop::add_handler(EPollHandlerInterface *handler) noexcept {
  if (handler->epoll_add(epollfd_) < 0) {
    delete handler;
    return -1;
  }

  ++numHandlers_;
  return handler->init();
}

int EPollLoop::del_handler(EPollHandlerInterface *handler) noexcept {
  // Check if there's a replacement handler for us to swap in.
  EPollHandlerInterface *nexthandler = handler->getNextHandler();
  if (nexthandler) {
    if (handler->epoll_replace(epollfd_, nexthandler) >= 0) {
      // Replacement was successful, so delete the old handler.
      delete handler;
      return nexthandler->init();
    }

    // Replacement failed, so delete both handlers.
    delete nexthandler;
  }

  const int r = handler->epoll_del(epollfd_);
  delete handler;
  --numHandlers_;
  return r;
}

void EPollLoop::run() noexcept {
  while (numHandlers_ > 0) {
    const size_t max_events = 10;
    epoll_event events[max_events];
    const int numevents = epoll_wait(epollfd_, events, max_events, -1);
    int eventidx;
    for (eventidx = 0; eventidx < numevents; eventidx++) {
      epoll_event *ev = &events[eventidx];
      EPollHandlerInterface *handler = (EPollHandlerInterface *)ev->data.ptr;

      if (ev->events & EPOLLIN) {
        if (handler->process_read() < 0) {
          if (del_handler(handler) < 0) {
            break;
          }
          continue;
        }
      }

      if (ev->events & EPOLLOUT) {
        if (handler->process_write() < 0) {
          if (del_handler(handler) < 0) {
            break;
          }
          continue;
        }
      }

      if (ev->events & (EPOLLERR | EPOLLHUP)) {
        if (del_handler(handler) < 0) {
          break;
        }
        continue;
      }
    }
  }
}

int EPollHandlerInterface::epoll_add(const int epollfd) noexcept {
  // Make sure that the socket is non-blocking.
  const int flags = fcntl(sock_, F_GETFL, 0);
  if (fcntl(sock_, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -1;
  }

  epoll_event ev = {};
  ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  ev.data.ptr = (void *)this;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock_, &ev) == -1) {
    return -1;
  }
  return 0;
}

int EPollHandlerInterface::epoll_del(const int epollfd) noexcept {
  if (epoll_ctl(epollfd, EPOLL_CTL_DEL, sock_, 0) == -1) {
    return -1;
  }
  return 0;
}

int EPollHandlerInterface::epoll_replace(
    const int epollfd, EPollHandlerInterface *newhandler) noexcept {
  assert(sock_ == newhandler->sock_);
  epoll_event ev = {};
  ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  ev.data.ptr = (void *)newhandler;
  if (epoll_ctl(epollfd, EPOLL_CTL_MOD, sock_, &ev) == -1) {
    return -1;
  }
  return 0;
}

ssize_t EPollRecvHandlerDatagram::replyto(const void *buf, size_t buflen,
                                          const sockaddr *dest_addr,
                                          socklen_t addrlen) noexcept {
  // TODO: implement buffering for Datagram sends, similar
  // to how it is done in EPollRecvHandlerStream.
  return sendto(sock_, buf, buflen, MSG_NOSIGNAL, dest_addr, addrlen);
}

EPollRecvHandlerDatagram::EPollRecvHandlerDatagram(
    const int sock, std::unique_ptr<RecvHandlerDatagram> &&handler) noexcept
    : EPollHandlerInterface(sock), handler_(std::move(handler)) {}

int EPollRecvHandlerDatagram::process_read() noexcept {
  // Keep reading from the socket until there's no more data.
  while (true) {
    sockaddr_storage peer_addr;
    socklen_t peer_addr_len;
    uint8_t buf[4096];

    peer_addr_len = sizeof(sockaddr_storage);
    const ssize_t recvsize = recvfrom(sock_, buf, sizeof(buf), 0,
                                      (sockaddr *)&peer_addr, &peer_addr_len);

    if (recvsize < 0) {
      return 0;
    }

    const int r = handler_->receive(
        buf, recvsize, *this, (const sockaddr *)&peer_addr, peer_addr_len);
    if (r < 0) {
      return r;
    }
  }
}

RecvBuf::RecvBuf() noexcept : recvbuf_(0), received_(0), remaining_(0) {}

RecvBuf::~RecvBuf() { free(recvbuf_); }

int RecvBuf::reset(size_t size) noexcept {
  uint8_t *newbuf = (uint8_t *)realloc(recvbuf_, size);
  if (newbuf == 0) {
    return -1;
  }
  recvbuf_ = newbuf;
  received_ = 0;
  remaining_ = size;
  return 0;
}

SendBuf::SendBuf() noexcept : sendbuf_(0), remaining_(0), pos_(0) {}

SendBuf::~SendBuf() { free(sendbuf_); }

// This method is called when all the data has been sent. Note: this
// implementation currently frees the buffer, but an alternative
// implementation would be to do nothing. If we didn't free the buffer here
// then it would just get realloc'ed in the next call to
// `SendBuf::append()`.
void SendBuf::reset() noexcept {
  assert(remaining_ == 0);
  free(sendbuf_);
  sendbuf_ = 0;
  pos_ = 0;
}

int SendBuf::append(const void *buf, size_t buflen) noexcept {
  if (buflen == 0) {
    return 0;
  }

  // Shift the remaining bytes to the start of the buffer.
  memmove(sendbuf_, sendbuf_ + pos_, remaining_);
  pos_ = 0;

  // Calculate the new buffer size.
  size_t newsize = 0;
  if (__builtin_add_overflow(remaining_, buflen, &newsize)) {
    return -1;
  }

  // Allocate sufficient space and append the bytes.
  uint8_t *newbuf = (uint8_t *)realloc(sendbuf_, newsize);
  if (newbuf == 0) {
    return -1;
  }
  sendbuf_ = newbuf;
  memcpy(newbuf + remaining_, buf, buflen);
  remaining_ = newsize;
  return 0;
}

EPollRecvHandlerStream::EPollRecvHandlerStream(
    const int sock, std::unique_ptr<RecvHandlerStream> &&handler) noexcept
    : EPollHandlerInterface(sock), handler_(std::move(handler)) {}

int EPollRecvHandlerStream::init() noexcept {
  const ssize_t remaining = handler_->accept(*this);
  if (remaining < 0) {
    return -1;
  }
  return recvbuf_.reset(remaining);
}

int EPollRecvHandlerStream::reply(const void *buf, size_t buflen) noexcept {
  if (sendbuf_.remaining() > 0) {
    // `sendbuf_` isn't empty, which means two things:
    //
    // 1. The remaining bytes in `sendbuf_` need to be sent before we can
    //    send the contents of `buf`.
    // 2. A previous send was incomplete, so we are already waiting for
    //    epoll to notify us that we can resume sending.
    //
    // Therefore, we cannot call `send` right now. We can only append the
    // contents of `buf` to `sendbuf_` and wait for the notification from
    // epoll.
    return sendbuf_.append(buf, buflen);
  } else {
    // First try to send the message directly. If that's unsuccessful,
    // or we only manage to send part of the message, then we'll copy
    // the rest of the message into `sendbuf_` and wait for an EPOLLOUT
    // notification.
    const ssize_t wr = send(sock_, buf, buflen, MSG_NOSIGNAL);
    if (wr < 0) {
      const int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // Copy everything to sendbuf_.
        return sendbuf_.append(buf, buflen);
      } else {
        return -1;
      }
    } else {
      return sendbuf_.append((uint8_t *)buf + wr, buflen - wr);
    }
  }
}

int EPollRecvHandlerStream::process_read() noexcept {
  // Keep reading from the socket until there's no more data.
  while (recvbuf_.remaining() > 0) {
    const ssize_t recvsize =
        recv(sock_, recvbuf_.curr(), recvbuf_.remaining(), 0);

    if (recvsize == 0) {
      // If we received zero bytes, then it means that our peer closed
      // the connection. Since remaining_ > 0, that means that something
      // has gone wrong.
      handler_->disconnect();
      return -1;
    }

    if (recvsize < 0) {
      int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // Need to wait for more input. (We will get a notification from
        // epoll when that happens.)
        return 0;
      }
      handler_->disconnect();
      return -1;
    }

    // Check if we have received all the input.
    recvbuf_.incr(recvsize);
    if (recvbuf_.remaining() == 0) {
      const ssize_t r = handler_->receive(*this, recvbuf_.get());
      if (r < 0) {
        return -1;
      }
      if (r == 0) {
        // We're not interested in reading any more data from the socket.
        // We might still have some data to send, though, so we don't close
        // the socket completely yet.
        shutdown(sock_, SHUT_RD);
      }

      // Get the buffer ready for the next message.
      if (recvbuf_.reset(r) < 0) {
        return -1;
      }
    }
  }
  return 0;
}

int EPollRecvHandlerStream::process_write() noexcept {
  // Keep writing to the socket until either we're done or the socket
  // blocks.
  while (sendbuf_.remaining() > 0) {
    const ssize_t wr =
        send(sock_, sendbuf_.curr(), sendbuf_.remaining(), MSG_NOSIGNAL);

    if (wr < 0) {
      int err = errno;
      if (err == EAGAIN || err == EWOULDBLOCK) {
        // The socket buffer is full, so we need to wait for epoll
        // to tell us that we can resume sending.
        return 0;
      }
      handler_->disconnect();
      return -1;
    }

    sendbuf_.incr(wr);
  }

  // All the data was successfully sent.
  sendbuf_.reset();

  if (recvbuf_.remaining() == 0) {
    // We've finished sending, and there's nothing more to receive, so
    // close the socket.
    return -1;
  }
  return 0;
}

EPollStreamConnectHandler::EPollStreamConnectHandler(
    const int sock, EPollLoop &loop,
    std::unique_ptr<BuildRecvHandlerStream> &&factory) noexcept
    : EPollHandlerInterface(sock), loop_(loop), factory_(std::move(factory)) {}

int EPollStreamConnectHandler::process_read() noexcept {
  class AutoCloseSock {
    int sock_;

  public:
    AutoCloseSock(const int sock) : sock_(sock) {}
    ~AutoCloseSock() { close(sock_); }
    int get() const { return sock_; }
    int release() {
      const int s = sock_;
      sock_ = -1;
      return s;
    }
  };

  // Implements EPollRecvHandlerStream with a destructor that closes
  // the socket file descriptor.
  class Handler : public EPollRecvHandlerStream {
  public:
    Handler(AutoCloseSock &&sock, std::unique_ptr<RecvHandlerStream> &&handler)
        : EPollRecvHandlerStream(sock.release(), std::move(handler)) {}

    ~Handler() { close(sock_); }
  };

  while (true) {
    sockaddr addr;
    socklen_t addr_len = sizeof(addr);
    AutoCloseSock s(accept(sock_, &addr, &addr_len));
    if (s.get() < 0) {
      return 0; // No need to close the listener down.
    }

    try {
      loop_.add_handler(
          new Handler(std::move(s), factory_->build(&addr, addr_len)));
    } catch (...) {
      // Catch any exceptions thrown by `new` or `factory_->build()` and bail
      // out.
      return -1;
    }
  }
  return 0;
}

int EPollStreamConnectHandler::process_write() noexcept {
  // This socket is only for listening, so this method never has to do
  // anything.
  return 0;
}
