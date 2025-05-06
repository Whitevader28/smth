#ifndef _SERVER_TCP_COM_H
#define _SERVER_TCP_COM_H

#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"

using namespace std;

int create_tcp_listenfd(uint16_t port, const char* ip);
int create_udp_listenfd(uint16_t port, const char* ip);

// Structure to hold client state including buffers
struct ClientState {
  int socketfd = -1;
  string id;
  bool connected = false;
  vector<char> recv_buffer;        // Buffer for partially received messages
  deque<vector<char>> send_queue;  // Queue of messages waiting to be sent
  size_t current_send_offset = 0;
  unordered_set<string> subscriptions;  // Subscribed topics / patterns

  ClientState(int fd = -1, string client_id = "")
      : socketfd(fd), id(client_id) {}
};

// Enum for socket types in poll array
enum SocketType {
  STDIN_SOCKET,
  TCP_LISTEN_SOCKET,
  UDP_LISTEN_SOCKET,
  TCP_CLIENT_SOCKET
};

class ClientConnections {
 private:
  int listenfd_tcp;
  int listenfd_udp;
  int stdinfd;
  // +3 for TCP listen, UDP listen, stdin
  struct pollfd poll_fds[MAX_TCP_CONNECTIONS + 3];
  int num_fds;

  unordered_map<int, shared_ptr<ClientState>> client_by_fd;
  unordered_map<string, shared_ptr<ClientState>> client_by_id;
  unordered_map<int, SocketType> fd_to_type;
  unordered_map<string, unordered_set<string>> topic_subscribers;

  void add_fd(int fd, short int events, SocketType type);
  void remove_fd(int fd);
  int find_poll_index(int fd);

  void connect_tcp_client();
  int handle_stdin();
  void handle_udp_message(int udp_fd);
  void handle_client_read(shared_ptr<ClientState> client);
  void handle_client_write(shared_ptr<ClientState> client);
  void process_received_data(shared_ptr<ClientState> client);
  void process_tcp_command(shared_ptr<ClientState> client,
                           const ChatPacket& packet);

  void send_message_to_client(
      shared_ptr<ClientState> client,
      const ChatPacket& packet);  // Queues a message for sending
  void broadcast_udp_message(const string& udp_topic,
                             const ChatPacket& tcp_packet);
  void subscribe_client_to_topic(shared_ptr<ClientState> client,
                                 const string& topic_pattern);
  void unsubscribe_client_from_topic(shared_ptr<ClientState> client,
                                     const string& topic_pattern);
  void remove_client_subscriptions(shared_ptr<ClientState> client);

  void disconnect_tcp_client(int fd);
  void onExit();  // Sends exit message to all connected clients

  // Wildcard matching helper
  bool match_topic(const string& topic, const string& pattern);

 public:
  ClientConnections(int tcp_sock, int udp_sock)
      : listenfd_tcp(tcp_sock),
        listenfd_udp(udp_sock),
        stdinfd(STDIN_FILENO),
        num_fds(0) {
    memset(poll_fds, 0, sizeof(poll_fds));

    add_fd(listenfd_tcp, POLLIN, TCP_LISTEN_SOCKET);
    add_fd(listenfd_udp, POLLIN, UDP_LISTEN_SOCKET);
    add_fd(stdinfd, POLLIN, STDIN_SOCKET);
  }

  int pollAll();  // Main event loop using poll
};

#endif  // _SERVER_TCP_COM_H
