#include "server_tcp_com.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <iostream>
#include <memory>
#include <unordered_set>
#include <string>
#include <vector>
#include <sstream>

#include "common.h"
#include "helpers.h"

// "123.456.789.012:65535 - " - exemplu de prefix maximal
#define MAX_PREFIX_LEN 25

int receive_from_client(int socket_fd, ChatPacket &packet) {
  int rc = 0;

  rc = recv_all(socket_fd, &packet, sizeof(packet));
  if (rc == 0) return 0;
  DIE(rc < 0, "recv");

  return rc;
}

int send_to_client(int socket_fd, ChatPacket &packet) {
  int rc = 0;

  rc = send_all(socket_fd, &packet, sizeof(packet));
  if (rc <= 0) {
    perror("send_all");
    return -1;
  }

  return rc;
}

int create_tcp_listenfd(uint16_t port, const char *ip) {
  const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  DIE(listenfd < 0, "socket");

  // Completam adresa serverului
  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  int rc = inet_pton(AF_INET, ip, &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton");

  rc = bind(listenfd, (const sockaddr *)&serv_addr, sizeof(serv_addr));
  DIE(rc < 0, "bind");

  disable_nagle(listenfd);

  return listenfd;
}

int create_udp_listenfd(uint16_t port, const char *ip) {
  const int listenfd = socket(AF_INET, SOCK_DGRAM, 0);
  DIE(listenfd < 0, "socket");

  // Completam adresa serverului
  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  int rc = inet_pton(AF_INET, ip, &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton");

  rc = bind(listenfd, (const sockaddr *)&serv_addr, sizeof(serv_addr));
  DIE(rc < 0, "bind");

  return listenfd;
}

void ClientConnections::onExit() {
  ChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.len = strlen("exit") + 1;
  strcpy(packet.message, "exit");

  for (int i = 0; i < num_fds; ++i) {
    struct pollfd curr = poll_fds[i];
    if (curr.fd != listenfd_tcp && curr.fd != stdinfd &&
        curr.fd != listenfd_udp)
      send_to_client(curr.fd, packet);
  }
}

void ClientConnections::remove_fd_by_pos(int pos) {
  fd_to_type.erase(poll_fds[pos].fd);
  for (int j = pos; j < num_fds - 1; j++) poll_fds[j] = poll_fds[j + 1];
  --num_fds;
}

void ClientConnections::add_fd(int fd, short int events) {
  poll_fds[num_fds].fd = fd;
  poll_fds[num_fds].events = events;
  num_fds++;
}

void ClientConnections::connect_tcp_client() {
  ChatPacket packet_from_tcp;
  shared_ptr<ClientTCP> client = make_shared<ClientTCP>();
  int rc;
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  const int newsockfd =
      accept(listenfd_tcp, (struct sockaddr *)&client_addr, &client_len);
  DIE(newsockfd < 0, "accept");

  // Dezactivam algoritmul Nagle
  disable_nagle(newsockfd);

  rc = receive_from_client(newsockfd, packet_from_tcp);
  DIE(rc < 0, "recv");

  if (rc == 0) {
    // Clientul a inchis conexiunea inainte de a trimite id-ul
    close(newsockfd);
  } else {
    // Primul mesaj de la client este id-ul
    char *client_id = packet_from_tcp.message;

    // Id ul exista deja
    if (client_tcp_by_id.find(client_id) != client_tcp_by_id.end()) {
      shared_ptr<ClientTCP> client = client_tcp_by_id[client_id];

      if (client->state == ClientTCP::CONNECTED) {
        // Clientul este deja conectat trimitem un mesaj de eroare
        ChatPacket packet;
        packet.len = strlen("EINUSE") + 1;
        strcpy(packet.message, "EINUSE");

        send_to_client(newsockfd, packet);
        printf("Client %s already connected.\n", client_id);

        close(newsockfd);
      } else {
        // Clientul s-a reconectat
        printf("New client %s connected from %s:%hu.\n", client_id,
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        client->socketfd = newsockfd;
        client->state = ClientTCP::CONNECTED;
        client_tcp_by_fd[newsockfd] = client;
        fd_to_type[newsockfd] = TCP_SOCKET;

        add_fd(newsockfd, POLLIN);
      }
    } else {
      // Clientul este nou
      client->socketfd = newsockfd;
      client->state = ClientTCP::CONNECTED;
      strcpy(client->id, client_id);
      client_tcp_by_id[client_id] = client;
      client_tcp_by_fd[newsockfd] = client;

      fd_to_type[newsockfd] = TCP_SOCKET;
      add_fd(newsockfd, POLLIN);

      printf("New client %s connected from %s:%hu.\n", client_id,
             inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }
  }
}

/**
 * Verificam doar mesajul "exit" de la stdin
 */
int ClientConnections::handle_stdin() {
  int rc;
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, MAX_MSG_SIZE + 1);
  // TODO: ar trebui sa nu presupun ca exist \n in input?
  rc = read(STDIN_FILENO, buf, MAX_MSG_SIZE);
  DIE(rc < 0, "read");
  buf[rc - 1] = 0;

  if (strcmp(buf, "exit") == 0) {
    onExit();
    return 1;
  }

  return 0;
}

// Helper function to split a string by a delimiter
std::vector<std::string> split(const std::string &s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (std::getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

// // Recursive helper function to perform the wildcard matching
// bool isMatch(const std::vector<std::string> &topic_levels,
//              const std::vector<std::string> &pattern_levels,
//              long unsigned int topic_idx,
//              long unsigned int pattern_idx) {

//     // Base Case 1: If the pattern is exhausted
//     if (pattern_idx == pattern_levels.size()) {
//         // Match only if the topic is also exhausted
//         return topic_idx == topic_levels.size();
//     }

//     // Base Case 2: If the topic is exhausted, but the pattern is not
//     if (topic_idx == topic_levels.size()) {
//         // Match only if all remaining pattern levels are '*'
//         for (long unsigned int i = pattern_idx; i < pattern_levels.size(); ++i) {
//             if (pattern_levels[i] != "*") {
//                 return false;
//             }
//         }
//         return true;
//     }

//     // Get the current pattern level
//     const std::string &current_pattern_level = pattern_levels[pattern_idx];

//     // Handle the '+' wildcard: matches exactly one level
//     if (current_pattern_level == "+") {
//         // Must have a topic level to match, and then continue matching the rest
//         return isMatch(topic_levels, pattern_levels, topic_idx + 1, pattern_idx + 1);
//     }
//     // Handle the '*' wildcard: matches zero or more levels
//     else if (current_pattern_level == "*") {
//         // Option 1: '*' matches zero levels. Move to the next pattern level.
//         if (isMatch(topic_levels, pattern_levels, topic_idx, pattern_idx + 1)) {
//             return true;
//         }
//         // Option 2: '*' matches one or more levels. Consume a topic level and stay at the '*' pattern level.
//         // This option is only possible if there are remaining topic levels.
//         if (topic_idx < topic_levels.size() && isMatch(topic_levels, pattern_levels, topic_idx + 1, pattern_idx)) {
//              return true;
//         }
//         // If neither option leads to a match, return false
//         return false;
//     }
//     // Handle a regular level: must match the current topic level exactly
//     else {
//         // Must have a topic level, and it must match the pattern level
//         if (topic_levels[topic_idx] == current_pattern_level) {
//             // If they match, continue matching the rest of the topic and pattern
//             return isMatch(topic_levels, pattern_levels, topic_idx + 1, pattern_idx + 1);
//         } else {
//             // If they don't match, it's not a valid match
//             return false;
//         }
//     }
// }

// // Main function to match a topic string against a pattern string with wildcards
// bool match_topic(const std::string &topic, const std::string &pattern) {
//     // Split the topic and pattern into levels using '/' as the delimiter
//     std::vector<std::string> topic_levels = split(topic, '/');
//     std::vector<std::string> pattern_levels = split(pattern, '/');

//     // Start the recursive matching process from the beginning of both level lists
//     return isMatch(topic_levels, pattern_levels, 0, 0);
// }

bool match_topic(const string &topic, const string &pattern) {
  if (topic == pattern) return true;
  return false;
}

void ClientConnections::handle_udp_message(pollfd curr) {
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, MAX_MSG_SIZE + 1);

  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(client_addr));
  socklen_t client_len = sizeof(client_addr);

  int rc = recvfrom(curr.fd, buf, MAX_MSG_SIZE, 0,
                    (struct sockaddr *)&client_addr, &client_len);
  DIE(rc < 0, "recvfrom");

  ChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.len = MAX_PREFIX_LEN + strlen(buf) + 1;
  sprintf(packet.message, "%s:%hu - ", inet_ntoa(client_addr.sin_addr),
          ntohs(client_addr.sin_port));

  // Adaugam topicul la mesaj
  strcat(packet.message, buf);

  // Adaugam tipul de date si mesajul
  char tmp[256];
  uint32_t base = 0;
  uint8_t power = 0;
  int i = 0;
  switch (buf[50]) {
    case 0:
      strcat(packet.message, " - INT - ");
      if (buf[51] == 1) strcat(packet.message, "-");

      sprintf(tmp, "%u", ntohl(*(uint32_t *)(buf + 52)));
      strcat(packet.message, tmp);
      break;
    case 1:
      strcat(packet.message, " - SHORT_REAL - ");

      sprintf(tmp, "%.2f", ntohs(*(uint16_t *)(buf + 51)) / 100.0);
      strcat(packet.message, tmp);
      break;
    case 2:
      strcat(packet.message, " - FLOAT - ");
      if (buf[51] == 1) strcat(packet.message, "-");

      base = ntohl(*(uint32_t *)(buf + 52));
      power = *(uint8_t *)(buf + 52 + 4);
      sprintf(tmp, "%f", base / pow(10.0, power));

      // Eliminam trailing 0s de la finalul numarului
      i = strlen(tmp) - 1;
      while (i >= 0 && tmp[i] == '0') {
        tmp[i] = 0;
        i--;
      }

      strcat(packet.message, tmp);
      break;
    case 3:
      strcat(packet.message, " - STRING - ");
      strncat(packet.message, buf + 51, MAX_CONTENT_SIZE);
      break;
    default:
      cerr << "Unknown type\n";
      return;
  }

  cerr << "Message: " << packet.message << endl;

  unordered_set<shared_ptr<const ClientTCP>> clients_notified;

  for (const auto &topic : topic_to_clients) {
    if (match_topic(topic.first, buf)) {
      // Trimitem mesajul la toti clientii care sunt abonati si activi si nu au
      // primit deja pe topicul respectiv
      for (const auto &client : topic.second) {
        if (client->state == ClientTCP::CONNECTED &&
            clients_notified.find(client) == clients_notified.end()) {
          send_to_client(client->socketfd, packet);
          clients_notified.insert(client);
        }
      }
    }
  }
}

int ClientConnections::handle_tcp_message(pollfd curr) {
  ChatPacket packet_from_client;
  memset(&packet_from_client, 0, sizeof(packet_from_client));

  // Try to receive data with non-blocking socket first to check if there's
  // really data
  int flags = fcntl(curr.fd, F_GETFL, 0);
  fcntl(curr.fd, F_SETFL, flags | O_NONBLOCK);

  int rc =
      recv(curr.fd, &packet_from_client, sizeof(packet_from_client), MSG_PEEK);

  // Restore blocking mode
  fcntl(curr.fd, F_SETFL, flags);

  if (rc == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    // False positive from poll - no actual data to read
    cerr << "No actual data available from client " << curr.fd << endl;
    return 0;
  }

  rc = receive_from_client(curr.fd, packet_from_client);
  DIE(rc < 0, "recv");

  // Clientul a inchis conexiunea
  if (rc == 0 || strcmp(packet_from_client.message, "exit") == 0) return 1;

  string input(packet_from_client.message);
  std::size_t end = input.find(' ');
  if (end == std::string::npos) {
    cerr << "Command not allowed\n";
    return 0;
  }

  string first = input.substr(0, end);
  string second = input.substr(end + 1);

  // Topicul nu este formatat corect
  if (second.length() == 0 || second.find(' ') != string::npos ||
      second[0] == '/' || second[second.length() - 1] == '/') {
    cerr << "Topic not formatted correctly\n";
    return 0;
  }

  if (first.compare("subscribe") == 0) {
    // TODO: REMOVE THIS
    if (strcmp(second.c_str(), "getAll") == 0) {
      // Print all  the user is subscribed to
      cerr << "Subscribed to topics: \n";
      for (const auto &topic : topic_to_clients) {
        if (topic.second.find(client_tcp_by_fd[curr.fd]) !=
            topic.second.end()) {
          cerr << "   " << topic.first << "\n";
        }
      }
      return 0;
    }
    subscribe_to_topic(curr, second);
  } else if (first.compare("unsubscribe") == 0) {
    unsubscribe_from_topic(curr, second);
  } else {
    cerr << "Command not allowed\n";
    return 0;
  }

  return 0;
}

void ClientConnections::subscribe_to_topic(pollfd curr, string topic) {
  topic_to_clients[topic].insert(client_tcp_by_fd[curr.fd]);
  cerr << "Client " << client_tcp_by_fd[curr.fd]->id
       << " subscribed to topic: " << topic << endl;
}

void ClientConnections::unsubscribe_from_topic(pollfd curr, string topic) {
  assert(client_tcp_by_fd.find(curr.fd) != client_tcp_by_fd.end());

  auto it = topic_to_clients.find(topic);
  shared_ptr<ClientTCP> client = client_tcp_by_fd[curr.fd];

  if (it == topic_to_clients.end()) {
    cerr << "Client " << client_tcp_by_fd[curr.fd]->id
         << " not subscribed to topic: " << topic << endl;
    return;
  }

  // Stergem clientul din lista de clienti
  auto &clients = it->second;
  clients.erase(client);

  // Daca nu mai sunt clienti pe topic, il stergem
  if (clients.empty()) topic_to_clients.erase(it);

  cerr << "Client " << client_tcp_by_fd[curr.fd]->id
       << " unsubscribed from topic: " << topic << endl;
}

void ClientConnections::disconnect_tcp_client(pollfd curr) {
  auto it = fd_to_type.find(curr.fd);
  assert(it != fd_to_type.end() && it->second == TCP_SOCKET);

  printf("Client %s disconnected.\n", client_tcp_by_fd[curr.fd]->id);
  client_tcp_by_fd[curr.fd]->state = ClientTCP::DISCONNECTED;
  client_tcp_by_fd[curr.fd]->socketfd = -1;
  client_tcp_by_fd.erase(curr.fd);
  fd_to_type.erase(curr.fd);

  close(curr.fd);
}

int ClientConnections::pollAll() {
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, MAX_MSG_SIZE + 1);

  int rc = poll(poll_fds, num_fds, -1);
  DIE(rc < 0, "poll");

  for (int i = 0; i < num_fds; ++i) {
    pollfd curr = poll_fds[i];
    if (curr.revents & POLLIN) {
      if (curr.fd == listenfd_tcp) {
        cerr << "New TCP connection request\n";
        auto it = fd_to_type.find(curr.fd);
        assert(it != fd_to_type.end() && it->second == TCP_SOCKET);
        connect_tcp_client();
      } else if (curr.fd == listenfd_udp) {
        cerr << "New UDP message received\n";
        auto it = fd_to_type.find(curr.fd);
        assert(it != fd_to_type.end() && it->second == UDP_SOCKET);
        handle_udp_message(curr);
      } else if (curr.fd == STDIN_FILENO) {
        cerr << "Input from stdin\n";
        // Am primit un mesaj de la tastatura
        if (handle_stdin()) return 1;
      } else {
        cerr << "TCP client " << curr.fd << " has data to read\n";
        auto it = fd_to_type.find(curr.fd);
        assert(it != fd_to_type.end() && it->second == TCP_SOCKET);
        if (handle_tcp_message(curr)) {
          disconnect_tcp_client(curr);
          remove_fd_by_pos(i);
        }
      }
    }
  }

  return 0;
}
