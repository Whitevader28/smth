#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "helpers.h"

using namespace std;

int receive_packet(int sockfd, ChatPacket& packet) {
  // Receive header first (length and type)
  size_t header_size = sizeof(uint16_t) + sizeof(uint32_t);
  vector<char> header_buffer(header_size);

  int rc = recv_all(sockfd, header_buffer.data(), header_size);
  if (rc <= 0) return rc;

  if ((size_t)rc != header_size) {
    cerr << "Error: Partial header received (" << rc << "/" << header_size
         << ")" << endl;
    return -1;
  }

  // Extract length and type from header buffer (network byte order)
  uint16_t net_len;
  uint32_t net_type;
  memcpy(&net_len, header_buffer.data(), sizeof(uint16_t));
  memcpy(&net_type, header_buffer.data() + sizeof(uint16_t), sizeof(uint32_t));

  packet.len = ntohs(net_len);
  packet.type = static_cast<MessageType>(ntohl(net_type));

  if (packet.len > sizeof(packet.payload)) {
    cerr << "Error: Received packet with excessive payload length ("
         << packet.len << "). Max allowed is " << sizeof(packet.payload)
         << endl;
    return -1;
  }

  // Receive payload
  rc = recv_all(sockfd, &packet.payload, packet.len);
  if (rc < 0) {
    return rc;
  }
  if (rc != packet.len) {
    cerr << "Error: Partial payload received (" << rc << "/" << packet.len
         << ")" << endl;
    return -1;
  }

  return rc;
}

int send_packet(int sockfd, const ChatPacket& packet) {
  // Prepare header
  uint16_t net_len = htons(packet.len);
  uint32_t net_type = htonl(static_cast<int>(packet.type));

  size_t header_size = sizeof(net_len) + sizeof(net_type);
  size_t total_size = header_size + packet.len;

  // Create a buffer to hold header + payload
  vector<char> message_buffer(total_size);
  memcpy(message_buffer.data(), &net_len, sizeof(net_len));
  memcpy(message_buffer.data() + sizeof(net_len), &net_type, sizeof(net_type));

  size_t payload_copy_len = packet.len;
  if (payload_copy_len > sizeof(packet.payload)) {
    cerr << "WARN: Packet payload length " << payload_copy_len
         << " exceeds payload buffer size " << sizeof(packet.payload)
         << ". Truncating copy for send." << endl;
    payload_copy_len = sizeof(packet.payload);
  }
  memcpy(message_buffer.data() + header_size, &packet.payload,
         payload_copy_len);

  int rc = send_all(sockfd, message_buffer.data(), total_size);

  return rc;
}

void run_client(int socket_fd) {
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, MAX_MSG_SIZE + 1);

  ChatPacket recv_packet;

  struct pollfd poll_fds[2];
  poll_fds[0].fd = socket_fd;
  poll_fds[0].events = POLLIN;  // Listen for incoming data from the server
  poll_fds[0].revents = 0;

  poll_fds[1].fd = STDIN_FILENO;
  poll_fds[1].events = POLLIN;  // Listen for input from stdin
  poll_fds[1].revents = 0;

  disable_nagle(socket_fd);

  while (1) {
    int rc = poll(poll_fds, 2, -1);
    DIE(rc < 0 && errno != EINTR, "poll");

    // Poll was interrupted by a signal, just continue the loop
    if (rc < 0 && errno == EINTR) continue;

    // Check for events on the server socket
    if (poll_fds[0].revents & POLLIN) {
      // Received data from the server
      int bytes_received = receive_packet(socket_fd, recv_packet);
      if (bytes_received <= 0) {
        // Connection closed by server or error
        if (bytes_received == 0)
          cerr << "Server closed connection." << endl;
        else
          perror("receive_packet from server");

        break;  // Exit client loop
      }

      // Process the received packet
      if (recv_packet.type == MSG_EXIT) {
        cerr << "Received EXIT message from server. Exiting." << endl;
        break;  // Exit client loop
      }
      if (recv_packet.type == MSG_ERROR) {
        recv_packet.payload
            .text[recv_packet.len > 0 ? recv_packet.len - 1 : 0] = '\0';
        cerr << "Received ERROR message from server: "
             << recv_packet.payload.text << endl;
        if (strcmp(recv_packet.payload.text, "EINUSE") == 0) {
          cerr << "Client ID already in use. Exiting." << endl;
          break;  // Exit client loop on duplicate ID error
        }
      }

      // Assuming MSG_UDP_FORWARD is the type for messages from UDP
      if (recv_packet.type == MSG_UDP_FORWARD) {
        recv_packet.payload
            .text[recv_packet.len > 0 ? recv_packet.len - 1 : 0] = '\0';
        // Display the message received from the server
        printf("%s\n", recv_packet.payload.text);
      }
      // else shouldn't happen :))
    }

    // Check for events on stdin
    if (poll_fds[1].revents & POLLIN) {
      rc = read(STDIN_FILENO, buf, MAX_MSG_SIZE);
      DIE(rc < 0, "read stdin");

      if (rc == 0) {
        cerr << "EOF received on stdin. Sending exit command." << endl;
        ChatPacket exit_packet;
        memset(&exit_packet, 0, sizeof(exit_packet));
        exit_packet.len = strlen("exit") + 1;
        exit_packet.type = MSG_EXIT;
        strncpy(exit_packet.payload.text, "exit", exit_packet.len);
        send_packet(socket_fd, exit_packet);
        break;  // Exit client loop
      }

      // Remove trailing newline if present
      if (rc > 0 && buf[rc - 1] == '\n') {
        buf[rc - 1] = '\0';
      } else {
        buf[rc] = '\0';
      }

      // Process user command
      string input(buf);
      std::size_t first_space_pos = input.find(' ');

      if (input == "exit") {
        ChatPacket exit_packet;
        memset(&exit_packet, 0, sizeof(exit_packet));
        exit_packet.len = strlen("exit") + 1;
        exit_packet.type = MSG_EXIT;
        strncpy(exit_packet.payload.text, "exit", exit_packet.len);
        send_packet(socket_fd, exit_packet);
        break;  // Exit client loop
      }

      // Handle subscribe/unsubscribe commands
      if (first_space_pos != std::string::npos) {
        string command = input.substr(0, first_space_pos);
        string topic_pattern = input.substr(first_space_pos + 1);

        // Basic validation for topic pattern (no spaces, not empty)
        if (topic_pattern.empty() || topic_pattern.find(' ') != string::npos) {
          cerr << "Invalid topic format: '" << topic_pattern << "'" << endl;
          continue;  // Skip invalid command
        }

        ChatPacket command_packet;
        memset(&command_packet, 0, sizeof(command_packet));

        if (command == "subscribe") {
          command_packet.type = MSG_SUBSCRIBE;
          strncpy(command_packet.payload.text, buf, MAX_MSG_SIZE);
          command_packet.payload.text[MAX_MSG_SIZE] = '\0';
          command_packet.len = strlen(command_packet.payload.text) + 1;

          printf("Subscribed to topic %s\n", topic_pattern.c_str());
          send_packet(socket_fd, command_packet);
        } else if (command == "unsubscribe") {
          command_packet.type = MSG_UNSUBSCRIBE;
          strncpy(command_packet.payload.text, buf, MAX_MSG_SIZE);
          command_packet.payload.text[MAX_MSG_SIZE] = '\0';
          command_packet.len = strlen(command_packet.payload.text) + 1;

          printf("Unsubscribed from topic %s\n", topic_pattern.c_str());
          send_packet(socket_fd, command_packet);
        } else {
          cerr << "Unknown command: " << command << endl;
        }
      } else {
        cerr << "Invalid command format." << endl;
      }
    }
  }
}

int main(int argc, char* argv[]) {
  int rc = 0;

  setvbuf(stdout, NULL, _IONBF, BUFSIZ);

  if (argc != 4) {
    printf("\n Usage: %s <ID_CLIENT> <IP_SERVER> <PORT_SERVER>\n", argv[0]);
    return 1;
  }

  uint16_t port;
  rc = sscanf(argv[3], "%hu", &port);
  DIE(rc != 1, "Given port is invalid");

  char id[MAX_CLIENT_ID_SIZE + 1];
  memset(id, 0, sizeof(id));
  strncpy(id, argv[1], MAX_CLIENT_ID_SIZE);
  id[MAX_CLIENT_ID_SIZE] = '\0';

  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  DIE(socket_fd < 0, "socket");

  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  rc = inet_pton(AF_INET, argv[2], &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton");

  // Connect to the server
  rc = connect(socket_fd, (const sockaddr*)&serv_addr, sizeof(serv_addr));

  if (rc < 0) {
    perror("connect");
    if (errno == ECONNREFUSED) {
      cerr << "Connection refused. Is the server running and accessible at "
           << argv[2] << ":" << port << "?" << endl;
    } else if (errno == ENETUNREACH) {
      cerr << "Network is unreachable." << endl;
    } else if (errno == ETIMEDOUT) {
      cerr << "Connection attempt timed out." << endl;
    }
    exit(EXIT_FAILURE);
  }

  // Send the client ID to the server immediately after connecting
  ChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.type = MSG_ID;
  strncpy(packet.payload.id, id, MAX_CLIENT_ID_SIZE);
  packet.payload.id[MAX_CLIENT_ID_SIZE] = '\0';
  packet.len = strlen(packet.payload.id) + 1;

  int bytes_sent = send_packet(socket_fd, packet);
  if (bytes_sent < 0) {
    perror("send_packet ID");
    close(socket_fd);
    exit(EXIT_FAILURE);
  }
  if (bytes_sent != (int)(sizeof(uint16_t) + sizeof(uint32_t) + packet.len)) {
    cerr << "Warning: Sent partial ID packet." << endl;
  }

  run_client(socket_fd);

  close(socket_fd);
  return 0;
}
