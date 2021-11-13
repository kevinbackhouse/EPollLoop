// Copyright 2021 Kevin Backhouse.
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

#pragma once

#include <sys/socket.h>
#include <sys/epoll.h>
#include <memory>

// Base class for handling notifications from epoll.
class EPollHandlerInterface {
  friend class EPollLoop;

protected:
  // The socket file descriptor. Note: this file descriptor is not
  // automatically closed in the destructor. This design enables the
  // overriding class to determine the ownership model for the file
  // descriptor.
  const int sock_;

  EPollHandlerInterface(const int sock) noexcept : sock_(sock) {}

  // Typically, this is overridden with a destructor that calls
  // `close(sock_)`.
  virtual ~EPollHandlerInterface() {}

  // This is called by `EPoll::register()`, after the file descriptor has
  // been registered with epoll. Returns zero on success, otherwise
  // negative.
  virtual int init() noexcept = 0;

  // If the result of `process_read` is negative, it means that the socket
  // should be closed and the epoll handler deleted.
  virtual int process_read() noexcept = 0;

  // If the result of `process_write` is negative, it means that the socket
  // should be closed and the epoll handler deleted.
  virtual int process_write() noexcept = 0;

private:
  // Helper method for `EPollLoop::add_handler()`.
  int epoll_add(const int epollfd) noexcept;

  // Helper method for `EPollLoop::del_handler()`.
  int epoll_del(const int epollfd) noexcept;
};

class EPollLoop final {
  const int epollfd_;
  size_t numHandlers_;

public:
  // Exception thrown by EPollLoop constructor if it fails
  // to initialize epoll.
  class CreateError: public std::exception {
    const int err_;

  public:
    CreateError() noexcept : err_(errno) {}

    const char* what() const noexcept override {
      return "Call to epoll_create1 failed.";
    }

    int getErrno() const noexcept { return err_; }
  };

  // Note: constructor might throw CreateError.
  EPollLoop();

  // Destructor automatically closes `epollfd_`.
  ~EPollLoop();

  // Takes ownership of `handler`. Returns zero on success, otherwise
  // negative.
  int add_handler(EPollHandlerInterface* handler) noexcept;

  // Unregisters `handler` from epoll and calls `delete handler`. This
  // method is called by `run()` when the socket shuts down either
  // gracefully or due to an error. Returns zero on success, otherwise
  // negative.
  int del_handler(EPollHandlerInterface* handler) noexcept;

  // This function loops, listening for epoll events. The loop ends when
  // there are no more handlers registered. You should open the relevant
  // sockets and register them with epoll before calling this function, for
  // example by calling `EPollStreamConnectHandler::build()`.
  void run() noexcept;
};

// Rather than passing the Datagram socket's file descriptor to
// RecvHandlerDatagram, we pass this abstract interface so that we can
// restrict what the RecvHandlerDatagram is able to do with the socket.
class SocketHandlerDatagram {
public:
  virtual ~SocketHandlerDatagram() {}

  // Send a reply. (Wrapper around sendto().)
  virtual ssize_t replyto(
    const void* buf, size_t buflen,
    const sockaddr *dest_addr, socklen_t addrlen
  ) noexcept = 0;
};

class RecvHandlerDatagram {
public:
  // This method is called by `EPollRecvHandlerDatagram::process_read`
  // when a message is received. Return 0 to keep receiving messages,
  // or a negative number to close the socket and delete the epoll
  // handler.
  virtual int receive(
    const void* buf, ssize_t len,
    SocketHandlerDatagram& sock,
    const sockaddr* peer_addr, socklen_t peer_addr_len
  ) noexcept = 0;
};

// Rather than passing the Stream socket's file descriptor to
// RecvHandlerStream, we pass this abstract interface so that we can
// restrict what the RecvHandlerStream is able to do with the socket.
class SocketHandlerStream {
public:
  // Send a reply. (Wrapper around send().) Returns zero on success,
  // otherwise negative. Note: there is no guarantee that the data
  // is sent immediately. If the socket isn't ready then the data
  // might get buffered for sending later.
  virtual int reply(const void* buf, size_t buflen) noexcept = 0;
};

class RecvHandlerStream {
public:
  virtual ~RecvHandlerStream() {};

  // This is method is called immediately after the connection is
  // accepted. The return value is the number of bytes expected on the next
  // message. But if the return value is negative then it means that the
  // socket should be closed and the epoll handler deleted.
  virtual ssize_t accept(SocketHandlerStream& sock) noexcept = 0;

  // `EPollRecvHandlerStream` calls this method when it has received the
  // number of bytes that we asked for. Unlike the corresponding method in
  // `RecvHandlerDatagram`, this method does not need a `len` parameter, because
  // we will get the exact number of bytes that we asked for. (We always
  // have to tell `EPollRecvHandlerStream` how big we are expecting the next
  // message to be.) The return value is the number of bytes expected on
  // the next message. But if the return value is negative then it means
  // that the socket should be closed and the epoll handler deleted.
  virtual ssize_t receive(SocketHandlerStream& sock, const void* buf) noexcept = 0;

  // The socket disconnected before we were finished sending/receiving.
  virtual void disconnect() noexcept = 0;
};

// Factory class for constructing instances of RecvHandlerStream.
class BuildRecvHandlerStream {
public:
  virtual ~BuildRecvHandlerStream() {};

  virtual std::unique_ptr<RecvHandlerStream> build(
    sockaddr* peer_addr, socklen_t peer_addr_len
  ) = 0;
};

// Implementation of EPollHandlerInterface which reads data from
// a file descriptor. The data that was read is passed to the `handler_`
// field for further processing.
class EPollRecvHandlerDatagram :
  public EPollHandlerInterface,
  virtual private SocketHandlerDatagram
{
  // Handler pointer is owned by this class.
  const std::unique_ptr<RecvHandlerDatagram> handler_;

  // Implements SocketHandlerDatagram interface.
  ssize_t replyto(
    const void* buf, size_t buflen,
    const sockaddr *dest_addr, socklen_t addrlen
  ) noexcept override;

public:
  // Takes ownership of `handler`. The ownership model for the `sock` file
  // descriptor is unspecified, as explained in the comments in
  // `EPollHandlerInterface`.
  EPollRecvHandlerDatagram(
    const int sock, std::unique_ptr<RecvHandlerDatagram>&& handler
  ) noexcept;

  int init() noexcept override { return 0; }

  int process_read() noexcept override;

  int process_write() noexcept override {
    // TODO: implement buffering for Datagram sends, similar
    // to how it is done in EPollRecvHandlerStream.
    return 0;
  }
};

// Manages a byte buffer for receiving data from a socket.
class RecvBuf final {
  uint8_t* recvbuf_;  // Buffer for incoming data.
  size_t received_;   // Number of bytes received so far. (Stored in `recvbuf_`.)
  size_t remaining_;  // Number of bytes we are still waiting for.

public:
  RecvBuf() noexcept;
  ~RecvBuf();

  // Reset the buffer so that it is ready to receive `size` bytes. Returns
  // zero on success, otherwise negative.
  int reset(size_t size) noexcept;

  // Get a pointer to the buffer.
  void* get() const noexcept { return recvbuf_; }

  // Get a pointer to the current position in the buffer.
  void* curr() const noexcept { return recvbuf_ + received_; }

  // Update `received_` after receiving data.
  void incr(const ssize_t recvsize) noexcept {
    received_ += recvsize;
    remaining_ -= recvsize;
  }

  // Number of bytes we are still waiting for.
  size_t remaining() const noexcept { return remaining_; }
};

// Manages a byte buffer for sending data to a socket.
class SendBuf final {
  uint8_t* sendbuf_;  // Buffer for outgoing data.
  size_t remaining_;  // Number of bytes remaining to send.
  size_t pos_;        // Number of bytes already sent.

public:
  SendBuf() noexcept;
  ~SendBuf();

  // This method is called when all the data has been sent.
  void reset() noexcept;

  // Number of bytes still left to send.
  size_t remaining() const noexcept { return remaining_; }

  // Append more bytes to the send buffer. (Allocates more memory if
  // necessary.) Returns zero on success, otherwise negative.
  int append(const void* buf, size_t buflen) noexcept;

  // Get a pointer to the current position in the buffer.
  void* curr() const noexcept { return sendbuf_ + pos_; }

  // Update `pos_` after receiving data.
  void incr(const ssize_t sendsize) noexcept {
    pos_ += sendsize;
    remaining_ -= sendsize;
  }
};

// Implementation of EPollHandlerInterface which reads data from
// a file descriptor. The data that was read is passed to the `handler_`
// field for further processing.
class EPollRecvHandlerStream :
  public EPollHandlerInterface,
  virtual private SocketHandlerStream
{
  // Handler pointer is owned by this class.
  const std::unique_ptr<RecvHandlerStream> handler_;

  RecvBuf recvbuf_;
  SendBuf sendbuf_;

  // This method is called after the class has been constructed and
  // registered with epoll. Returns zero on success, otherwise negative.
  int init() noexcept override;

  // Implements SocketHandlerStream interface.
  int reply(const void* buf, size_t buflen) noexcept override;

public:
  // Takes ownership of `handler`. The ownership model for the `sock` file
  // descriptor is unspecified, as explained in the comments in
  // `EPollHandlerInterface`.
  EPollRecvHandlerStream(
    const int sock, std::unique_ptr<RecvHandlerStream>&& handler
  ) noexcept;

  int process_read() noexcept override;
  int process_write() noexcept override;
};

// Implementation of EPollHandlerInterface which handles Stream connection
// request.
class EPollStreamConnectHandler : public EPollHandlerInterface {
  // Reference to the EPoll object that owns this handler.
  // TODO: pass this as an argument to process_read/process_write instead?
  EPollLoop& loop_;

  // Factory pointer is owned by this class.
  std::unique_ptr<BuildRecvHandlerStream> factory_;

public:
  // Keeps a reference to `loop` but does not own it. Takes ownership of
  // `factory`. The ownership model for the `sock` file descriptor is
  // unspecified, as explained in the comments in `EPollHandlerInterface`.
  EPollStreamConnectHandler(
    const int sock, EPollLoop& loop,
    std::unique_ptr<BuildRecvHandlerStream>&& factory
  ) noexcept;

  int process_read() noexcept override;
  int process_write() noexcept override;
};
