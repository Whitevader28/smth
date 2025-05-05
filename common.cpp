#include "common.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <fcntl.h> // Required for fcntl
#include <sys/types.h>
#include <unistd.h>
#include <errno.h> // Required for errno
#include <string.h> // Required for memcpy

#include <iostream>
#include <vector>

/**
 * @brief Attempts to receive data from a non-blocking socket.
 *
 * Reads up to `max_len` bytes into the provided buffer. Handles EAGAIN/EWOULDBLOCK.
 *
 * @param sockfd The non-blocking socket file descriptor.
 * @param buffer The buffer to read data into.
 * @param max_len The maximum number of bytes to read.
 * @return Number of bytes received, 0 on connection closed, -1 on error (excluding EAGAIN/EWOULDBLOCK), -2 on EAGAIN/EWOULDBLOCK.
 */
int recv_nonblocking(int sockfd, void *buffer, size_t max_len) {
    ssize_t bytes_received = recv(sockfd, buffer, max_len, 0);

    if (bytes_received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -2; // Indicate non-blocking call would block
        }
        perror("recv");
        return -1; // Genuine error
    } else if (bytes_received == 0) {
        // Connection closed by peer
        return 0;
    }

    return (int)bytes_received;
}


/**
 * @brief Attempts to send data on a non-blocking socket.
 *
 * Tries to send `len` bytes from the buffer. Handles EAGAIN/EWOULDBLOCK.
 *
 * @param sockfd The non-blocking socket file descriptor.
 * @param buffer The buffer containing data to send.
 * @param len The number of bytes to attempt to send.
 * @return Number of bytes actually sent, 0 if EAGAIN/EWOULDBLOCK occurred immediately, -1 on error.
 */
int send_nonblocking(int sockfd, const void *buffer, size_t len) {
    ssize_t bytes_sent = send(sockfd, buffer, len, 0);

    if (bytes_sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // The send buffer is full, cannot send more *right now*
            return 0; // Indicate non-blocking call would block (or sent 0 bytes)
        }
        perror("send");
        return -1; // Genuine error
    }
    // Note: send() returning 0 is unusual for a connected TCP socket,
    // but we treat it as 0 bytes sent if it happens.

    return (int)bytes_sent;
}


// --- Kept for reference or potential use by subscriber, but server should use non-blocking ---
// WARNING: This is a blocking receive function. Use with caution in event loops.
int recv_all(int sockfd, void *buffer, size_t len) {
  size_t bytes_received = 0;
  size_t bytes_remaining = len;
  char *buf_ptr = (char *)buffer;

  while (bytes_remaining > 0) {
    // Use MSG_WAITALL to simplify blocking receive if supported and desired
    // int rc = recv(sockfd, buf_ptr + bytes_received, bytes_remaining, MSG_WAITALL);
    int rc = recv(sockfd, buf_ptr + bytes_received, bytes_remaining, 0);

    if (rc < 0) {
      perror("recv");
      return -1; // Error
    } else if (rc == 0) {
      // Connection closed before receiving all requested data
      return bytes_received; // Return partial data received
    }
    bytes_received += rc;
    bytes_remaining -= rc;
  }

  return bytes_received; // Should be equal to len if successful
}

// WARNING: This is a blocking send function (original logic modified slightly).
// It's generally NOT recommended for the server-side event loop.
// The subscriber might still use it.
int send_all(int sockfd, void *buffer, size_t len) {
  size_t bytes_sent = 0;
  size_t bytes_remaining = len;
  char *buf_ptr = (char *)buffer;

  while (bytes_remaining > 0) {
    // Use blocking send directly
    int rc = send(sockfd, buf_ptr + bytes_sent, bytes_remaining, 0);

    if (rc < 0) {
        perror("send");
        return -1; // Error
    } else if (rc == 0) {
        // This is unusual for a blocking TCP send on a connected socket,
        // could indicate an issue or closed connection.
        fprintf(stderr, "send returned 0 unexpectedly in blocking send_all.\n");
        return bytes_sent; // Return partial bytes sent
    }
    bytes_sent += rc;
    bytes_remaining -= rc;
  }

  return bytes_sent; // Should be equal to len if successful
}
// --- End of potentially blocking functions ---


void disable_nagle(int socket_fd) {
  int flag = 1;  // Value for TCP_NODELAY is 1
  int rc = setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                      sizeof(flag));
  if (rc < 0) {
      perror("setsockopt(TCP_NODELAY) failed");
      // Depending on the application, this might be a non-critical error.
  } else {
      // std::cerr << "Nagle disabled for fd " << socket_fd << std::endl;
  }
}

// Helper to set a socket to non-blocking mode
bool set_nonblocking(int sockfd) {
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL)");
        return false;
    }
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL, O_NONBLOCK)");
        return false;
    }
    return true;
}
