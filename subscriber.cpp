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
#include <string>
#include <vector>

#include "common.h"
#include "helpers.h"

using namespace std;

int receive_packet(int sockfd, ChatPacket& packet) {
    // Receive header first (length and type)
    size_t header_size = sizeof(uint16_t) + sizeof(uint32_t);
    vector<char> header_buffer(header_size);

    int rc = recv_all(sockfd, header_buffer.data(), header_size);
    if (rc <= 0)
        return rc;

    if ((size_t)rc != header_size) {
        cerr << "Error: Partial header received (" << rc << "/" << header_size << ")" << endl;
        return -1;
    }

    // Extract length and type from header buffer (network byte order)
    uint16_t net_len;
    uint32_t net_type;
    memcpy(&net_len, header_buffer.data(), sizeof(uint16_t));
    memcpy(&net_type, header_buffer.data() + sizeof(uint16_t), sizeof(uint32_t));

    packet.len = ntohs(net_len);
    packet.type = static_cast<MessageType>(ntohl(net_type));

    // Validate payload length
    if (packet.len > sizeof(packet.payload)) {
        cerr << "Error: Received packet with excessive payload length (" << packet.len << "). Max allowed is " << sizeof(packet.payload) << endl;
        // Attempt to read and discard the excess data to clear the socket buffer?
        // For simplicity here, we'll just return error and hope the server didn't send malformed data.
        // A more robust client might try to read and discard.
        return -1; // Indicate error
    }

    // Receive payload
    rc = recv_all(sockfd, &packet.payload, packet.len);
     if (rc < 0) {
         // Error during payload receive
         return rc;
     }
     if (rc != packet.len) {
         // Should not happen with recv_all unless connection closed partially
         cerr << "Error: Partial payload received (" << rc << "/" << packet.len << ")" << endl;
         return -1; // Indicate error
     }

    return rc; // Return bytes received (payload length)
}

// Function to send a ChatPacket to the server
// Uses blocking send_all from common.cpp
int send_packet(int sockfd, const ChatPacket& packet) {
    // Prepare header (length and type in network byte order)
    uint16_t net_len = htons(packet.len);
    uint32_t net_type = htonl(static_cast<int>(packet.type)); // Ensure type is sent as 4 bytes

    size_t header_size = sizeof(net_len) + sizeof(net_type);
    size_t total_size = header_size + packet.len;

    // Create a buffer to hold header + payload
    vector<char> message_buffer(total_size);
    memcpy(message_buffer.data(), &net_len, sizeof(net_len));
    memcpy(message_buffer.data() + sizeof(net_len), &net_type, sizeof(net_type));

    // Safely copy payload, respecting packet length
    size_t payload_copy_len = packet.len;
    if (payload_copy_len > sizeof(packet.payload)) {
         cerr << "WARN: Packet payload length " << payload_copy_len << " exceeds payload buffer size " << sizeof(packet.payload) << ". Truncating copy for send." << endl;
         payload_copy_len = sizeof(packet.payload);
    }
    memcpy(message_buffer.data() + header_size, &packet.payload, payload_copy_len);

    // Send the entire buffer (header + payload)
    int rc = send_all(sockfd, message_buffer.data(), total_size);

    return rc; // Return bytes sent (total_size) or -1 on error
}


void run_client(int socket_fd) {
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, MAX_MSG_SIZE + 1);

  ChatPacket recv_packet;

  struct pollfd poll_fds[2];
  poll_fds[0].fd = socket_fd;
  poll_fds[0].events = POLLIN; // Listen for incoming data from the server
  poll_fds[1].fd = STDIN_FILENO;
  poll_fds[1].events = POLLIN; // Listen for input from stdin
  poll_fds[1].revents = 0;
  poll_fds[0].revents = 0;

  // Disable Nagle's algorithm for the client socket for low latency
  disable_nagle(socket_fd);


  while (1) {
    int rc = poll(poll_fds, 2, -1); // Wait indefinitely for events
    DIE(rc < 0 && errno != EINTR, "poll"); // Handle poll errors, ignore EINTR

    if (rc < 0 && errno == EINTR) {
        // Poll was interrupted by a signal, just continue the loop
        continue;
    }

    // Check for events on the server socket
    if (poll_fds[0].revents & POLLIN) {
      // Received data from the server
      int bytes_received = receive_packet(socket_fd, recv_packet);
      if (bytes_received <= 0) {
          // Connection closed by server or error
          if (bytes_received == 0) {
              cerr << "Server closed connection." << endl;
          } else {
              perror("receive_packet from server");
          }
          break; // Exit client loop
      }

      // Process the received packet
      if (recv_packet.type == MSG_EXIT) {
          cerr << "Received EXIT message from server. Exiting." << endl;
          break; // Exit client loop
      }
      if (recv_packet.type == MSG_ERROR) {
          // Ensure null termination for safety before printing
          recv_packet.payload.text[recv_packet.len > 0 ? recv_packet.len - 1 : 0] = '\0';
          cerr << "Received ERROR message from server: " << recv_packet.payload.text << endl;
          if (strcmp(recv_packet.payload.text, "EINUSE") == 0) {
              cerr << "Client ID already in use. Exiting." << endl;
              break; // Exit client loop on duplicate ID error
          }
          // Handle other error types if necessary
      }
      // Assuming MSG_UDP_FORWARD is the type for messages from UDP clients
      if (recv_packet.type == MSG_UDP_FORWARD) {
           // Ensure null termination for safety before printing
           recv_packet.payload.text[recv_packet.len > 0 ? recv_packet.len - 1 : 0] = '\0';
           // Print the formatted message received from the server
           printf("%s\n", recv_packet.payload.text);
      }
      // Ignore other unexpected message types
    }

    // Check for events on stdin
    if (poll_fds[1].revents & POLLIN) {
      // Received data from the keyboard (user input)
      rc = read(STDIN_FILENO, buf, MAX_MSG_SIZE);
      DIE(rc < 0, "read stdin");

      if (rc == 0) { // EOF on stdin (e.g., Ctrl+D)
          cerr << "EOF received on stdin. Sending exit command." << endl;
          ChatPacket exit_packet;
          memset(&exit_packet, 0, sizeof(exit_packet));
          exit_packet.len = strlen("exit") + 1; // Include null terminator
          exit_packet.type = MSG_EXIT;
          strncpy(exit_packet.payload.text, "exit", exit_packet.len);
          send_packet(socket_fd, exit_packet);
          break; // Exit client loop
      }


      // Remove trailing newline if present
      if (rc > 0 && buf[rc - 1] == '\n') {
          buf[rc - 1] = '\0';
      } else {
          buf[rc] = '\0'; // Null terminate if newline wasn't the last char
      }


      // Process user command
      string input(buf);
      std::size_t first_space_pos = input.find(' ');

      if (input == "exit") {
        ChatPacket exit_packet;
        memset(&exit_packet, 0, sizeof(exit_packet));
        exit_packet.len = strlen("exit") + 1; // Include null terminator
        exit_packet.type = MSG_EXIT;
        strncpy(exit_packet.payload.text, "exit", exit_packet.len);
        send_packet(socket_fd, exit_packet);
        break; // Exit client loop
      }

      // Handle subscribe/unsubscribe commands
      if (first_space_pos != std::string::npos) {
          string command = input.substr(0, first_space_pos);
          string topic_pattern = input.substr(first_space_pos + 1);

          // Basic validation for topic pattern (no spaces, not empty)
          if (topic_pattern.empty() || topic_pattern.find(' ') != string::npos) {
              cerr << "Invalid topic format: '" << topic_pattern << "'" << endl;
              continue; // Skip invalid command
          }
          // Further validation based on spec (e.g., leading/trailing slashes for wildcards?)
          // The spec mentions "şiruri de caractere ASCII fără spații" for topics used in the application.
          // Wildcards + and * are allowed, separated by /. The split function in server_tcp_com.cpp
          // handles consecutive slashes by ignoring empty tokens. Let's assume the server handles
          // the full validation of the topic pattern including wildcard rules.
          // For the client, we just need to send the command and the pattern.

          ChatPacket command_packet;
          memset(&command_packet, 0, sizeof(command_packet));

          if (command == "subscribe") {
              command_packet.type = MSG_SUBSCRIBE;
              // Send the whole command string "subscribe topic_pattern" as payload
              // The server will parse this.
              strncpy(command_packet.payload.text, buf, MAX_MSG_SIZE); // Use the original buf with command and topic
              command_packet.payload.text[MAX_MSG_SIZE] = '\0'; // Ensure null termination
              command_packet.len = strlen(command_packet.payload.text) + 1; // Include null terminator

              printf("Subscribed to topic %s\n", topic_pattern.c_str());
              send_packet(socket_fd, command_packet);

          } else if (command == "unsubscribe") {
              command_packet.type = MSG_UNSUBSCRIBE;
              // Send the whole command string "unsubscribe topic_pattern" as payload
              strncpy(command_packet.payload.text, buf, MAX_MSG_SIZE); // Use the original buf
              command_packet.payload.text[MAX_MSG_SIZE] = '\0'; // Ensure null termination
              command_packet.len = strlen(command_packet.payload.text) + 1; // Include null terminator

              printf("Unsubscribed from topic %s\n", topic_pattern.c_str());
              send_packet(socket_fd, command_packet);

          } else {
              // Unknown command
              cerr << "Unknown command: " << command << endl;
              // Do not send anything to the server for unknown commands
          }
      } else {
          // Input without space (not 'exit', 'subscribe', or 'unsubscribe' with topic)
          cerr << "Invalid command format." << endl;
          // Do not send anything to the server
      }
    }
  }
}

int main(int argc, char *argv[]) {
  int rc = 0;

  // Disable buffering for stdout
  setvbuf(stdout, NULL, _IONBF, BUFSIZ);
  // Disable buffering for stderr as well (good practice)
  setvbuf(stderr, NULL, _IONBF, BUFSIZ);


  if (argc != 4) {
    printf("\n Usage: %s <ID_CLIENT> <IP_SERVER> <PORT_SERVER>\n", argv[0]);
    return 1;
  }

  uint16_t port;
  rc = sscanf(argv[3], "%hu", &port);
  DIE(rc != 1, "Given port is invalid");

  char id[MAX_CLIENT_ID_SIZE + 1]; // +1 for null terminator safety
  memset(id, 0, sizeof(id));
  // Use strncpy to prevent buffer overflow if argv[1] is too long
  strncpy(id, argv[1], MAX_CLIENT_ID_SIZE);
  id[MAX_CLIENT_ID_SIZE] = '\0'; // Ensure null termination


  // Create a TCP socket for connection with the server
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  DIE(socket_fd < 0, "socket");

  // Complete the server address structure
  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  rc = inet_pton(AF_INET, argv[2], &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton"); // Handles invalid IP address format

  // Connect to the server
  rc = connect(socket_fd, (const sockaddr *)&serv_addr, sizeof(serv_addr));
  // Check specific connection errors
  if (rc < 0) {
      perror("connect");
      // Provide more specific error messages for common connection issues
      if (errno == ECONNREFUSED) {
          cerr << "Connection refused. Is the server running and accessible at " << argv[2] << ":" << port << "?" << endl;
      } else if (errno == ENETUNREACH) {
          cerr << "Network is unreachable." << endl;
      } else if (errno == ETIMEDOUT) {
          cerr << "Connection attempt timed out." << endl;
      }
      exit(EXIT_FAILURE); // Exit on connection failure
  }


  // Send the client ID to the server immediately after connecting
  ChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.type = MSG_ID;
  // Use the validated and null-terminated id buffer
  strncpy(packet.payload.id, id, MAX_CLIENT_ID_SIZE);
  packet.payload.id[MAX_CLIENT_ID_SIZE] = '\0'; // Double-ensure null termination in payload
  packet.len = strlen(packet.payload.id) + 1; // Include null terminator in length

  int bytes_sent = send_packet(socket_fd, packet);
  if (bytes_sent < 0) {
      perror("send_packet ID");
      close(socket_fd);
      exit(EXIT_FAILURE);
  }
  if (bytes_sent != (int)(sizeof(uint16_t) + sizeof(uint32_t) + packet.len)) {
      cerr << "Warning: Sent partial ID packet." << endl;
      // Depending on severity, might close connection here
  }


  // Run the main client loop
  run_client(socket_fd);

  // Close the socket when the client loop finishes
  close(socket_fd);

  return 0;
}
