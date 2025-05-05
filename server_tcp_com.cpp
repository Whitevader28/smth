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
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "helpers.h"

// "123.456.789.012:65535 - " - exemplu de prefix maximal
#define MAX_PREFIX_LEN 25

// receive_from_client remains unchanged as it relies on blocking recv_all
// If recv needs non-blocking behavior, recv_all needs modification similar to
// send_all
int receive_from_client(int socket_fd, ChatPacket &packet) {
  int rc = 0;
  uint16_t net_len;
  // First, read the length (2 bytes)
  rc = recv_all(socket_fd, &net_len, sizeof(net_len));
  if (rc <= 0) return rc;  // Return 0 on close, -1 on error
  // DIE(rc < 0, "recv len"); // Let caller handle -1
  packet.len = ntohs(net_len);

  if (packet.len > MAX_MSG_SIZE) {
    fprintf(stderr,
            "Error: received packet length %u exceeds MAX_MSG_SIZE %d\n",
            packet.len, MAX_MSG_SIZE);
    // Handle error: discard, close connection? For now, return error.
    return -1;
  }

  // Then, read the type (4 bytes, enum as int)
  int type_int = 0;
  rc = recv_all(socket_fd, &type_int, sizeof(type_int));
  if (rc <= 0) return rc;  // Return 0 on close, -1 on error
  // DIE(rc < 0, "recv type"); // Let caller handle -1
  packet.type = static_cast<MessageType>(ntohl(type_int));
  // Then, read the payload
  rc = recv_all(socket_fd, &packet.payload, packet.len);
  if (rc <= 0) return rc;  // Return 0 on close, -1 on error
  // DIE(rc < 0, "recv payload"); // Let caller handle -1
  // Return total bytes for the message frame (len+type+payload)
  cerr << "Packet length (recv): " << packet.len << '\n';
  return sizeof(net_len) + sizeof(type_int) + rc;
}

int send_to_client(int socket_fd, ChatPacket &packet) {
  int rc = 0;
  cerr << "Packet length (send): " << packet.len << '\n';
  uint16_t net_len = htons(packet.len);
  uint16_t len_size = sizeof(net_len);
  int type_int = htonl(static_cast<int>(packet.type));
  uint16_t type_size = sizeof(type_int);
  uint16_t payload_size = packet.len;
  int total_sent = 0;

  // Send length
  rc = send_all(socket_fd, &net_len, len_size);
  if (rc < 0) {  // Error during send_all
    perror("send_all len");
    return -1;
  } else if ((size_t)rc < len_size) {
    cerr << "WARN: Could not send full length to client " << socket_fd
         << " (sent " << rc << "/" << len_size << ")" << endl;
    return rc;  // Return bytes actually sent
  }
  total_sent += rc;

  // Send type
  rc = send_all(socket_fd, &type_int, type_size);
  if (rc < 0) {
    perror("send_all type");
    return -1;
  } else if ((size_t)rc < type_size) {
    cerr << "WARN: Could not send full type to client " << socket_fd
         << " (sent " << rc << "/" << type_size << ")" << endl;
    return total_sent + rc;
  }
  total_sent += rc;

  // Send payload
  rc = send_all(socket_fd, &packet.payload, payload_size);
  if (rc < 0) {
    perror("send_all payload");
    return -1;
  } else if ((size_t)rc < payload_size) {
    cerr << "WARN: Could not send full payload to client " << socket_fd
         << " (sent " << rc << "/" << payload_size << ")" << endl;
    // Fall through to return total bytes sent including partial payload
  }
  total_sent += rc;  // Add potentially partial payload bytes sent

  return total_sent;  // Return total bytes actually sent
}

int create_tcp_listenfd(uint16_t port, const char *ip) {
  const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  DIE(listenfd < 0, "socket");

  // Allow address reuse quickly
  int enable = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) <
      0) {
    perror("setsockopt(SO_REUSEADDR) failed");
  }

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

  disable_nagle(listenfd);  // Nagle is usually disabled on listening socket
                            // too, though mainly affects connections

  return listenfd;
}

int create_udp_listenfd(uint16_t port, const char *ip) {
  const int listenfd = socket(AF_INET, SOCK_DGRAM, 0);
  DIE(listenfd < 0, "socket");

  // Allow address reuse quickly
  int enable = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) <
      0) {
    perror("setsockopt(SO_REUSEADDR) failed");
  }

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
  packet.len = strlen("exit") + 1;  // Include null terminator
  packet.type = MSG_EXIT;
  strncpy(packet.payload.text, "exit", packet.len);  // Use actual length
  // packet.payload.text[MAX_MSG_SIZE] = 0; // Already null terminated by
  // strncpy + length

  // Iterate using the map directly is safer if remove_fd_by_pos is used
  // Need a copy of fds to iterate over if disconnect_tcp_client modifies the
  // underlying structures
  vector<int> client_fds;
  for (auto const &[fd, type] : fd_to_type) {
    if (type == TCP_SOCKET && fd != listenfd_tcp) {
      client_fds.push_back(fd);
    }
  }

  for (int client_fd : client_fds) {
    cerr << "Sending exit to client fd: " << client_fd << endl;
    // Use the potentially non-blocking send_to_client
    int bytes_sent = send_to_client(client_fd, packet);
    if (bytes_sent < 0) {
      cerr << "Error sending exit message to client fd: " << client_fd << endl;
    } else if ((size_t)bytes_sent <
               (sizeof(packet.len) + sizeof(packet.type) + packet.len)) {
      cerr << "WARN: Could not send full exit message to client fd: "
           << client_fd << endl;
    }
  }
}

void ClientConnections::remove_fd_by_pos(int pos) {
  // Ensure pos is valid
  if (pos < 0 || pos >= num_fds) return;

  int fd_to_remove = poll_fds[pos].fd;
  cerr << "Removing fd " << fd_to_remove << " at pos " << pos << endl;

  // Remove from maps first to avoid issues if fd is reused quickly
  fd_to_type.erase(fd_to_remove);
  if (client_tcp_by_fd.count(fd_to_remove)) {
    // Optional: update client state if not already disconnected
    // client_tcp_by_fd[fd_to_remove]->state = ClientTCP::DISCONNECTED;
    // client_tcp_by_fd[fd_to_remove]->socketfd = -1; // Mark as invalid
    client_tcp_by_fd.erase(fd_to_remove);
  }

  // Shift remaining elements in poll_fds
  for (int j = pos; j < num_fds - 1; j++) {
    poll_fds[j] = poll_fds[j + 1];
  }

  // Clear the last element (optional but good practice)
  memset(&poll_fds[num_fds - 1], 0, sizeof(struct pollfd));

  --num_fds;
}

void ClientConnections::add_fd(int fd, short int events) {
  if (num_fds >= MAX_TCP_CONNECTIONS + 3) {  // Check against array size
    cerr << "Error: Maximum number of connections reached. Cannot add fd " << fd
         << endl;
    // Optionally close fd here?
    return;
  }
  poll_fds[num_fds].fd = fd;
  poll_fds[num_fds].events = events;
  poll_fds[num_fds].revents = 0;  // Ensure revents is clear
  num_fds++;
  cerr << "Added fd " << fd << " at index " << num_fds - 1 << endl;
}

void ClientConnections::connect_tcp_client() {
  ChatPacket packet_from_tcp;
  shared_ptr<ClientTCP> client = make_shared<ClientTCP>();
  int rc;
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  const int newsockfd =
      accept(listenfd_tcp, (struct sockaddr *)&client_addr, &client_len);
  // Don't DIE here, handle error gracefully if possible
  if (newsockfd < 0) {
    perror("accept");
    return;  // Cannot accept connection
  }

  // Dezactivam algoritmul Nagle
  disable_nagle(newsockfd);

  // Set socket to non-blocking for potential later use with non-blocking I/O
  // int flags = fcntl(newsockfd, F_GETFL, 0);
  // if (flags != -1) {
  //    fcntl(newsockfd, F_SETFL, flags | O_NONBLOCK);
  // } else {
  //    perror("fcntl F_GETFL for newsockfd");
  // }
  // NOTE: Keep it blocking for now as receive_from_client is blocking

  rc = receive_from_client(newsockfd, packet_from_tcp);
  // Handle error or close from receive_from_client
  if (rc <= 0) {
    if (rc == 0) {
      fprintf(stderr, "Client disconnected before sending ID.\n");
    } else {
      perror("recv client ID");
    }
    close(newsockfd);  // Close the new socket
    return;            // Don't add fd or process further
  }

  // Primul mesaj de la client este id-ul (assume type MSG_ID)
  if (packet_from_tcp.type != MSG_ID) {
    fprintf(stderr,
            "Error: First packet from client was not MSG_ID (type=%d)\n",
            packet_from_tcp.type);
    // Optionally send error message back
    close(newsockfd);
    return;
  }

  // Ensure null termination for safety
  packet_from_tcp.payload.id[MAX_CLIENT_ID_SIZE - 1] = '\0';
  char *client_id = packet_from_tcp.payload.id;
  string client_id_str(client_id);  // Use std::string for map keys

  // Id ul exista deja
  if (client_tcp_by_id.count(client_id_str)) {  // Use count for existence check
    shared_ptr<ClientTCP> existing_client = client_tcp_by_id[client_id_str];

    if (existing_client->state == ClientTCP::CONNECTED) {
      // Clientul este deja conectat trimitem un mesaj de eroare
      printf("Client %s already connected.\n", client_id);
      ChatPacket err_packet;
      memset(&err_packet, 0, sizeof(err_packet));
      const char *errMsg = "EINUSE";  // Or a more descriptive message
      err_packet.len = strlen(errMsg) + 1;
      err_packet.type = MSG_ERROR;
      strncpy(err_packet.payload.text, errMsg, err_packet.len);

      // Use non-blocking send
      send_to_client(newsockfd, err_packet);  // Ignore result for error msg
      close(newsockfd);  // Close the duplicate connection attempt
    } else {
      // Clientul s-a reconectat
      char ip_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
      printf("New client %s connected from %s:%hu.\n", client_id, ip_str,
             ntohs(client_addr.sin_port));

      // Update existing client object
      existing_client->socketfd = newsockfd;
      existing_client->state = ClientTCP::CONNECTED;
      // Don't change ID map, just update FD map and fd_to_type map
      client_tcp_by_fd[newsockfd] = existing_client;
      fd_to_type[newsockfd] = TCP_SOCKET;

      add_fd(newsockfd, POLLIN);  // Add the new fd to poll set
    }
  } else {
    // Clientul este nou
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    printf("New client %s connected from %s:%hu.\n", client_id, ip_str,
           ntohs(client_addr.sin_port));

    // Configure the new client object
    client->socketfd = newsockfd;
    client->state = ClientTCP::CONNECTED;
    strncpy(client->id, client_id, MAX_CLIENT_ID_SIZE);
    client->id[MAX_CLIENT_ID_SIZE - 1] = '\0';  // Ensure null termination

    // Add to maps
    client_tcp_by_id[client_id_str] = client;
    client_tcp_by_fd[newsockfd] = client;
    fd_to_type[newsockfd] = TCP_SOCKET;

    add_fd(newsockfd, POLLIN);
  }
}

/**
 * Verificam doar mesajul "exit" de la stdin
 */
int ClientConnections::handle_stdin() {
  int rc;
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, MAX_MSG_SIZE + 1);

  // Use read directly, assuming line buffering or user hits Enter
  rc = read(STDIN_FILENO, buf, MAX_MSG_SIZE);
  if (rc < 0) {
    perror("read stdin");
    return 0;  // Don't exit on stdin read error
  }
  if (rc == 0) {  // EOF on stdin
    cerr << "EOF received on stdin." << endl;
    // Optionally trigger exit? Or just ignore.
    return 0;
  }

  // Remove trailing newline if present
  if (rc > 0 && buf[rc - 1] == '\n') {
    buf[rc - 1] = 0;
    rc--;  // Adjust length
  } else {
    buf[rc] = 0;  // Null terminate if newline wasn't the last char
  }

  if (strcmp(buf, "exit") == 0) {
    cerr << "Exit command received from stdin." << endl;
    onExit();  // Send exit to clients
    return 1;  // Signal to main loop to exit
  } else {
    cerr << "Unknown command from stdin: " << buf << endl;
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

// TODO: Implement actual wildcard matching as per assignment description
// The current simple check `topic == pattern` is incorrect for wildcards.
bool match_topic(const string &topic, const string &pattern) {
  if (topic == pattern) return true;
  // Placeholder - Add proper wildcard ('+', '*') logic here
  // This requires parsing topic and pattern by '/' and comparing levels.
  return false;
}

void ClientConnections::handle_udp_message(pollfd curr) {
  char buf[MAX_MSG_SIZE + 1];  // Buffer for the raw UDP payload
  memset(buf, 0, MAX_MSG_SIZE + 1);

  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(client_addr));
  socklen_t client_len = sizeof(client_addr);

  int rc =
      recvfrom(curr.fd, buf, MAX_MSG_SIZE, 0,  // Read only up to MAX_MSG_SIZE
               (struct sockaddr *)&client_addr, &client_len);

  if (rc < 0) {
    perror("recvfrom UDP");
    return;
  }
  // Ensure buffer is null-terminated within received length if used as string
  // buf[rc] = '\\0'; // Be careful if buffer isn't just text

  // Validate received size - needs at least topic (50) + type (1) = 51 bytes
  if (rc < 51) {
    cerr << "Error: Received UDP packet too short (" << rc << " bytes)" << endl;
    return;
  }

  // --- Prepare the message to forward to TCP clients ---
  ChatPacket packet_to_tcp;
  memset(&packet_to_tcp, 0, sizeof(packet_to_tcp));
  packet_to_tcp.type = MSG_TEXT;  // Type indicates it's forwarded text data

  char *tcp_msg_payload =
      packet_to_tcp.payload.text;  // Pointer to payload buffer
  int tcp_payload_len = 0;         // Current length of the payload being built
  char tmp_val_str[256];           // Temporary buffer for formatting numbers

  // 1. Format sender IP:PORT prefix
  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
  tcp_payload_len += snprintf(tcp_msg_payload + tcp_payload_len,
                              MAX_MSG_SIZE - tcp_payload_len, "%s:%hu - ",
                              ip_str, ntohs(client_addr.sin_port));

  // 2. Append TOPIC (first 50 bytes of UDP payload, ensure null termination)
  char topic_name[51];
  strncpy(topic_name, buf, 50);
  topic_name[50] = '\0';
  tcp_payload_len +=
      snprintf(tcp_msg_payload + tcp_payload_len,
               MAX_MSG_SIZE - tcp_payload_len, "%s - ", topic_name);

  // 3. Parse UDP payload content based on TYPE (byte 51, index 50)
  uint8_t data_type = (uint8_t)buf[50];
  const char *type_str = "UNKNOWN";
  char formatted_value[MAX_CONTENT_SIZE + 10] = {
      0};  // Buffer for formatted value string

  uint32_t int_val;
  uint16_t short_real_val;
  uint8_t sign;
  uint32_t float_mod;
  uint8_t float_pow;

  // Check remaining buffer size before accessing content
  if (rc < 51) {  // Already checked, but double check
    cerr << "Error: UDP packet too short for data type." << endl;
    return;
  }

  switch (data_type) {
    case 0: {// INT
      type_str = "INT";
      if (rc < 51 + 1 + 4) {
        cerr << "Error: UDP packet too short for INT." << endl;
        return;
      }
      sign = (uint8_t)buf[51];
      int_val = ntohl(*(uint32_t *)(buf + 52));
      snprintf(formatted_value, sizeof(formatted_value), "%s%u",
               (sign == 1 ? "-" : ""), int_val);
      break;
    }
    case 1: { // SHORT_REAL
      type_str = "SHORT_REAL";
      if (rc < 51 + 2) {
        cerr << "Error: UDP packet too short for SHORT_REAL." << endl;
        return;
      }
      short_real_val = ntohs(*(uint16_t *)(buf + 51));
      snprintf(formatted_value, sizeof(formatted_value), "%.2f",
               short_real_val / 100.0);
      break;
    }
    case 2: { // FLOAT
      type_str = "FLOAT";
      if (rc < 51 + 1 + 4 + 1) {
        cerr << "Error: UDP packet too short for FLOAT." << endl;
        return;
      }
      sign = (uint8_t)buf[51];
      float_mod = ntohl(*(uint32_t *)(buf + 52));
      float_pow = (uint8_t)buf[52 + 4];
      // Use snprintf for safe formatting, %.Xg for float precision
      snprintf(formatted_value, sizeof(formatted_value), "%s%.*g",
               (sign == 1 ? "-" : ""),
               10,  // Max significant digits to prevent excessive length
               (float)float_mod / pow(10.0, float_pow));
      break;
    }
    case 3: { // STRING
      type_str = "STRING";
      // Content starts at buf + 51. Max length is rc - 51.
      int content_len = rc - 51;
      if (content_len < 0) content_len = 0;  // Should not happen if rc >= 51
      if (content_len > MAX_CONTENT_SIZE) {
        cerr << "WARN: UDP String content longer than MAX_CONTENT_SIZE, "
                "truncating."
             << endl;
        content_len = MAX_CONTENT_SIZE;
      }
      // Copy safely, ensuring null termination
      strncpy(formatted_value, buf + 51, content_len);
      formatted_value[content_len] = '\0';  // Ensure null termination
      break;
    }
    default: {
      cerr << "Unknown UDP data type: " << (int)data_type << endl;
      snprintf(formatted_value, sizeof(formatted_value), "[Invalid Type %d]",
               data_type);
      // Don't forward messages with unknown types? Or forward with error?
      return;  // For now, don't forward.
    }
  }

  // 4. Append TYPE string
  tcp_payload_len +=
      snprintf(tcp_msg_payload + tcp_payload_len,
               MAX_MSG_SIZE - tcp_payload_len, "%s - ", type_str);

  // 5. Append formatted VALUE
  tcp_payload_len +=
      snprintf(tcp_msg_payload + tcp_payload_len,
               MAX_MSG_SIZE - tcp_payload_len, "%s", formatted_value);

  // Finalize TCP packet
  packet_to_tcp.len = tcp_payload_len + 1;  // Include null terminator in length
  tcp_msg_payload[tcp_payload_len] = '\0';  // Ensure null termination
  // Sanity check length
  if (packet_to_tcp.len > MAX_MSG_SIZE + 1) {
    cerr << "Error: Constructed TCP payload exceeds MAX_MSG_SIZE." << endl;
    packet_to_tcp.len = MAX_MSG_SIZE + 1;   // Cap length
    tcp_msg_payload[MAX_MSG_SIZE] = '\0';  // Ensure termination
  }

  cerr << "Forwarding UDP msg: " << tcp_msg_payload
       << endl;  // Keep logging for debugging

  // Extract the topic name correctly from the buffer for matching.
  std::string udp_topic_name(topic_name);

  // Iterate through all registered topics and their subscribers
  for (const auto &topic_pair : topic_to_clients) {
    const std::string &subscribed_pattern =
        topic_pair.first;  // This is the pattern client subscribed with
    const auto &clients =
        topic_pair.second;  // This is a set<shared_ptr<const ClientTCP>>

    // Corrected topic matching: Match the actual UDP topic against the
    // subscribed pattern
    // TODO: Replace with actual wildcard matching logic!
    if (match_topic(
            udp_topic_name,
            subscribed_pattern)) {  // Match(actual_topic, subscribed_pattern)
      cerr << "Matched topic '" << udp_topic_name << "' against pattern '"
           << subscribed_pattern << "'" << endl;
      // Send message to all active clients subscribed to this pattern who
      // haven't received it yet
      for (const auto &client :
           clients) {  // client here is shared_ptr<const ClientTCP>
        // Check client state and if already notified for this UDP message
        // --- Now the types match for find() ---
          cerr << "Sending to client: " << client->id
               << " (fd: " << client->socketfd << ")" << endl;
          int bytes_sent = send_to_client(client->socketfd, packet_to_tcp);
          if (bytes_sent < 0) {
            cerr << "Error sending to client: " << client->id << endl;
            // Handle error: maybe mark client for disconnect?
          } else if ((size_t)bytes_sent <
                     (sizeof(packet_to_tcp.len) + sizeof(packet_to_tcp.type) +
                      packet_to_tcp.len)) {
            // Warning already printed by send_to_client
            // Consider buffering remaining data if critical
          }
      }
    }
  }
}

int ClientConnections::handle_tcp_message(pollfd curr) {
  ChatPacket packet_from_client;
  bool client_disconnected = false;
  int client_fd = curr.fd;  // Keep fd for logging

  // Loop to process multiple messages if available
  while (true) {
    // Check if data is available without blocking
    char peek_buffer;
    int peek_rc = recv(client_fd, &peek_buffer, 1, MSG_PEEK | MSG_DONTWAIT);

    if (peek_rc == 0) {
      // Connection closed by peer
      client_disconnected = true;
      break;
    } else if (peek_rc < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No more data available right now
        break;
      } else {
        // Actual error
        perror("recv MSG_PEEK");
        client_disconnected = true;  // Treat error as disconnect
        break;
      }
    }

    // Data seems available, try to receive a full message (using blocking
    // helper)
    memset(&packet_from_client, 0,
           sizeof(packet_from_client));  // Clear packet for next message
    int rc = receive_from_client(client_fd, packet_from_client);

    if (rc < 0) {  // Error during receive_from_client
      perror("receive_from_client");
      client_disconnected = true;
      break;
    }
    if (rc == 0) {  // Connection closed during receive_from_client
      client_disconnected = true;
      break;
    }

    // --- Process the received packet ---
    // Ensure payload is null-terminated for string operations
    if (packet_from_client.type == MSG_SUBSCRIBE ||
        packet_from_client.type == MSG_UNSUBSCRIBE ||
        packet_from_client.type == MSG_TEXT ||
        packet_from_client.type == MSG_EXIT) {
      packet_from_client.payload.text[packet_from_client.len - 1] =
          '\0';  // Assuming len includes null term
      packet_from_client.payload.text[MAX_MSG_SIZE] = '\0';  // Boundary check
    } else if (packet_from_client.type == MSG_ID) {
      packet_from_client.payload.id[packet_from_client.len - 1] =
          '\0';  // Assuming len includes null term
      packet_from_client.payload.id[MAX_CLIENT_ID_SIZE - 1] =
          '\0';  // Boundary check
    }

    if (packet_from_client.type == MSG_EXIT) {
      // Check payload content just in case
      if (strcmp(packet_from_client.payload.text, "exit") == 0) {
        client_disconnected = true;
        cerr << "Client fd " << client_fd << " sent exit command." << endl;
        break;  // Exit loop after processing exit command
      } else {
        cerr << "WARN: Received MSG_EXIT type but payload is not 'exit'."
             << endl;
        // Treat as disconnect anyway? Or ignore? For now, treat as disconnect.
        client_disconnected = true;
        break;
      }
    }

    std::string input_command;
    if (packet_from_client.type == MSG_SUBSCRIBE ||
        packet_from_client.type == MSG_UNSUBSCRIBE) {
      // Payload should be "command topic"
      input_command = std::string(packet_from_client.payload.text);
    } else {
      cerr << "Received unhandled/unexpected message type from TCP client fd "
           << client_fd << ": " << packet_from_client.type << endl;
      continue;  // Skip processing, try next message
    }

    std::size_t space_pos = input_command.find(' ');
    if (space_pos == std::string::npos) {
      cerr << "Command format error from fd " << client_fd << ": '"
           << input_command << "'" << endl;
      continue;  // Skip malformed command
    }

    string command = input_command.substr(0, space_pos);
    string topic = input_command.substr(space_pos + 1);

    // Basic topic validation (disallow spaces, empty, leading/trailing '/')
    if (topic.empty() || topic.find(' ') != string::npos ||
        topic.front() == '/' || topic.back() == '/') {
      cerr << "Topic format error from fd " << client_fd << ": '" << topic
           << "'" << endl;
      continue;  // Skip malformed topic
    }

    if (command == "subscribe") {
      // TODO: REMOVE THIS debug command eventually
      if (topic == "getAll") {
        // Print all the user is subscribed to (for debugging)
        cerr << "Client " << client_tcp_by_fd[client_fd]->id
             << " requested getAll:\n";
        bool found = false;
        for (const auto &pair : topic_to_clients) {
          if (pair.second.count(client_tcp_by_fd[client_fd])) {
            cerr << "   " << pair.first << "\n";
            found = true;
          }
        }
        if (!found) cerr << "   (Not subscribed to any topics)\n";
        continue;  // Don't treat as normal subscribe
      }
      subscribe_to_topic(curr, topic);  // Pass original pollfd struct if needed
    } else if (command == "unsubscribe") {
      unsubscribe_from_topic(curr,
                             topic);  // Pass original pollfd struct if needed
    } else {
      cerr << "Unknown command from fd " << client_fd << ": '" << command << "'"
           << endl;
      continue;
    }
    // If we reached here, a message was processed successfully. Loop again.
  }  // End while(true) loop

  return client_disconnected
             ? 1
             : 0;  // Return 1 if client disconnected, 0 otherwise
}

void ClientConnections::subscribe_to_topic(pollfd curr, string topic) {
  // Ensure client exists for this fd
  if (!client_tcp_by_fd.count(curr.fd)) {
    cerr << "Error: subscribe_to_topic called for unknown fd " << curr.fd
         << endl;
    return;
  }
  topic_to_clients[topic].insert(client_tcp_by_fd[curr.fd]);
  cerr << "Client " << client_tcp_by_fd[curr.fd]->id
       << " subscribed to topic: " << topic << endl;
}

void ClientConnections::unsubscribe_from_topic(pollfd curr, string topic) {
  // Ensure client exists for this fd
  if (!client_tcp_by_fd.count(curr.fd)) {
    cerr << "Error: unsubscribe_from_topic called for unknown fd " << curr.fd
         << endl;
    return;
  }
  shared_ptr<ClientTCP> client = client_tcp_by_fd[curr.fd];

  // Find the topic in the map
  auto it = topic_to_clients.find(topic);

  if (it == topic_to_clients.end()) {
    // Topic doesn't exist, so client can't be subscribed
    cerr << "Client " << client->id
         << " tried to unsubscribe from non-existent/empty topic: " << topic
         << endl;
    return;
  }

  // Try removing the client from the set for this topic
  auto &clients = it->second;
  size_t num_removed = clients.erase(client);

  if (num_removed > 0) {
    cerr << "Client " << client->id << " unsubscribed from topic: " << topic
         << endl;
    // If the set becomes empty after removal, erase the topic entry from the
    // map
    if (clients.empty()) {
      topic_to_clients.erase(it);
      cerr << "Topic " << topic << " removed as no clients are subscribed."
           << endl;
    }
  } else {
    // Client was not found in the set for this topic
    cerr << "Client " << client->id << " was not subscribed to topic: " << topic
         << endl;
  }
}

void ClientConnections::disconnect_tcp_client(pollfd curr) {
  int fd_to_disconnect = curr.fd;
  cerr << "Disconnecting client fd " << fd_to_disconnect << endl;

  // Check if client is still mapped (might have been removed by
  // remove_fd_by_pos if disconnect is delayed)
  if (client_tcp_by_fd.count(fd_to_disconnect)) {
    auto client = client_tcp_by_fd[fd_to_disconnect];
    printf("Client %s disconnected.\n", client->id);  // Required output

    // Update state and maps BEFORE closing fd
    client->state = ClientTCP::DISCONNECTED;
    client->socketfd = -1;  // Mark socket fd as invalid in the client object

    // Remove from fd-based maps (ID map retains the client object for potential
    // reconnect)
    client_tcp_by_fd.erase(fd_to_disconnect);
    fd_to_type.erase(fd_to_disconnect);

    // Optional: Remove client from all topic subscription sets
    // This prevents sending stored messages upon reconnect unless
    // store-and-forward is implemented string client_id_str = client->id; //
    // Need string version if iterating topics for (auto it =
    // topic_to_clients.begin(); it != topic_to_clients.end(); /* manual advance
    // */) {
    //     it->second.erase(client);
    //     if (it->second.empty()) {
    //         cerr << "Topic " << it->first << " removed during disconnect
    //         cleanup." << endl; it = topic_to_clients.erase(it); // Erase and
    //         advance iterator
    //     } else {
    //         ++it; // Advance iterator
    //     }
    // }

  } else {
    cerr << "WARN: disconnect_tcp_client called for fd " << fd_to_disconnect
         << " which is no longer mapped." << endl;
    // Still need to remove from fd_to_type if somehow it remained
    fd_to_type.erase(fd_to_disconnect);
  }

  // Close the socket file descriptor
  close(fd_to_disconnect);

  // Note: We don't remove from poll_fds here; pollAll loop handles that via
  // remove_fd_by_pos
}

int ClientConnections::pollAll() {
  // Timeout for poll can be 0 for non-blocking check, -1 for infinite wait, or
  // positive ms
  int poll_timeout_ms = -1;  // Wait indefinitely for an event
  int rc = poll(poll_fds, num_fds, poll_timeout_ms);

  if (rc < 0) {
    // EINTR might happen, should potentially restart poll
    if (errno == EINTR) {
      cerr << "poll interrupted, retrying..." << endl;
      return 0;  // Indicate loop should continue
    }
    // Other errors are more serious
    perror("poll");
    // What should happen on poll error? Exit? Try to recover?
    return 1;  // For now, signal exit on critical poll error
  }

  if (rc == 0) {
    // Timeout occurred (only if poll_timeout_ms > 0)
    // This part is currently unreachable with timeout = -1
    cerr << "poll timed out" << endl;
    return 0;  // Continue loop
  }

  // Iterate through descriptors that might have events
  // Need to iterate carefully as remove_fd_by_pos modifies the array
  for (int i = 0; i < num_fds; /* manual increment */) {
    pollfd current_pollfd = poll_fds[i];  // Copy struct for safety
    int current_fd = current_pollfd.fd;
    bool fd_removed = false;  // Flag to check if the current fd was removed

    if (current_pollfd.revents == 0) {
      // No events for this fd, move to the next
      ++i;
      continue;
    }

    // --- Handle different event types ---

    if (current_pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      // Error condition on socket
      cerr << "Error/Hangup event on fd " << current_fd
           << " (revents: " << current_pollfd.revents << ")" << endl;
      if (current_fd == listenfd_tcp || current_fd == listenfd_udp ||
          current_fd == stdinfd) {
        // Error on listening socket or stdin is critical
        fprintf(stderr, "Critical error on listening socket or stdin.\n");
        return 1;  // Signal exit
      } else {
        // Error on client socket, treat as disconnect
        disconnect_tcp_client(current_pollfd);  // Pass the struct
        remove_fd_by_pos(i);                    // Remove from poll set
        fd_removed = true;
      }
    } else if (current_pollfd.revents & POLLIN) {
      // Data available to read
      if (current_fd == listenfd_tcp) {
        cerr << "POLLIN on TCP listen fd " << current_fd << endl;
        // Handle new connection attempts (can accept multiple in a row if
        // ready)
        while (true) {  // Loop accept until EAGAIN
          // Need to make listening socket non-blocking for this loop or use
          // select/poll again? Sticking to one accept per POLLIN event for
          // simplicity now.
          connect_tcp_client();
          // Check if more connections are waiting immediately (requires
          // non-blocking accept) For now, break after one accept attempt per
          // POLLIN signal.
          break;
        }
      } else if (current_fd == listenfd_udp) {
        cerr << "POLLIN on UDP listen fd " << current_fd << endl;
        // Handle incoming UDP message (read one datagram)
        handle_udp_message(current_pollfd);
      } else if (current_fd == STDIN_FILENO) {
        cerr << "POLLIN on stdin fd " << current_fd << endl;
        // Handle input from stdin
        if (handle_stdin()) {
          return 1;  // Exit command received
        }
      } else {
        // Data from existing TCP client
        cerr << "POLLIN on TCP client fd " << current_fd << endl;
        // Make sure it's still considered a TCP socket
        if (fd_to_type.count(current_fd) &&
            fd_to_type[current_fd] == TCP_SOCKET) {
          if (handle_tcp_message(current_pollfd)) {  // handle_tcp_message
                                                     // returns 1 on disconnect
            disconnect_tcp_client(current_pollfd);   // Perform cleanup
            remove_fd_by_pos(i);                     // Remove from poll set
            fd_removed = true;
          }
        } else {
          cerr << "WARN: POLLIN event for unknown or non-TCP fd " << current_fd
               << endl;
          // Should not happen if maps are consistent
          remove_fd_by_pos(i);  // Remove the problematic fd
          fd_removed = true;
          close(current_fd);  // Close the fd just in case
        }
      }
    }

    // --- Increment loop counter ---
    // Only increment if the current element wasn't removed
    if (!fd_removed) {
      ++i;
    }
    // If fd was removed, the next element shifted to index i, so we re-evaluate
    // index i in the next iteration.

  }  // End for loop iterating through poll_fds

  return 0;  // Signal loop should continue
}
