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



enum MessageType {
  MSG_ID = 0,
  MSG_TEXT,
  MSG_SUBSCRIBE,
  MSG_UNSUBSCRIBE,
  MSG_EXIT,
  MSG_ERROR,
  MSG_UNKNOWN
};

union MessagePayload {
  char text[MAX_MSG_SIZE + 1];
  char id[MAX_CLIENT_ID_SIZE];
  // Add more fields as needed for other message types
};

class ChatPacket {
 public:
  uint16_t len; // Length of the payload
  MessageType type;
  MessagePayload payload;

  void print_message();
};

#endif
