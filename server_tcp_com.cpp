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

#include <algorithm>  // For std::find
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>  // Required for std::unordered_set
#include <vector>

#include "common.h"  // Includes ChatPacket, MessageType etc. Ensure common.h defines MAX_UDP_MSG_SIZE and MSG_UDP_FORWARD in MessageType enum
#include "helpers.h"  // Includes DIE

// --- Function Definitions for Socket Creation (restored from original) ---
int create_tcp_listenfd(uint16_t port, const char* ip) {
  const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  DIE(listenfd < 0, "socket");

  // Allow address reuse quickly
  int enable = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) <
      0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    // Consider closing fd and returning -1 or exiting
  }

  // Set socket to non-blocking *before* bind/listen if accept loop needs it
  // if (!set_nonblocking(listenfd)) {
  //     close(listenfd);
  //     DIE(1,"set_nonblocking listenfd"); // Or return -1
  // }

  // Prepare server address structure
  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  int rc = inet_pton(AF_INET, ip, &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton");

  // Bind the socket
  rc = bind(listenfd, (const struct sockaddr*)&serv_addr, sizeof(serv_addr));
  DIE(rc < 0, "bind");

  // Disable Nagle's algorithm (usually desired for low latency servers)
  // Note: disable_nagle is defined in common.cpp/common.h
  disable_nagle(listenfd);

  // Start listening (done in run_multiplexed in server.cpp now)
  // rc = listen(listenfd, MAX_TCP_CONNECTIONS); // MAX_TCP_CONNECTIONS should
  // be defined (e.g., in common.h) DIE(rc < 0, "listen");

  return listenfd;
}

int create_udp_listenfd(uint16_t port, const char* ip) {
  const int listenfd = socket(AF_INET, SOCK_DGRAM, 0);
  DIE(listenfd < 0, "socket UDP");

  // Allow address reuse quickly
  int enable = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) <
      0) {
    perror("setsockopt(SO_REUSEADDR) failed for UDP");
    // Consider closing fd and returning -1 or exiting
  }

  // Set non-blocking if recvfrom will be used in a non-blocking way
  if (!set_nonblocking(listenfd)) {
    close(listenfd);
    DIE(1, "set_nonblocking listenfd_udp");  // Or return -1
  }

  // Prepare server address structure
  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  int rc = inet_pton(AF_INET, ip, &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton UDP");

  // Bind the socket
  rc = bind(listenfd, (const struct sockaddr*)&serv_addr, sizeof(serv_addr));
  DIE(rc < 0, "bind UDP");

  return listenfd;
}
// --- End Function Definitions ---

// Helper function to split a string by a delimiter
static std::vector<std::string> split(const std::string& s, char delimiter) {
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(s);
  while (std::getline(tokenStream, token, delimiter)) {
    // Don't add empty tokens if multiple delimiters are consecutive
    // or if delimiter is at start/end
    if (!token.empty()) {
      tokens.push_back(token);
    }
  }
  return tokens;
}

// --- ClientConnections Implementation ---

int ClientConnections::find_poll_index(int fd) {
  for (int i = 0; i < num_fds; ++i) {
    if (poll_fds[i].fd == fd) {
      return i;
    }
  }
  return -1;  // Not found
}

void ClientConnections::add_fd(int fd, short int events, SocketType type) {
  if (num_fds >=
      MAX_TCP_CONNECTIONS + 3) {  // +3 for stdin, tcp_listen, udp_listen
    cerr << "Error: Maximum number of fds reached. Cannot add fd " << fd
         << endl;
    // Optionally close fd here if it's a new client connection attempt?
    if (type == TCP_CLIENT_SOCKET) close(fd);
    return;
  }
  int index = num_fds;
  poll_fds[index].fd = fd;
  poll_fds[index].events = events;
  poll_fds[index].revents = 0;  // Ensure revents is clear
  fd_to_type[fd] = type;
  num_fds++;
  // cerr << "Added fd " << fd << " (" << type << ") at index " << index << ",
  // num_fds=" << num_fds << endl;
}

void ClientConnections::remove_fd(int fd) {
  int pos = find_poll_index(fd);
  if (pos < 0) {
    // cerr << "WARN: Tried to remove fd " << fd << " which is not in poll_fds."
    // << endl; Clean up maps just in case
    fd_to_type.erase(fd);
    // client_by_fd removal is handled in disconnect_tcp_client
    return;
  }

  // cerr << "Removing fd " << fd << " at pos " << pos << ", num_fds was " <<
  // num_fds << endl;

  // Remove from maps first
  fd_to_type.erase(fd);
  // Note: client_by_fd removal is handled in disconnect_tcp_client

  // Shift remaining elements in poll_fds
  // Use memmove for efficiency if needed, but loop is clear
  for (int j = pos; j < num_fds - 1; j++) {
    poll_fds[j] = poll_fds[j + 1];
  }

  // Clear the last element (optional but good practice)
  memset(&poll_fds[num_fds - 1], 0, sizeof(struct pollfd));

  --num_fds;
  // cerr << "Removed fd. num_fds is now " << num_fds << endl;
}

void ClientConnections::connect_tcp_client() {
  struct sockaddr_in client_addr;
  socklen_t client_len = sizeof(client_addr);

  // Accept the connection
  // Use non-blocking accept if listenfd_tcp is non-blocking
  const int newsockfd =
      accept(listenfd_tcp, (struct sockaddr*)&client_addr, &client_len);
  if (newsockfd < 0) {
    // EAGAIN/EWOULDBLOCK means no more connections waiting right now
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // This is expected if the listening socket is non-blocking and we loop
      // accept
      return;
    }
    perror("accept");
    return;  // Other error
  }

  // --- Configure the new socket ---
  disable_nagle(newsockfd);  // Disable Nagle for low latency

  if (!set_nonblocking(newsockfd)) {  // Make the client socket non-blocking
    cerr << "Error setting non-blocking for client fd " << newsockfd << endl;
    close(newsockfd);
    return;
  }

  // --- Add to poll set immediately, wait for ID message ---
  // We add it *before* receiving the ID. We'll read the ID via the normal poll
  // mechanism. Create a temporary state until ID is received? Or handle ID
  // reception in handle_client_read? Let's create the state now, but mark as
  // not fully connected until ID is validated.
  auto new_client = make_shared<ClientState>(newsockfd);
  // Temporarily map the new fd to this state. This will be updated if it's a
  // reconnect.
  client_by_fd[newsockfd] = new_client;
  add_fd(newsockfd, POLLIN,
         TCP_CLIENT_SOCKET);  // Initially just listen for read (for ID)

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
  // Don't print the "New client..." message until the ID is received and
  // validated.
  cerr << "Accepted connection from " << ip_str << ":"
       << ntohs(client_addr.sin_port) << " on fd " << newsockfd
       << ". Waiting for ID." << endl;
}

void ClientConnections::disconnect_tcp_client(int fd, bool cleanup_poll) {
  // cerr << "Disconnecting client fd " << fd << endl; // Reduced verbosity

  // Find client state using the file descriptor
  auto it_fd = client_by_fd.find(fd);
  if (it_fd != client_by_fd.end()) {
    shared_ptr<ClientState> client = it_fd->second;

    // Print disconnect message only if client was fully connected (had valid
    // ID)
    if (client && client->connected) {  // Check client pointer validity too
      printf("Client %s disconnected.\n",
             client->id.c_str());  // Required output
    } else {
      cerr << "Client on fd " << fd
           << " disconnected before sending/validating ID or client state "
              "invalid."
           << endl;
    }

    // Update state (mark as disconnected, invalidate fd)
    // The ClientState object itself remains in client_by_id
    if (client) {  // Ensure client pointer is valid before accessing members
      client->connected = false;
      client->socketfd = -1;  // Mark socket fd as invalid in the client object
      // Keep send_queue and recv_buffer for potential store-and-forward (if
      // implemented) client->recv_buffer.clear(); // Clear receive buffer on
      // disconnect? Depends on protocol. client->send_queue.clear(); // Clear
      // send queue on disconnect? Depends on protocol.
      // client->current_send_offset = 0;
    } else {
      cerr << "WARN: disconnect_tcp_client found invalid client pointer for fd "
           << fd << endl;
    }

    // Remove from fd-based map (this is crucial as the fd is no longer valid)
    client_by_fd.erase(it_fd);

    // *** IMPORTANT CHANGE: DO NOT remove client from client_by_id. ***
    // *** Subscriptions persist in the ClientState object stored in
    // client_by_id. ***

    // *** IMPORTANT CHANGE: DO NOT call remove_client_subscriptions here. ***
    // *** Subscriptions are kept for reconnects. ***

  } else {
    cerr << "WARN: disconnect_tcp_client called for fd " << fd
         << " which has no ClientState mapped in client_by_fd." << endl;
    // This might happen if the fd was already cleaned up, but let's try to
    // remove from poll anyway.
  }

  // Close the socket file descriptor
  close(fd);

  // Remove from poll set if requested (usually yes)
  if (cleanup_poll) {
    remove_fd(fd);
  }
}

void ClientConnections::onExit() {
  ChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.len = strlen("exit") + 1;  // Include null terminator
  packet.type = MSG_EXIT;
  strncpy(packet.payload.text, "exit", packet.len);  // Copy "exit"

  cerr << "Server shutting down. Sending exit to connected clients..." << endl;

  // Iterate through currently connected clients using client_by_fd
  vector<int> client_fds_to_notify;
  for (auto const& [fd, client_ptr] : client_by_fd) {
    // Only notify currently connected clients
    if (client_ptr && client_ptr->connected && client_ptr->socketfd >= 0) {
      client_fds_to_notify.push_back(fd);
    }
  }

  for (int client_fd : client_fds_to_notify) {
    auto it = client_by_fd.find(client_fd);
    if (it != client_by_fd.end()) {
      shared_ptr<ClientState> client = it->second;
      cerr << "Sending exit to client fd: " << client_fd
           << " (ID: " << client->id << ")" << endl;
      // Use send_packet logic which queues/sends
      send_message_to_client(client, packet);
    }
  }
  // Consider adding a short sleep or a final poll with timeout here if needed
  // sleep(1); // Give time for messages to potentially be sent
}

int ClientConnections::handle_stdin() {
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, sizeof(buf));

  int rc = read(STDIN_FILENO, buf, MAX_MSG_SIZE);
  if (rc < 0) {
    perror("read stdin");
    return 0;  // Don't exit on stdin read error
  }
  if (rc == 0) {  // EOF on stdin
    cerr << "EOF received on stdin. Initiating exit." << endl;
    return 1;  // Treat EOF as exit command
  }

  // Remove trailing newline if present
  if (rc > 0 && buf[rc - 1] == '\n') {
    buf[rc - 1] = '\0';
  } else {
    buf[rc] = '\0';  // Null terminate if newline wasn't the last char
  }

  if (strcmp(buf, "exit") == 0) {
    cerr << "Exit command received from stdin." << endl;
    return 1;  // Signal to main loop to exit
  } else {
    cerr << "Unknown command from stdin: '" << buf << "'. Use 'exit' to quit."
         << endl;
  }

  return 0;  // Continue loop
}

bool ClientConnections::match_topic(const string& topic,
                                    const string& pattern) {
  vector<string> topic_levels = split(topic, '/');
  vector<string> pattern_levels = split(pattern, '/');

  // Base case: Empty topic and pattern match
  if (topic_levels.empty() && pattern_levels.empty()) {
    return true;
  }
  // Edge case: Pattern is just "*" and topic is not empty - match
  if (pattern_levels.size() == 1 && pattern_levels[0] == "*" &&
      !topic_levels.empty()) {
    return true;
  }
  // Edge case: Topic is empty but pattern is not "*" - no match
  if (topic_levels.empty() &&
      !(pattern_levels.size() == 1 && pattern_levels[0] == "*")) {
    return false;
  }
  // Edge case: Pattern is empty but topic is not - no match
  if (pattern_levels.empty() && !topic_levels.empty()) {
    return false;
  }

  // Dynamic programming approach for '*' wildcard
  // dp[i][j] will be true if the first i levels of the topic match the first j
  // levels of the pattern
  vector<vector<bool>> dp(topic_levels.size() + 1,
                          vector<bool>(pattern_levels.size() + 1, false));

  // Base case: Empty topic and empty pattern match
  dp[0][0] = true;

  // Handle patterns starting with '*'
  if (!pattern_levels.empty() && pattern_levels[0] == "*") {
    dp[0][1] = true;  // '*' can match zero levels
  }

  for (size_t i = 1; i <= topic_levels.size(); ++i) {
    for (size_t j = 1; j <= pattern_levels.size(); ++j) {
      const string& current_pattern = pattern_levels[j - 1];

      if (current_pattern == "+") {
        // '+' matches one level in topic if previous levels matched
        dp[i][j] = dp[i - 1][j - 1];
      } else if (current_pattern == "*") {
        // '*' matches zero or more levels
        // Matches if previous pattern matched current topic level (dp[i-1][j])
        // OR if current pattern matched previous topic levels (dp[i][j-1])
        dp[i][j] = dp[i - 1][j] || dp[i][j - 1];
      } else {
        // Exact match required
        if (topic_levels[i - 1] == current_pattern) {
          dp[i][j] = dp[i - 1][j - 1];
        }
      }
    }
  }

  return dp[topic_levels.size()][pattern_levels.size()];
}

void ClientConnections::handle_udp_message(int udp_fd) {
  // Assuming MAX_UDP_MSG_SIZE is defined in common.h
  char raw_udp_buf[MAX_UDP_MSG_SIZE + 1];  // Buffer for the raw UDP payload
  memset(raw_udp_buf, 0,
         sizeof(raw_udp_buf));  // Now memset is after declaration

  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(client_addr));
  socklen_t client_len = sizeof(client_addr);

  int rc = recvfrom(udp_fd, raw_udp_buf, MAX_UDP_MSG_SIZE, 0,
                    (struct sockaddr*)&client_addr, &client_len);

  if (rc < 0) {
    // EAGAIN or EWOULDBLOCK is expected on non-blocking UDP socket
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;  // No datagram available right now
    }
    perror("recvfrom UDP");
    return;
  }
  if (rc == 0) {
    cerr << "WARN: recvfrom UDP returned 0." << endl;
    return;
  }

  // --- Basic validation of UDP packet structure ---
  // Needs topic (max 50) + type (1) = 51 bytes minimum?
  if (rc < 50 + 1) {  // Need space for topic and type byte
    cerr << "Error: Received UDP packet too short for topic+type (" << rc
         << " bytes)" << endl;
    return;
  }

  // --- Extract Topic ---
  char topic_name[51];  // Buffer for topic name + null terminator
  strncpy(topic_name, raw_udp_buf, 50);
  topic_name[50] = '\0';  // Ensure null termination for safety
  string udp_topic_str(topic_name);

  // --- Extract Data Type ---
  uint8_t data_type = (uint8_t)raw_udp_buf[50];

  // --- Prepare the message payload to forward to TCP clients ---
  ChatPacket packet_to_tcp;
  memset(&packet_to_tcp, 0, sizeof(packet_to_tcp));
  // Assuming MSG_UDP_FORWARD is defined in MessageType enum in common.h
  packet_to_tcp.type =
      MSG_UDP_FORWARD;  // Use a specific type for forwarded UDP messages

  char* tcp_msg_payload =
      packet_to_tcp.payload.text;  // Pointer to payload buffer
  int tcp_payload_len = 0;         // Current length of the payload being built

  // 1. Format sender IP:PORT prefix
  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
  tcp_payload_len += snprintf(tcp_msg_payload + tcp_payload_len,
                              MAX_MSG_SIZE - tcp_payload_len, "%s:%hu - ",
                              ip_str, ntohs(client_addr.sin_port));

  // 2. Append TOPIC name
  tcp_payload_len +=
      snprintf(tcp_msg_payload + tcp_payload_len,
               MAX_MSG_SIZE - tcp_payload_len, "%s - ", udp_topic_str.c_str());

  // 3. Parse UDP content based on TYPE and format it
  const char* type_str = "UNKNOWN";
  char formatted_value[MAX_CONTENT_SIZE + 20] = {
      0};  // Buffer for formatted value string (+extra space)
  const char* udp_content_ptr = raw_udp_buf + 51;  // Start of content payload
  int udp_content_len = rc - 51;                   // Length of content payload

  // Check if received data is sufficient for the declared type
  bool data_ok = true;
  uint8_t sign;             // Moved declaration outside switch
  uint32_t int_val;         // Moved declaration outside switch
  uint16_t short_real_val;  // Moved declaration outside switch
  uint32_t float_mod;       // Moved declaration outside switch
  uint8_t float_pow;        // Moved declaration outside switch

  switch (data_type) {
    case 0:  // INT
    {  // Added scope for local variables if any (none here, but good practice)
      type_str = "INT";
      if (udp_content_len < 1 + 4) {
        data_ok = false;
        break;
      }
      sign = (uint8_t)udp_content_ptr[0];
      // Use memcpy to avoid potential alignment issues when casting pointers
      memcpy(&int_val, udp_content_ptr + 1, sizeof(uint32_t));
      int_val = ntohl(int_val);
      snprintf(formatted_value, sizeof(formatted_value), "%s%u",
               (sign == 1 && int_val > 0 ? "-" : ""), int_val);
      break;
    }  // End case 0 scope

    case 1:  // SHORT_REAL
    {        // Added scope
      type_str = "SHORT_REAL";
      if (udp_content_len < 2) {
        data_ok = false;
        break;
      }
      memcpy(&short_real_val, udp_content_ptr, sizeof(uint16_t));
      short_real_val = ntohs(short_real_val);
      // Format with exactly two decimal places
      snprintf(formatted_value, sizeof(formatted_value), "%.2f",
               short_real_val / 100.0);
      break;
    }  // End case 1 scope

    case 2:  // FLOAT
    {        // Added scope to contain float_val initialization
      type_str = "FLOAT";
      if (udp_content_len < 1 + 4 + 1) {
        data_ok = false;
        break;
      }
      sign = (uint8_t)udp_content_ptr[0];
      memcpy(&float_mod, udp_content_ptr + 1, sizeof(uint32_t));
      float_mod = ntohl(float_mod);
      float_pow = (uint8_t)udp_content_ptr[1 + 4];
      // Calculate the float value carefully
      double float_val = (double)float_mod;  // Initialization inside the scope
      if (float_pow > 0) {
        float_val /= pow(10.0, float_pow);
      }
      // Use snprintf for safe formatting. %g might remove trailing zeros.
      // Let's control precision more directly if needed, or stick to %g.
      snprintf(formatted_value, sizeof(formatted_value), "%s%.10g",
               (sign == 1 && float_val > 0 ? "-" : ""), float_val);
      break;
    }  // End case 2 scope

    case 3:  // STRING
    {        // Added scope to contain copy_len initialization
      type_str = "STRING";
      // Content starts at udp_content_ptr. Max length is udp_content_len.
      int copy_len = udp_content_len;  // Initialization inside the scope
      if (copy_len < 0) copy_len = 0;  // Safety check
      if (copy_len > MAX_CONTENT_SIZE) {
        cerr << "WARN: UDP String content longer than MAX_CONTENT_SIZE ("
             << copy_len << "), truncating." << endl;
        copy_len = MAX_CONTENT_SIZE;
      }
      // Copy safely, ensuring null termination
      // Use memcpy for potentially non-null-terminated data, then add
      // terminator
      memcpy(formatted_value, udp_content_ptr, copy_len);
      // strncpy(formatted_value, udp_content_ptr, copy_len);
      formatted_value[copy_len] = '\0';  // Ensure null termination
      break;
    }  // End case 3 scope

    default: {  // Added scope
      data_ok = false;
      break;
    }  // End default scope
  }  // End switch

  if (!data_ok) {
    cerr << "Error: Invalid or incomplete UDP data for type " << (int)data_type
         << " in packet from " << ip_str << ":" << ntohs(client_addr.sin_port)
         << " (content len: " << udp_content_len << ")" << endl;
    snprintf(formatted_value, sizeof(formatted_value), "[Invalid Data]");
    // Decide whether to forward invalid messages or drop them. Let's drop them.
    return;
  }

  // 4. Append TYPE string (Check available space)
  if (MAX_MSG_SIZE > tcp_payload_len) {
    tcp_payload_len +=
        snprintf(tcp_msg_payload + tcp_payload_len,
                 MAX_MSG_SIZE - tcp_payload_len, "%s - ", type_str);
  }

  // 5. Append formatted VALUE (Check available space)
  if (MAX_MSG_SIZE > tcp_payload_len) {
    tcp_payload_len +=
        snprintf(tcp_msg_payload + tcp_payload_len,
                 MAX_MSG_SIZE - tcp_payload_len, "%s", formatted_value);
  }

  // --- Finalize TCP packet ---
  // Ensure null termination within MAX_MSG_SIZE
  if (tcp_payload_len >= MAX_MSG_SIZE) {
    tcp_payload_len = MAX_MSG_SIZE - 1;  // Leave space for null terminator
    cerr << "WARN: Constructed TCP payload truncated to fit MAX_MSG_SIZE."
         << endl;
  }
  tcp_msg_payload[tcp_payload_len] = '\0';
  packet_to_tcp.len = tcp_payload_len + 1;  // Length includes null terminator

  // cerr << "Prepared TCP forward: len=" << packet_to_tcp.len << ", payload='"
  // << tcp_msg_payload << "'" << endl;

  // --- Broadcast to subscribed TCP clients ---
  broadcast_udp_message(udp_topic_str, packet_to_tcp);
}

void ClientConnections::broadcast_udp_message(const string& udp_topic,
                                              const ChatPacket& tcp_packet) {
  // Keep track of clients already notified for this specific message to avoid
  // duplicates if multiple wildcard patterns match the same client for the same
  // topic.
  unordered_set<string> notified_client_ids;

  // Iterate through all client states stored by ID (these are the potentially
  // subscribed clients)
  for (const auto& id_client_pair : client_by_id) {
    const string& client_id = id_client_pair.first;
    shared_ptr<ClientState> client = id_client_pair.second;

    // Check if the client is currently connected AND not already notified for
    // this message
    if (client && client->connected && client->socketfd >= 0 &&
        notified_client_ids.find(client_id) == notified_client_ids.end()) {
      // Check if this connected client is subscribed to the topic or a matching
      // pattern
      bool is_subscribed = false;
      for (const string& pattern : client->subscriptions) {
        if (match_topic(udp_topic, pattern)) {
          is_subscribed = true;
          break;  // Found a matching subscription pattern
        }
      }

      if (is_subscribed) {
        // cerr << "  Forwarding to client ID: " << client_id << " (fd: " <<
        // client->socketfd << ") for topic '" << udp_topic << "'" << endl;
        send_message_to_client(client, tcp_packet);
        notified_client_ids.insert(
            client_id);  // Mark as notified for this message
      } else {
        // cerr << "  Client ID: " << client_id << " is connected but not
        // subscribed to topic '" << udp_topic << "'" << endl;
      }
    } else {
      // cerr << "  Skipping client ID: " << client_id << " (not connected,
      // invalid state, or already notified)" << endl;
    }
  }

  if (notified_client_ids.empty()) {
    // cerr << "No connected clients subscribed to topic '" << udp_topic << "'"
    // << endl;
  }
}

void ClientConnections::send_message_to_client(shared_ptr<ClientState> client,
                                               const ChatPacket& packet) {
  if (!client || !client->connected || client->socketfd < 0) {
    // cerr << "WARN: Attempted to send to disconnected or invalid client (ID: "
    // << (client ? client->id : "N/A") << ")" << endl;
    return;
  }

  // Serialize the packet (header + payload) into a byte vector
  uint16_t net_len = htons(packet.len);
  uint32_t net_type =
      htonl(static_cast<int>(packet.type));  // Ensure type is sent as 4 bytes
  size_t header_size = sizeof(net_len) + sizeof(net_type);
  // Validate packet.len before calculating total_size
  // Allow for ID message payload which might exceed MAX_MSG_SIZE slightly
  size_t max_allowed_payload_for_send =
      (packet.type == MSG_ID) ? MAX_CLIENT_ID_SIZE : MAX_MSG_SIZE;

  if (packet.len > (MAX_MSG_SIZE + MAX_CLIENT_ID_SIZE +
                    100)) {  // General sanity check length
    cerr << "ERROR: send_message_to_client called with excessive packet.len="
         << packet.len << ". Dropping message for client " << client->id << "."
         << endl;
    return;
  }
  if (packet.len > max_allowed_payload_for_send &&
      packet.type != MSG_ID) {  // Check specific limit for non-ID types
    cerr << "ERROR: send_message_to_client called with payload length "
         << packet.len << " exceeding limit " << max_allowed_payload_for_send
         << " for msg type " << packet.type << ". Dropping message for client "
         << client->id << "." << endl;
    return;
  }

  size_t total_size = header_size + packet.len;

  vector<char> message_buffer(total_size);
  memcpy(message_buffer.data(), &net_len, sizeof(net_len));
  memcpy(message_buffer.data() + sizeof(net_len), &net_type, sizeof(net_type));
  // Ensure payload copy does not exceed buffer bounds or packet length
  size_t payload_copy_len = packet.len;
  // Limit copy length to available payload data and buffer size
  if (payload_copy_len > sizeof(packet.payload)) {
    cerr << "WARN: send_message_to_client: Packet payload length "
         << payload_copy_len << " exceeds internal payload buffer size "
         << sizeof(packet.payload) << ". Truncating copy from source." << endl;
    payload_copy_len = sizeof(packet.payload);
  }
  if (header_size + payload_copy_len > total_size) {
    cerr << "ERROR: send_message_to_client: Internal logic error calculating "
            "buffer sizes for send. Adjusting copy length."
         << endl;
    // Avoid buffer overflow during memcpy
    payload_copy_len = total_size > header_size ? total_size - header_size : 0;
  }

  memcpy(message_buffer.data() + header_size, &packet.payload,
         payload_copy_len);  // Copy payload safely

  // Add the serialized message to the client's send queue
  client->send_queue.push_back(std::move(message_buffer));
  // cerr << "Queued message (type " << packet.type << ", total size " <<
  // total_size << ") for client " << client->id << ". Queue size: " <<
  // client->send_queue.size() << endl;

  // Check if we can start sending immediately (if queue was empty)
  // and register for POLLOUT events if needed.
  int poll_idx = find_poll_index(client->socketfd);
  if (poll_idx != -1) {
    // If the send queue was empty before adding this message,
    // we might be able to send immediately. Try a non-blocking send.
    // Also, ensure POLLOUT is set so poll() wakes us up when ready to send
    // more.
    if (client->send_queue.size() == 1 && client->current_send_offset == 0) {
      handle_client_write(client);  // Attempt initial send
    }

    // Make sure POLLOUT is registered if there's data pending
    if (!client->send_queue.empty()) {
      if (!(poll_fds[poll_idx].events & POLLOUT)) {
        // cerr << "Adding POLLOUT for fd " << client->socketfd << endl;
        poll_fds[poll_idx].events |= POLLOUT;
      }
    } else {
      // Should not happen right after adding, but for completeness:
      // If queue becomes empty, remove POLLOUT
      if (poll_fds[poll_idx].events & POLLOUT) {
        // cerr << "Queue empty, removing POLLOUT for fd " << client->socketfd
        // << endl;
        poll_fds[poll_idx].events &= ~POLLOUT;
      }
    }
  } else {
    cerr << "ERROR: Cannot find client fd " << client->socketfd
         << " in poll set to register POLLOUT." << endl;
  }
}

// --- Non-blocking Read/Write Handlers ---

void ClientConnections::handle_client_read(shared_ptr<ClientState> client) {
  if (!client || client->socketfd < 0) return;

  // Use a larger buffer if MAX_MSG_SIZE is large, or read in smaller chunks
  char read_buf[4096];  // Temporary buffer for reading (adjust size as needed)
  int bytes_read =
      recv_nonblocking(client->socketfd, read_buf, sizeof(read_buf));

  if (bytes_read == -1) {  // Genuine error
    cerr << "Error reading from client fd " << client->socketfd
         << " (ID: " << client->id << "). Disconnecting." << endl;
    disconnect_tcp_client(client->socketfd);
  } else if (bytes_read == 0) {  // Connection closed by peer
    // cerr << "Client fd " << client->socketfd << " (ID: " << client->id << ")
    // closed connection." << endl;
    disconnect_tcp_client(client->socketfd);
  } else if (bytes_read == -2) {  // EAGAIN / EWOULDBLOCK
    // Nothing to read right now, perfectly normal for non-blocking
    // cerr << "No data available to read on fd " << client->socketfd << " right
    // now." << endl;
  } else {  // Data received
    // cerr << "Read " << bytes_read << " bytes from fd " << client->socketfd <<
    // " (ID: " << client->id << ")" << endl; Append data to client's persistent
    // receive buffer
    client->recv_buffer.insert(client->recv_buffer.end(), read_buf,
                               read_buf + bytes_read);
    // Process the buffer to extract complete messages
    process_received_data(client);
  }
}

void ClientConnections::handle_client_write(shared_ptr<ClientState> client) {
  if (!client || client->socketfd < 0 || client->send_queue.empty()) {
    // Nothing to send or client invalid
    // If queue is empty, ensure POLLOUT is removed
    if (client && client->socketfd >= 0) {
      int poll_idx = find_poll_index(client->socketfd);
      if (poll_idx != -1 && (poll_fds[poll_idx].events & POLLOUT)) {
        // cerr << "Send queue empty for fd " << client->socketfd << ". Removing
        // POLLOUT." << endl;
        poll_fds[poll_idx].events &= ~POLLOUT;
      }
    }
    return;
  }

  // Get the current message chunk to send
  vector<char>& current_message = client->send_queue.front();
  size_t bytes_to_send = current_message.size() - client->current_send_offset;
  const char* send_ptr = current_message.data() + client->current_send_offset;

  // cerr << "Attempting to write " << bytes_to_send << " bytes to fd " <<
  // client->socketfd << " (ID: " << client->id << ")" << endl;

  int bytes_sent = send_nonblocking(client->socketfd, send_ptr, bytes_to_send);

  if (bytes_sent == -1) {  // Genuine error
    cerr << "Error writing to client fd " << client->socketfd
         << " (ID: " << client->id << "). Disconnecting." << endl;
    disconnect_tcp_client(client->socketfd);  // Error -> disconnect
  } else if (bytes_sent == 0) {  // EAGAIN / EWOULDBLOCK (or sent 0)
    // Cannot send more right now. Keep POLLOUT registered.
    // cerr << "Send buffer full for fd " << client->socketfd << ". Will retry
    // later." << endl; Ensure POLLOUT is still set
    int poll_idx = find_poll_index(client->socketfd);
    if (poll_idx != -1 && !(poll_fds[poll_idx].events & POLLOUT)) {
      // cerr << "Re-adding POLLOUT for fd " << client->socketfd << " as send
      // blocked." << endl;
      poll_fds[poll_idx].events |= POLLOUT;
    }
  } else {  // Successfully sent some bytes
    client->current_send_offset += bytes_sent;
    // cerr << "Sent " << bytes_sent << " bytes to fd " << client->socketfd <<
    // ". Total sent for this msg: " << client->current_send_offset << "/" <<
    // current_message.size() << endl;

    // Check if the current message is fully sent
    if (client->current_send_offset == current_message.size()) {
      // cerr << "Finished sending message for fd " << client->socketfd << ".
      // Queue size before pop: " << client->send_queue.size() << endl;
      client->send_queue.pop_front();
      client->current_send_offset = 0;
      // cerr << "Queue size after pop: " << client->send_queue.size() << endl;

      // If the queue is now empty, remove POLLOUT interest
      if (client->send_queue.empty()) {
        int poll_idx = find_poll_index(client->socketfd);
        if (poll_idx != -1 && (poll_fds[poll_idx].events & POLLOUT)) {
          // cerr << "Send queue empty for fd " << client->socketfd << ".
          // Removing POLLOUT." << endl;
          poll_fds[poll_idx].events &= ~POLLOUT;
        }
      } else {
        // More messages in queue, try sending the next one immediately? NO -
        // let poll decide. Ensure POLLOUT is still set if more data remains
        int poll_idx = find_poll_index(client->socketfd);
        if (poll_idx != -1 && !(poll_fds[poll_idx].events & POLLOUT)) {
          poll_fds[poll_idx].events |= POLLOUT;
        }
      }
    } else {
      // Message partially sent, ensure POLLOUT is set to try again later
      int poll_idx = find_poll_index(client->socketfd);
      if (poll_idx != -1 && !(poll_fds[poll_idx].events & POLLOUT)) {
        // cerr << "Adding POLLOUT for fd " << client->socketfd << " as message
        // partially sent." << endl;
        poll_fds[poll_idx].events |= POLLOUT;
      }
    }
  }
}

void ClientConnections::process_received_data(shared_ptr<ClientState> client) {
  if (!client) return;

  bool message_processed;
  do {
    message_processed = false;
    vector<char>& buf = client->recv_buffer;
    size_t buf_size = buf.size();
    size_t header_size =
        sizeof(uint16_t) + sizeof(uint32_t);  // len (2) + type (4)

    // Need at least enough data for the header
    if (buf_size < header_size) {
      // cerr << "Recv buffer (" << buf_size << " bytes) too small for header ("
      // << header_size << "). Waiting for more data." << endl;
      break;  // Not enough data for a header yet
    }

    // Extract length and type from buffer
    uint16_t net_len;
    uint32_t net_type;
    memcpy(&net_len, buf.data(), sizeof(uint16_t));
    memcpy(&net_type, buf.data() + sizeof(uint16_t), sizeof(uint32_t));

    uint16_t payload_len = ntohs(net_len);
    MessageType msg_type = static_cast<MessageType>(ntohl(net_type));
    size_t total_msg_len = header_size + payload_len;

    // Basic validation on payload length reported in header
    // Allow for ID message payload which might exceed MAX_MSG_SIZE slightly
    size_t max_allowed_payload =
        (msg_type == MSG_ID) ? MAX_CLIENT_ID_SIZE : MAX_MSG_SIZE;
    // Also add a general sanity limit
    if (payload_len >
        (MAX_MSG_SIZE + MAX_CLIENT_ID_SIZE + 100)) {  // General sanity check
      cerr << "ERROR: Received excessive payload length (" << payload_len
           << ") from fd " << client->socketfd << ". Disconnecting." << endl;
      disconnect_tcp_client(client->socketfd);
      return;  // Stop processing for this client
    }
    if (payload_len > max_allowed_payload &&
        msg_type != MSG_ID) {  // Check specific limit for non-ID types
      cerr << "ERROR: Received payload length " << payload_len
           << " exceeds limit " << max_allowed_payload << " for msg type "
           << msg_type << " from fd " << client->socketfd << ". Disconnecting."
           << endl;
      disconnect_tcp_client(client->socketfd);
      return;  // Stop processing for this client
    }

    // Check if the complete message is available in the buffer
    if (buf_size >= total_msg_len) {
      // cerr << "Complete message found in buffer (type " << msg_type << ",
      // payload " << payload_len << ", total " << total_msg_len << "). Buffer
      // size: " << buf_size << endl; We have a full message, extract it
      ChatPacket received_packet;
      memset(&received_packet, 0, sizeof(received_packet));
      received_packet.len = payload_len;
      received_packet.type = msg_type;

      // Safely copy payload, respecting buffer sizes
      size_t copy_len = payload_len;
      if (copy_len > sizeof(received_packet.payload)) {
        cerr << "WARN: Received payload length " << copy_len
             << " exceeds internal packet buffer "
             << sizeof(received_packet.payload)
             << ". Truncating data for processing." << endl;
        copy_len = sizeof(received_packet.payload);
      }
      memcpy(&received_packet.payload, buf.data() + header_size, copy_len);

      // Process the extracted message
      process_tcp_command(client, received_packet);

      // Check if client got disconnected during processing BEFORE erasing from
      // buffer
      if (!client || client->socketfd < 0)
        return;  // Exit if client disconnected

      // Remove the processed message from the buffer
      buf.erase(buf.begin(), buf.begin() + total_msg_len);
      message_processed = true;  // Indicate we processed a message and should
                                 // check buffer again

    } else {
      // cerr << "Incomplete message in buffer (type " << msg_type << ", payload
      // " << payload_len << ", total " << total_msg_len << "). Need " <<
      // total_msg_len << ", have " << buf_size << ". Waiting for more data." <<
      // endl;
      break;  // Not enough data for the full message yet
    }

  } while (message_processed);  // Loop if we processed a message, as there
                                // might be another one
}

void ClientConnections::process_tcp_command(shared_ptr<ClientState> client,
                                            const ChatPacket& packet) {
  if (!client) return;

  // --- Handle ID message first (special case) ---
  if (!client->connected) {
    if (packet.type == MSG_ID) {
      // Validate ID length (payload length from header)
      if (packet.len == 0 || packet.len > MAX_CLIENT_ID_SIZE) {
        cerr << "ERROR: Invalid ID length (" << packet.len << ") from fd "
             << client->socketfd << ". Disconnecting." << endl;
        disconnect_tcp_client(client->socketfd);
        return;
      }

      // Ensure null termination for safety when creating std::string
      char client_id_cstr[MAX_CLIENT_ID_SIZE + 1];  // +1 for safety null term
      // Use packet.len for strncpy length, as it's validated against
      // MAX_CLIENT_ID_SIZE Note: packet.len includes the null terminator sent
      // by the client.
      size_t copy_len = packet.len;
      if (copy_len > MAX_CLIENT_ID_SIZE + 1)
        copy_len = MAX_CLIENT_ID_SIZE + 1;  // Sanity limit
      strncpy(client_id_cstr, packet.payload.id, copy_len);
      client_id_cstr[copy_len > 0 ? copy_len - 1 : 0] =
          '\0';  // Ensure null termination at copied length or 0 if empty
      string client_id_str(client_id_cstr);

      // Check if ID is already connected
      if (client_by_id.count(client_id_str)) {
        shared_ptr<ClientState> existing_client = client_by_id[client_id_str];
        if (existing_client && existing_client->connected) {
          // Client ID already in use and connected
          printf("Client %s already connected.\n",
                 client_id_str.c_str());  // Required output

          // Send error message back to the *new* connection attempt
          ChatPacket err_packet;
          memset(&err_packet, 0, sizeof(err_packet));
          const char* errMsg = "EINUSE";
          err_packet.len = strlen(errMsg) + 1;
          err_packet.type = MSG_ERROR;
          strncpy(err_packet.payload.text, errMsg, err_packet.len);
          // Queue the error message (non-blocking)
          send_message_to_client(client, err_packet);
          // Disconnect the *new* connection attempt shortly after sending error
          // Maybe add a flag to disconnect after write finishes? For now,
          // disconnect immediately.
          disconnect_tcp_client(client->socketfd);
          return;  // Stop processing for this fd
        } else {
          // Reconnect scenario: ID exists but is not currently connected
          // Update the existing ClientState object with the new socket fd
          // The existing_client object holds the persistent subscriptions.
          cerr << "Client " << client_id_str << " reconnected." << endl;

          // Remove the old fd mapping if it somehow still exists (shouldn't)
          if (existing_client && existing_client->socketfd >= 0 &&
              existing_client->socketfd != client->socketfd) {
            cerr << "WARN: Found old socket fd " << existing_client->socketfd
                 << " for reconnecting client " << client_id_str
                 << ". Cleaning up." << endl;
            // Use the existing disconnect logic which handles poll removal etc.
            disconnect_tcp_client(existing_client->socketfd);
          }

          // Update the existing client state with the new fd and connected
          // status
          existing_client->socketfd = client->socketfd;
          existing_client->connected = true;
          // Keep existing recv_buffer and send_queue for potential SF (if
          // implemented) Clear them if SF is NOT implemented or desired on
          // reconnect. For now, let's clear them for simplicity unless SF is
          // explicitly handled.
          existing_client->recv_buffer.clear();
          existing_client->send_queue.clear();
          existing_client->current_send_offset = 0;

          // Update the fd -> client mapping to point to the *existing* state
          // object The current 'client' shared_ptr (pointing to the temporary
          // state created in connect_tcp_client) will go out of scope. We need
          // to update the map entry for the *new* fd to point to the persistent
          // existing_client object.
          client_by_fd[client->socketfd] = existing_client;

          // Print connection message
          struct sockaddr_in peer_addr;
          socklen_t peer_len = sizeof(peer_addr);
          // Use the new socket fd to get peer name
          if (getpeername(existing_client->socketfd,
                          (struct sockaddr*)&peer_addr, &peer_len) == 0) {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &peer_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
            printf("New client %s connected from %s:%hu.\n",
                   client_id_str.c_str(), ip_str,
                   ntohs(peer_addr.sin_port));  // Required output
          } else {
            perror("getpeername on reconnect");
            printf("New client %s connected from <unknown>.\n",
                   client_id_str.c_str());
          }

          // Subscriptions are already in existing_client->subscriptions.
          // No need to add to topic_subscribers here, it's done during
          // subscribe. The broadcast logic will find this client by ID and
          // check its 'connected' status.

          // Client is now fully connected
          return;  // Finished processing ID for reconnect
        }
      } else {
        // New client connection
        client->id = client_id_str;
        client->connected = true;
        client_by_id[client_id_str] =
            client;  // Add to ID map (persistent storage)
        // client_by_fd[client->socketfd] is already set in connect_tcp_client

        // Print connection message
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        if (getpeername(client->socketfd, (struct sockaddr*)&peer_addr,
                        &peer_len) == 0) {
          char ip_str[INET_ADDRSTRLEN];
          inet_ntop(AF_INET, &peer_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
          printf("New client %s connected from %s:%hu.\n",
                 client_id_str.c_str(), ip_str,
                 ntohs(peer_addr.sin_port));  // Required output
        } else {
          perror("getpeername for new client");
          printf("New client %s connected from <unknown>.\n",
                 client_id_str.c_str());
        }

        // Client is now fully connected. Subscriptions are empty initially.
        return;  // Finished processing ID for new client
      }
    } else {
      // First message was not MSG_ID
      cerr << "ERROR: First message from fd " << client->socketfd
           << " was not MSG_ID (type=" << packet.type << "). Disconnecting."
           << endl;
      disconnect_tcp_client(client->socketfd);
      return;
    }
  }

  // --- Handle other message types from already connected clients ---

  // Create a local mutable copy for potential modification (e.g., null term)
  char payload_buffer[MAX_MSG_SIZE + 1];  // Use appropriate max size
  size_t copy_len = packet.len;
  if (copy_len > MAX_MSG_SIZE) {
    copy_len = MAX_MSG_SIZE;  // Ensure we don't copy too much
  }
  // Ensure copy_len is not negative if packet.len was somehow 0
  if (copy_len == 0 && packet.len > 0) {
    // This case might indicate an issue, but proceed with 0 copy_len
    cerr << "WARN: packet.len > 0 but copy_len became 0." << endl;
  } else if (copy_len > 0) {
    memcpy(payload_buffer, packet.payload.text, copy_len);
  }
  // Ensure null termination for string operations. packet.len includes the null
  // terminator. So, the actual string data is packet.len - 1 bytes.
  if (packet.len > 0 && packet.len <= MAX_MSG_SIZE) {
    payload_buffer[packet.len - 1] =
        '\0';  // Null terminate at the end of the string data
  } else {
    payload_buffer[0] = '\0';  // Ensure empty string if length is 0 or invalid
  }

  // Use payload_buffer for subsequent operations instead of packet.payload.text
  char* payload_text = payload_buffer;

  switch (packet.type) {
    case MSG_SUBSCRIBE:
    case MSG_UNSUBSCRIBE: {
      // Payload should be "command topic_pattern"
      // Example: "subscribe topicA" or "unsubscribe topicB"
      string full_command(payload_text);  // Use the local buffer
      size_t first_space_pos = full_command.find(' ');

      if (first_space_pos == string::npos) {
        cerr << "Error: Malformed subscribe/unsubscribe command from "
             << client->id << ": '" << full_command << "'. No space found."
             << endl;
        break;  // Ignore malformed command
      }

      string command = full_command.substr(0, first_space_pos);
      string topic_pattern = full_command.substr(first_space_pos + 1);

      // Basic validation for topic pattern (not empty)
      if (topic_pattern.empty()) {
        cerr << "Error: Subscribe/unsubscribe command from " << client->id
             << " has empty topic pattern." << endl;
        break;  // Skip invalid command
      }
      // Further validation based on spec (no spaces in topic, valid wildcard
      // usage?) The spec says topics are "şiruri de caractere ASCII fără
      // spații". This implies the topic_pattern itself should not contain
      // spaces after extraction.
      if (topic_pattern.find(' ') != string::npos) {
        cerr << "Error: Topic pattern contains spaces from client "
             << client->id << ": '" << topic_pattern << "'" << endl;
        break;  // Skip invalid command
      }
      // The wildcard rules (+ and *) are handled by match_topic.
      // We don't need to validate the structure of the pattern here, just its
      // presence.

      if (packet.type == MSG_SUBSCRIBE) {
        subscribe_client_to_topic(client, topic_pattern);
      } else {  // MSG_UNSUBSCRIBE
        unsubscribe_client_from_topic(client, topic_pattern);
      }
      break;
    }

    case MSG_EXIT:
      // Check payload content just in case (client should send "exit")
      if (strcmp(payload_text, "exit") == 0) {  // Use local buffer
        cerr << "Client " << client->id
             << " requested disconnect via EXIT message." << endl;
        disconnect_tcp_client(client->socketfd);
      } else {
        cerr << "WARN: Received MSG_EXIT type from " << client->id
             << " but payload is not 'exit': '" << payload_text
             << "'. Disconnecting anyway." << endl;
        disconnect_tcp_client(client->socketfd);
      }
      break;

    case MSG_ID:  // Should not happen after initial connection
      cerr << "WARN: Received unexpected MSG_ID from already connected client "
           << client->id << ". Ignoring." << endl;
      break;

    case MSG_ERROR:  // Client should not send errors
      cerr << "WARN: Received MSG_ERROR from client " << client->id
           << ". Ignoring." << endl;
      break;
    // Assuming MSG_UDP_FORWARD is defined in MessageType enum in common.h
    case MSG_UDP_FORWARD:  // Client should not send forwarded messages
      cerr << "WARN: Received MSG_UDP_FORWARD from client " << client->id
           << ". Ignoring." << endl;
      break;
    default:
      cerr << "WARN: Received unknown/unexpected message type (" << packet.type
           << ") from client " << client->id << ". Ignoring." << endl;
      break;
  }
}

// --- Subscription Management ---

void ClientConnections::subscribe_client_to_topic(
    shared_ptr<ClientState> client, const string& topic_pattern) {
  if (!client || client->id.empty()) {  // Need valid client and ID
    cerr << "Error: Attempted to subscribe invalid client." << endl;
    return;
  }

  // Add topic pattern to client's personal list (persists across disconnects)
  client->subscriptions.insert(topic_pattern);

  // Add client ID to the global map for this topic pattern (used for
  // broadcasting)
  topic_subscribers[topic_pattern].insert(client->id);

  cerr << "Client " << client->id << " subscribed to pattern: " << topic_pattern
       << endl;
  // Note: Client expects "Subscribed to topic X" - this needs to be sent back
  // by server? The spec says client prints this *after sending* the command.
  // Server doesn't need to confirm.
}

void ClientConnections::unsubscribe_client_from_topic(
    shared_ptr<ClientState> client, const string& topic_pattern) {
  if (!client || client->id.empty()) {  // Need valid client and ID
    cerr << "Error: Attempted to unsubscribe invalid client." << endl;
    return;
  }

  // Remove pattern from client's personal list
  client->subscriptions.erase(topic_pattern);

  // Remove client ID from the global map for this topic pattern
  auto it_topic = topic_subscribers.find(topic_pattern);
  if (it_topic != topic_subscribers.end()) {
    it_topic->second.erase(client->id);
    // If no more clients are subscribed to this pattern (connected or
    // disconnected), remove the pattern entry
    if (it_topic->second.empty()) {
      topic_subscribers.erase(it_topic);
      cerr << "Topic pattern " << topic_pattern
           << " removed as no clients are subscribed." << endl;
    }
  }

  cerr << "Client " << client->id
       << " unsubscribed from pattern: " << topic_pattern << endl;
  // Note: Client expects "Unsubscribed from topic X" - server doesn't need to
  // confirm.
}

// Removed remove_client_subscriptions function as subscriptions persist.

// --- Main Event Loop ---

int ClientConnections::pollAll() {
  // Timeout for poll can be -1 (infinite), 0 (non-blocking check), or positive
  // ms
  int poll_timeout_ms = 100;  // Check frequently (e.g., 100ms)

  // cerr << "Polling " << num_fds << " fds..." << endl;
  int rc = poll(poll_fds, num_fds, poll_timeout_ms);
  // cerr << "Poll returned " << rc << endl;

  if (rc < 0) {
    // EINTR might happen, should potentially restart poll
    if (errno == EINTR) {
      cerr << "poll interrupted, retrying..." << endl;
      return 0;  // Indicate loop should continue
    }
    // Other errors are more serious
    perror("poll");
    return 1;  // Signal exit on critical poll error
  }

  if (rc == 0) {
    // Timeout occurred (only if poll_timeout_ms > 0)
    // cerr << "poll timed out" << endl;
    return 0;  // Continue loop
  }

  // Iterate through descriptors that *might* have events
  // Need to iterate carefully as handlers might modify poll_fds via remove_fd
  // Iterate backwards or use index carefully
  for (int i = num_fds - 1; i >= 0; --i) {
    // Check if fd at index i still exists (num_fds might have changed during
    // loop)
    if (i >= num_fds) {
      // cerr << "Skipping index " << i << " as num_fds is now " << num_fds <<
      // endl;
      continue;
    }

    // Make a copy of the pollfd struct at index i BEFORE potentially modifying
    // the array
    struct pollfd current_pollfd = poll_fds[i];
    int current_fd = current_pollfd.fd;

    // Check if the fd is still valid (might have been closed and removed by a
    // previous iteration)
    auto type_it = fd_to_type.find(current_fd);
    if (type_it == fd_to_type.end()) {
      // This fd was likely removed in a previous step of this loop iteration.
      // Skip. cerr << "Skipping fd " << current_fd << " at index " << i << " as
      // it's no longer mapped." << endl;
      continue;
    }
    SocketType type = type_it->second;

    if (current_pollfd.revents == 0) {
      continue;  // No events for this fd
    }

    // --- Handle Errors First ---
    if (current_pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      cerr << "Error/Hangup/Invalid event on fd " << current_fd
           << " (type: " << type << ", revents: " << current_pollfd.revents
           << ")" << endl;
      if (type == TCP_LISTEN_SOCKET || type == UDP_LISTEN_SOCKET ||
          type == STDIN_SOCKET) {
        fprintf(stderr,
                "Critical error on listening socket or stdin. Exiting.\n");
        onExit();  // Try to notify clients before exiting
        return 1;  // Signal exit
      } else if (type == TCP_CLIENT_SOCKET) {
        // Error on client socket, treat as disconnect
        disconnect_tcp_client(current_fd);  // This calls remove_fd
      } else {
        // Should not happen
        close(current_fd);
        remove_fd(current_fd);
      }
      continue;  // Move to next fd index (remember we iterate backwards)
    }

    // --- Handle Readable Events ---
    if (current_pollfd.revents & POLLIN) {
      // cerr << "POLLIN on fd " << current_fd << " (type: " << type << ")" <<
      // endl;
      if (type == TCP_LISTEN_SOCKET) {
        // Handle new connection attempts (can accept multiple in a row if
        // ready) Loop accept until EAGAIN if listening socket is non-blocking
        // For simplicity, accept one per POLLIN event if blocking.
        // If non-blocking, loop: while(connect_tcp_client());
        connect_tcp_client();  // Accept one connection
      } else if (type == UDP_LISTEN_SOCKET) {
        // Handle incoming UDP message (read one datagram)
        // Loop recvfrom until EAGAIN for UDP
        while (true) {
          handle_udp_message(current_fd);
          // Check errno after handle_udp_message returns if it hit EAGAIN
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            errno = 0;  // Reset errno
            break;      // No more datagrams for now
          }
          // If recvfrom had another error, handle_udp_message printed it.
          // If recvfrom succeeded, loop again.
        }

      } else if (type == STDIN_SOCKET) {
        // Handle input from stdin
        if (handle_stdin()) {
          onExit();  // Send exit to clients
          return 1;  // Exit command received
        }
      } else if (type == TCP_CLIENT_SOCKET) {
        // Data from existing TCP client
        auto it = client_by_fd.find(current_fd);
        if (it != client_by_fd.end()) {
          handle_client_read(it->second);
          // handle_client_read might disconnect, check if fd still valid before
          // POLLOUT check
          if (fd_to_type.find(current_fd) == fd_to_type.end()) {
            continue;  // Client was disconnected, skip POLLOUT check
          }
        } else {
          cerr << "WARN: POLLIN event for fd " << current_fd
               << " but no client state found in client_by_fd. Closing."
               << endl;
          close(current_fd);
          remove_fd(current_fd);  // Clean up poll set
          continue;               // Skip POLLOUT check
        }
      }
    }

    // --- Handle Writable Events ---
    // Must check AFTER potential disconnect in POLLIN handler
    // Check if fd still exists in map before proceeding
    if (fd_to_type.count(current_fd) &&
        fd_to_type[current_fd] == TCP_CLIENT_SOCKET &&
        (current_pollfd.revents & POLLOUT)) {
      // cerr << "POLLOUT on fd " << current_fd << " (type: " << type << ")" <<
      // endl;
      auto it = client_by_fd.find(current_fd);
      if (it != client_by_fd.end()) {
        handle_client_write(it->second);
      } else {
        cerr << "WARN: POLLOUT event for fd " << current_fd
             << " but no client state found in client_by_fd. Removing POLLOUT."
             << endl;
        // Find index and remove POLLOUT event bit - careful with iterating
        // backwards
        int poll_idx_now =
            find_poll_index(current_fd);  // Find current index again
        if (poll_idx_now != -1 && poll_idx_now < num_fds) {  // Check bounds
          poll_fds[poll_idx_now].events &= ~POLLOUT;
        }
      }
    }

  }  // End for loop iterating through poll_fds

  return 0;  // Signal loop should continue
}
