#ifndef __COMMON_H__
#define __COMMON_H__

#include <stddef.h>
#include <stdint.h>

#define MAX_TCP_CONNECTIONS 25
#define MAX_UDP_CONNECTIONS 25
#define MAX_CLIENT_ID_SIZE 256
#define MAX_MSG_SIZE 2031

#define MAX_CONTENT_SIZE 1500
#define MAX_UDP_MSG_SIZE 1600

int recv_all(int sockfd, void *buffer, size_t len);
int send_all(int sockfd, void *buffer, size_t len);
void disable_nagle(int socket_fd);
bool set_nonblocking(int sockfd);
int recv_nonblocking(int sockfd, void *buffer, size_t max_len);
int send_nonblocking(int sockfd, const void *buffer, size_t len);

enum MessageType {
  MSG_ID = 0,
  MSG_TEXT,
  MSG_SUBSCRIBE,
  MSG_UNSUBSCRIBE,
  MSG_EXIT,
  MSG_ERROR,
  MSG_UNKNOWN,
  MSG_UDP_FORWARD
};

union MessagePayload {
  char text[MAX_MSG_SIZE + 1];
  char id[MAX_CLIENT_ID_SIZE];
};

class ChatPacket {
 public:
  uint16_t len;
  MessageType type;
  MessagePayload payload;

  void print_message();
};

#endif // __COMMON_H__
