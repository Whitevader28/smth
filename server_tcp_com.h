#ifndef __TCP_COM_H__
#define __TCP_COM_H__

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <set>
#include <iostream>

#include "common.h"

#define UDP_SOCKET 0
#define TCP_SOCKET 1

using namespace std;

int receive_from_client(int socket_fd, ChatPacket &packet);
int send_to_client(int socket_fd, ChatPacket &packet);

int create_tcp_listenfd(uint16_t port, const char *ip);
int create_udp_listenfd(uint16_t port, const char *ip);

class ClientTCP {
 public:
  enum State { CONNECTED, DISCONNECTED };
  int socketfd;
  char id[MAX_CLIENT_ID_SIZE];
  State state = CONNECTED;

  void print() {
    cout << "Client id: " << id << endl;
    cout << "Socket fd: " << socketfd << endl;
    cout << "State: " << (state == CONNECTED ? "CONNECTED" : "DISCONNECTED") << endl;
  }
};

class ClientConnections {
 public:
  // Se adauga si STD_IN_FD, listen_fd_tcp si listen_fd_udp in plus
  struct pollfd poll_fds[MAX_TCP_CONNECTIONS + 2] = {0};
  int listenfd_tcp = -1, listenfd_udp = -1, stdinfd = -1, num_fds = 0;

  // Folosit pentru a verifica daca un client TCP este deja conectat
  unordered_map<string, shared_ptr<ClientTCP>> client_tcp_by_id;
  unordered_map<int, shared_ptr<ClientTCP>> client_tcp_by_fd;

  // Folosit pentru a verifica ce tip de client este
  // TCP sau UDP
  unordered_map<int, int> fd_to_type;
  unordered_map<string, set<shared_ptr<const ClientTCP>>> topic_to_clients;

  ClientConnections(int tcp_fd, int udp_fd) {
    listenfd_tcp = tcp_fd;
    listenfd_udp = udp_fd;
    stdinfd = STDIN_FILENO;

    add_fd(listenfd_tcp, POLLIN);
    add_fd(listenfd_udp, POLLIN);
    add_fd(STDIN_FILENO, POLLIN);

    fd_to_type[listenfd_tcp] = TCP_SOCKET;
    fd_to_type[listenfd_udp] = UDP_SOCKET;
  }

  /**
   * Sends an "exit" message to all the tcp clients
   */
  void onExit();
  void remove_fd_by_pos(int pos);
  void add_fd(int fd, short int events);

  /**
   * Poll all file descriptors and act on each event
   */
  int pollAll();

 private:
  void connect_tcp_client();
  void disconnect_tcp_client(pollfd curr);

  /**
   * Gestionare mesajele de la clientii TCP
   * @param curr - file descriptor-ul clientului
   * @return 1 daca clientul s-a deconectat
   *         0 daca clientul este inca conectat
   */
  int handle_tcp_message(pollfd curr);
  void handle_udp_message(pollfd curr);

  void subscribe_to_topic(pollfd curr, string topic);
  void unsubscribe_from_topic(pollfd curr, string topic);

  int handle_stdin();
};

#endif
