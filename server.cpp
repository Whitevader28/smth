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
#include "server_tcp_com.h"

#define IP_DEFAULT_ROUTE "0.0.0.0"

using namespace std;

unordered_map<int, int> client_id_to_port;

void run_multiplexed(int listenfd_tcp, int listenfd_udp) {
  ClientConnections connections(listenfd_tcp, listenfd_udp);
  int rc;

  rc = listen(listenfd_tcp, MAX_TCP_CONNECTIONS);
  DIE(rc < 0, "listen");

  while (1) {
    // Verificam daca a fost tastat "exit" la stdin
    if (connections.pollAll()) {
      break;
    }
  }
}

int main(int argc, char *argv[]) {
  int rc = 0;

  setvbuf(stdout, NULL, _IONBF, BUFSIZ);

  if (argc != 2) {
    printf("\n Usage: %s <port>\n", argv[0]);
    return 1;
  }

  uint16_t port;
  rc = sscanf(argv[1], "%hu", &port);
  DIE(rc != 1, "Given port is invalid");

  // Cream un socket TCP pentru conexiunea cu un client
  int listenfd_tcp = create_tcp_listenfd(port, IP_DEFAULT_ROUTE);
  int listendfd_udp = create_udp_listenfd(port, IP_DEFAULT_ROUTE);

  run_multiplexed(listenfd_tcp, listendfd_udp);

  close(listendfd_udp);
  close(listenfd_tcp);

  return 0;
}
