#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <unordered_map>

#include "common.h"
#include "helpers.h"

using namespace std;

void run_client(int socket_fd) {
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, MAX_MSG_SIZE + 1);

  ChatPacket sent_packet;
  ChatPacket recv_packet;

  struct pollfd poll_fds[2];
  poll_fds[0].fd = socket_fd;
  poll_fds[0].events = POLLIN;
  poll_fds[1].fd = STDIN_FILENO;
  poll_fds[1].events = POLLIN;
  poll_fds[1].revents = 0;
  poll_fds[0].revents = 0;

  while (1) {
    int rc = poll(poll_fds, 2, -1);
    DIE(rc < 0, "poll");
    if (poll_fds[0].revents & POLLIN) {
      // Am primit date de la server
      rc = recv_all(socket_fd, &recv_packet, sizeof(recv_packet));
      if (rc <= 0) break;

      if (strcmp(recv_packet.message, "exit") == 0) {
        // cerr << "Server closed connection\n";
        break;
      }

      if (strcmp(recv_packet.message, "EINUSE") == 0) {
        // cerr << "Client already connected\n";
        break;
      }

      printf("%s\n", recv_packet.message);
    }
    if (poll_fds[1].revents & POLLIN) {
      // Am primit date de la tastatura
      // TODO: daca sa elimin sau nu new lines?
      rc = read(STDIN_FILENO, buf, MAX_MSG_SIZE);
      DIE(rc < 0, "read");
      buf[rc - 1] = 0;

      // Close the client if the command is "exit"
      if (strcmp(buf, "exit") == 0) {
        ChatPacket exit_packet;
        exit_packet.len = strlen("exit") + 1;
        strcpy(exit_packet.message, "exit");
        send_all(socket_fd, &exit_packet, sizeof(exit_packet));
        break;
      }

      string input(buf);
      std::size_t end = input.find(' ');
      if (end == std::string::npos) {
        // cerr << "Command not allowed\n";
        continue;
      }

      string first = input.substr(0, end);
      string second = input.substr(end + 1);

      if (second.length() > MAX_MSG_SIZE) {
        // cerr << "Topic too long\n";
        continue;
      }

      if (second.length() == 0) {
        // cerr << "Topic not specified\n";
        continue;
      }

      if (second.find(' ') != string::npos || second[0] == '/' ||
          second[second.length() - 1] == '/') {
        // cerr << "Topic not formatted correctly\n";
        continue;
      }

      if (first.compare("subscribe") == 0) {
        printf("Subscribed to topic ");
      } else if (first.compare("unsubscribe") == 0) {
        printf("Unsubscribed from topic ");
      } else {
        // cerr << "Command not allowed\n";
        continue;
      }
      printf("%s\n", second.c_str());

      sent_packet.len = strlen(buf) + 1;
      strcpy(sent_packet.message, buf);

      // Trimitem pachetul la server.
      send_all(socket_fd, &sent_packet, sizeof(sent_packet));
    }
  }
}

int main(int argc, char *argv[]) {
  int rc = 0;

  setvbuf(stdout, NULL, _IONBF, BUFSIZ);

  if (argc != 4) {
    printf("\n Usage: %s <ID_CLIENT> <IP_SERVER> <PORT_SERVER>\n", argv[0]);
    return 1;
  }

  uint16_t port;
  rc = sscanf(argv[3], "%hu", &port);
  DIE(rc != 1, "Given port is invalid");

  char id[MAX_CLIENT_ID_SIZE];
  rc = sscanf(argv[1], "%s", id);
  DIE(rc != 1, "Given id is invalid");

  // Cream un socket TCP pentru conexiunea cu serverul
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  DIE(socket_fd < 0, "socket");

  // Completam adresa serverului
  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  rc = inet_pton(AF_INET, argv[2], &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton");

  rc = connect(socket_fd, (const sockaddr *)&serv_addr, sizeof(serv_addr));
  DIE(rc < 0, "bind");

  ChatPacket packet;
  packet.len = strlen(id) + 1;
  strcpy(packet.message, id);

  // Trimitem id-ul la server.
  send_all(socket_fd, &packet, sizeof(packet));

  run_client(socket_fd);

  close(socket_fd);
  return 0;
}
