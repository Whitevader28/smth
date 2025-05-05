#include "common.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <fcntl.h> // Required for fcntl
#include <sys/types.h>
#include <unistd.h>
#include <errno.h> // Required for errno

#include <iostream>

int recv_all(int sockfd, void *buffer, size_t len) {
  size_t bytes_received = 0;
  size_t bytes_remaining = len;

  while (bytes_remaining) {
    int rc = recv(sockfd, (char *)buffer + bytes_received, bytes_remaining, 0);
    if (rc < 0) {
      perror("recv");
      return -1;
    } else if (rc == 0) {
      // Connection closed
      break;
    }
    bytes_received += rc;
    bytes_remaining -= rc;
  }

  return bytes_received;
}

int send_all(int sockfd, void *buffer, size_t len) {
  size_t bytes_sent = 0;
  size_t bytes_remaining = len;

  // Get current flags
  int flags = fcntl(sockfd, F_GETFL, 0);
  if (flags == -1) {
      perror("fcntl F_GETFL");
      return -1; // Or handle error appropriately
  }

  // Set non-blocking
  if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
      perror("fcntl F_SETFL O_NONBLOCK");
      // Consider trying to restore original flags if possible
      return -1; // Or handle error appropriately
  }

  while (bytes_remaining > 0) {
    int rc = send(sockfd, (char *)buffer + bytes_sent, bytes_remaining, 0);
    if (rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // Buffer is full, stop trying for now
        break;
      }
      perror("send"); // Genuine error
      bytes_sent = -1; // Indicate error
      break;
    } else if (rc == 0) {
      // Connection closed? Or unexpected for TCP send.
      // Indicate error or treat as buffer full? For simplicity, break.
      fprintf(stderr, "send returned 0 unexpectedly.\n");
      bytes_sent = -1; // Indicate error
      break;
    }
    bytes_sent += rc;
    bytes_remaining -= rc;
  }

  // Restore original flags
  if (fcntl(sockfd, F_SETFL, flags) == -1) {
      perror("fcntl F_SETFL restore flags");
      // Function might have already partially succeeded, but flag restoration failed.
      // Decide how to handle this. Returning current bytes_sent might be reasonable.
      // If bytes_sent was already set to -1 due to another error, keep it -1.
  }

  // If no bytes were sent due to EAGAIN/EWOULDBLOCK, bytes_sent is 0.
  // If a real error occurred, bytes_sent is -1.
  // Otherwise, bytes_sent contains the number of bytes sent.
  return bytes_sent;
}


void disable_nagle(int socket_fd) {
  int flag = 1;  // TCP_NODELAY
  int rc = setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                      sizeof(flag));
  if (rc < 0) perror("setsockopt TCP_NODELAY failed");
}