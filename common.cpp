#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <vector>

int recv_nonblocking(int sockfd, void *buffer, size_t max_len) {
  ssize_t bytes_received = recv(sockfd, buffer, max_len, 0);

  if (bytes_received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;

    perror("recv");
    return -1;
  } else if (bytes_received == 0) {
    // Connection closed by server
    return 0;
  }

  return (int)bytes_received;
}

int send_nonblocking(int sockfd, const void *buffer, size_t len) {
  ssize_t bytes_sent = send(sockfd, buffer, len, 0);

  if (bytes_sent < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;

    return -1;
  }

  return (int)bytes_sent;
}

// Blocking receive
int recv_all(int sockfd, void *buffer, size_t len) {
  size_t bytes_received = 0;
  size_t bytes_remaining = len;
  char *buf_ptr = (char *)buffer;

  while (bytes_remaining > 0) {
    int rc = recv(sockfd, buf_ptr + bytes_received, bytes_remaining, 0);

    if (rc < 0) {
      perror("recv");
      return -1;  // Error
    } else if (rc == 0) {
      // Connection closed
      return bytes_received;
    }
    bytes_received += rc;
    bytes_remaining -= rc;
  }

  return bytes_received;  // Should be equal to len if successful
}

// Blocking send
int send_all(int sockfd, void *buffer, size_t len) {
  size_t bytes_sent = 0;
  size_t bytes_remaining = len;
  char *buf_ptr = (char *)buffer;

  while (bytes_remaining > 0) {
    int rc = send(sockfd, buf_ptr + bytes_sent, bytes_remaining, 0);

    if (rc < 0) {
      perror("send");
      return -1;  // Error
    } else if (rc == 0) {
      fprintf(stderr, "send returned 0 unexpectedly in blocking send_all.\n");
      return bytes_sent;
    }
    bytes_sent += rc;
    bytes_remaining -= rc;
  }

  return bytes_sent;  // Should be equal to len if successful
}

void disable_nagle(int socket_fd) {
  int flag = 1;
  int rc = setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                      sizeof(flag));
  if (rc < 0) perror("setsockopt(TCP_NODELAY) failed");
}

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
