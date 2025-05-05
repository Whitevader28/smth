#ifndef __COMMON_H__
#define __COMMON_H__

#include <stddef.h>
#include <stdint.h>

#define MAX_TCP_CONNECTIONS 25
#define MAX_UDP_CONNECTIONS 25
#define MAX_CLIENT_ID_SIZE 256
#define MAX_MSG_SIZE 2031

#define MAX_CONTENT_SIZE 1500

int recv_all(int sockfd, void *buffer, size_t len);
int send_all(int sockfd, void *buffer, size_t len);
void disable_nagle(int socket_fd);


class ChatPacket {
 public:
  uint16_t len;
  char message[MAX_MSG_SIZE + 1];

  void print_messge();
  void print_message();
};

#endif
