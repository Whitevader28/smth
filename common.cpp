#include "common.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

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
      // Conexiune inchisa
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

  while (bytes_remaining > 0) {
    int rc = send(sockfd, (char *)buffer + bytes_sent, bytes_remaining, 0);
    if (rc < 0) {
      perror("send");
      return -1;
    } else if (rc == 0) {
      // Conexiune inchisa
      break;
    }
    bytes_sent += rc;
    bytes_remaining -= rc;
  }

  return bytes_sent;
}

void disable_nagle(int socket_fd) {
  int flag = 1;  // TCP_NODELAY
  int rc = setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag,
                      sizeof(flag));
  if (rc < 0) perror("setsockopt TCP_NODELAY failed");
}