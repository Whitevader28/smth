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

#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "helpers.h"

int create_tcp_listenfd(uint16_t port, const char* ip) {
  const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  DIE(listenfd < 0, "socket");

  // Allow address reuse
  int enable = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    perror("setsockopt(SO_REUSEADDR) failed");

  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  int rc = inet_pton(AF_INET, ip, &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton");

  rc = bind(listenfd, (const struct sockaddr*)&serv_addr, sizeof(serv_addr));
  DIE(rc < 0, "bind");

  disable_nagle(listenfd);

  return listenfd;
}

int create_udp_listenfd(uint16_t port, const char* ip) {
  const int listenfd = socket(AF_INET, SOCK_DGRAM, 0);
  DIE(listenfd < 0, "socket UDP");

  // Allow address reuse
  int enable = 1;
  if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
    perror("setsockopt(SO_REUSEADDR) failed for UDP");

  // Set non-blocking
  if (!set_nonblocking(listenfd)) {
    close(listenfd);
    DIE(1, "set_nonblocking listenfd_udp");
  }

  struct sockaddr_in serv_addr;
  socklen_t socket_len = sizeof(struct sockaddr_in);

  memset(&serv_addr, 0, socket_len);
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(port);
  int rc = inet_pton(AF_INET, ip, &serv_addr.sin_addr.s_addr);
  DIE(rc <= 0, "inet_pton UDP");

  rc = bind(listenfd, (const struct sockaddr*)&serv_addr, sizeof(serv_addr));
  DIE(rc < 0, "bind UDP");

  return listenfd;
}

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

int ClientConnections::find_poll_index(int fd) {
  for (int i = 0; i < num_fds; ++i)
    if (poll_fds[i].fd == fd) return i;

  return -1;  // Not found
}

void ClientConnections::add_fd(int fd, short int events, SocketType type) {
  if (num_fds >=
      MAX_TCP_CONNECTIONS + 3) {  // +3 for stdin, tcp_listen, udp_listen
    cerr << "Error: Maximum number of fds reached. Cannot add fd " << fd
         << endl;

    if (type == TCP_CLIENT_SOCKET) close(fd);
    return;
  }

  int index = num_fds;
  poll_fds[index].fd = fd;
  poll_fds[index].events = events;
  poll_fds[index].revents = 0;
  fd_to_type[fd] = type;
  num_fds++;
}

// Removes fd from poll_fds and associated maps
void ClientConnections::remove_fd(int fd) {
  int pos = find_poll_index(fd);
  if (pos < 0) {
    fd_to_type.erase(fd);
    return;
  }

  fd_to_type.erase(fd);

  for (int j = pos; j < num_fds - 1; j++) poll_fds[j] = poll_fds[j + 1];

  --num_fds;
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
      return;
    }
    perror("accept");
    return;
  }

  disable_nagle(newsockfd);

  if (!set_nonblocking(newsockfd)) {
    cerr << "Error setting non-blocking for client fd " << newsockfd << endl;
    close(newsockfd);
    return;
  }

  // We add it *before* receiving the ID. We'll read the ID via the normal poll
  auto new_client = make_shared<ClientState>(newsockfd);
  client_by_fd[newsockfd] = new_client;
  add_fd(newsockfd, POLLIN,
         TCP_CLIENT_SOCKET);  // Initially just listen for read (for ID)

  char ip_str[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &client_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
  cerr << "Accepted connection from " << ip_str << ":"
       << ntohs(client_addr.sin_port) << " on fd " << newsockfd
       << ". Waiting for ID." << endl;
}

void ClientConnections::disconnect_tcp_client(int fd) {
  auto it_fd = client_by_fd.find(fd);
  if (it_fd != client_by_fd.end()) {
    shared_ptr<ClientState> client = it_fd->second;

    if (client && client->connected) {
      printf("Client %s disconnected.\n", client->id.c_str());
    } else {
      cerr << "Client on fd " << fd
           << " disconnected before sending/validating ID or client state "
              "invalid."
           << endl;
    }

    // Mark client as disconnected
    // Keep his id in client_by_id for potential reconnections
    if (client) {
      client->connected = false;
      client->socketfd = -1;
    } else {
      cerr << "invalid client pointer for fd in disconnect_tcp_client: " << fd
           << endl;
    }

    client_by_fd.erase(it_fd);
  } else {
    cerr << "disconnect_tcp_client called for fd " << fd
         << " has no entry in client_by_fd." << endl;
  }

  close(fd);
  remove_fd(fd);
}

void ClientConnections::onExit() {
  ChatPacket packet;
  memset(&packet, 0, sizeof(packet));
  packet.len = strlen("exit") + 1;  // Include '\0'
  packet.type = MSG_EXIT;
  strncpy(packet.payload.text, "exit", packet.len);

  cerr << "Server closing. Sending message to all clients" << endl;

  // Notify connected clients
  for (auto const& [fd, client_ptr] : client_by_fd)
    if (client_ptr && client_ptr->connected && client_ptr->socketfd >= 0)
      send_message_to_client(client_ptr, packet);
}

int ClientConnections::handle_stdin() {
  char buf[MAX_MSG_SIZE + 1];
  memset(buf, 0, sizeof(buf));

  int rc = read(STDIN_FILENO, buf, MAX_MSG_SIZE);
  if (rc < 0) {
    perror("read stdin");
    return 0;
  }

  if (rc == 0) return 0;  // Don't tread EOF as a command

  // Remove trailing newline if present
  if (rc > 0 && buf[rc - 1] == '\n') {
    buf[rc - 1] = '\0';
  } else {
    buf[rc] = '\0';
  }

  if (strcmp(buf, "exit") == 0) {
    cerr << "Exit command received from stdin." << endl;
    return 1;  // Signal to exit
  } else {
    cerr << "Unknown command from stdin: '" << buf << "'. Use 'exit' to quit."
         << endl;
  }

  return 0;
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

  // dp[i][j] is true if the first i levels of the topic match the first j
  // levels of the pattern
  vector<vector<bool>> dp(topic_levels.size() + 1,
                          vector<bool>(pattern_levels.size() + 1, false));

  // Base case: Empty topic and empty pattern match
  dp[0][0] = true;

  // Handle patterns starting with '*'
  if (!pattern_levels.empty() && pattern_levels[0] == "*") {
    dp[0][1] = true;
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
  char raw_udp_buf[MAX_UDP_MSG_SIZE + 1];
  memset(raw_udp_buf, 0, sizeof(raw_udp_buf));

  struct sockaddr_in client_addr;
  memset(&client_addr, 0, sizeof(client_addr));
  socklen_t client_len = sizeof(client_addr);

  int rc = recvfrom(udp_fd, raw_udp_buf, MAX_UDP_MSG_SIZE, 0,
                    (struct sockaddr*)&client_addr, &client_len);

  if (rc < 0) {
    // EAGAIN or EWOULDBLOCK is expected on non-blocking UDP socket
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;  // No datagram available right now

    perror("recvfrom UDP");
    return;
  }
  if (rc == 0) {
    cerr << "WARN: recvfrom UDP returned 0." << endl;
    return;
  }

  if (rc < 50 + 1) {  // Need at least 51 bytes (50 for topic + 1 for type)
    cerr << "Error: Received UDP packet too short for topic+type (" << rc
         << " bytes)" << endl;
    return;
  }

  char topic_name[51];  // Buffer for topic name
  strncpy(topic_name, raw_udp_buf, 50);
  topic_name[50] = '\0';
  string udp_topic_str(topic_name);

  // Extract the message for TCP clients and construct the response
  ChatPacket packet_to_tcp;
  memset(&packet_to_tcp, 0, sizeof(packet_to_tcp));
  packet_to_tcp.type = MSG_UDP_FORWARD;

  char* tcp_msg_payload = packet_to_tcp.payload.text;
  int tcp_payload_len = 0;

  // 0. Get data type
  uint8_t data_type = (uint8_t)raw_udp_buf[50];

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
  char formatted_value[MAX_CONTENT_SIZE + 20] = {0};
  const char* udp_content_ptr = raw_udp_buf + 51;  // Start of content
  int udp_content_len = rc - 51;

  // Check if received data is sufficient for the declared type
  bool data_ok = true;
  uint8_t sign;
  uint32_t int_val;
  uint16_t short_real_val;
  uint32_t float_mod;
  uint8_t float_pow;

  switch (data_type) {
    case 0:  // INT
    {
      type_str = "INT";
      if (udp_content_len < 1 + 4) {
        data_ok = false;
        break;
      }
      sign = (uint8_t)udp_content_ptr[0];
      memcpy(&int_val, udp_content_ptr + 1, sizeof(uint32_t));
      int_val = ntohl(int_val);
      snprintf(formatted_value, sizeof(formatted_value), "%s%u",
               (sign == 1 && int_val > 0 ? "-" : ""), int_val);
      break;
    }

    case 1:  // SHORT_REAL
    {
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
    }

    case 2:  // FLOAT
    {
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
      double float_val = (double)float_mod;
      if (float_pow > 0) {
        float_val /= pow(10.0, float_pow);
      }
      // Use snprintf for safe formatting. %g might remove trailing zeros.
      // Let's control precision more directly if needed, or stick to %g.
      snprintf(formatted_value, sizeof(formatted_value), "%s%.10g",
               (sign == 1 && float_val > 0 ? "-" : ""), float_val);
      break;
    }

    case 3:  // STRING
    {
      type_str = "STRING";
      // Content starts at udp_content_ptr. Max length is udp_content_len.
      int copy_len = udp_content_len;  // Initialization inside the scope
      if (copy_len < 0) copy_len = 0;  // Safety check
      if (copy_len > MAX_CONTENT_SIZE) {
        cerr << "WARN: UDP String content longer than MAX_CONTENT_SIZE ("
             << copy_len << "), truncating." << endl;
        copy_len = MAX_CONTENT_SIZE;
      }

      memcpy(formatted_value, udp_content_ptr, copy_len);
      formatted_value[copy_len] = '\0';
      break;
    }

    default: {
      data_ok = false;
      break;
    }
  }

  if (!data_ok) {
    cerr << "Error: Invalid or incomplete UDP data for type " << (int)data_type
         << " in packet from " << ip_str << ":" << ntohs(client_addr.sin_port)
         << " (content len: " << udp_content_len << ")" << endl;
    snprintf(formatted_value, sizeof(formatted_value), "[Invalid Data]");

    // Drop the packet
    return;
  }

  // 4. Append TYPE string
  if (MAX_MSG_SIZE > tcp_payload_len)
    tcp_payload_len +=
        snprintf(tcp_msg_payload + tcp_payload_len,
                 MAX_MSG_SIZE - tcp_payload_len, "%s - ", type_str);

  // 5. Append formatted VALUE
  if (MAX_MSG_SIZE > tcp_payload_len)
    tcp_payload_len +=
        snprintf(tcp_msg_payload + tcp_payload_len,
                 MAX_MSG_SIZE - tcp_payload_len, "%s", formatted_value);

  if (tcp_payload_len >= MAX_MSG_SIZE) {
    tcp_payload_len = MAX_MSG_SIZE - 1;  // Space for '\0'
    cerr << "WARN: Constructed TCP payload truncated to fit MAX_MSG_SIZE."
         << endl;
  }

  tcp_msg_payload[tcp_payload_len] = '\0';
  packet_to_tcp.len = tcp_payload_len + 1;

  broadcast_udp_message(udp_topic_str, packet_to_tcp);
}

void ClientConnections::broadcast_udp_message(const string& udp_topic,
                                              const ChatPacket& tcp_packet) {
  for (const auto& id_client_pair : client_by_id) {
    shared_ptr<ClientState> client = id_client_pair.second;

    // Send the message only to connected and subscribed clients
    if (client && client->connected && client->socketfd >= 0) {
      bool is_subscribed = false;
      for (const string& pattern : client->subscriptions) {
        if (match_topic(udp_topic, pattern)) {
          is_subscribed = true;
          break;  // Found a matching subscription pattern
        }
      }

      if (is_subscribed) {
        send_message_to_client(client, tcp_packet);
      }
    }
  }
}

void ClientConnections::send_message_to_client(shared_ptr<ClientState> client,
                                               const ChatPacket& packet) {
  if (!client || !client->connected || client->socketfd < 0) {
    return;
  }

  // Ensure type is sent as 4 bytes
  uint16_t net_len = htons(packet.len);
  uint32_t net_type = htonl(static_cast<int>(packet.type));
  size_t header_size = sizeof(net_len) + sizeof(net_type);

  size_t max_allowed_payload_for_send =
      (packet.type == MSG_ID) ? MAX_CLIENT_ID_SIZE : MAX_MSG_SIZE;

  if (packet.len > (MAX_MSG_SIZE + MAX_CLIENT_ID_SIZE + 100)) {
    cerr << "ERROR: send_message_to_client too big packet=" << packet.len
         << ". Dropping message for client " << client->id << "." << endl;
    return;
  }
  if (packet.len > max_allowed_payload_for_send && packet.type != MSG_ID) {
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

    payload_copy_len = total_size > header_size ? total_size - header_size : 0;
  }

  memcpy(message_buffer.data() + header_size, &packet.payload,
         payload_copy_len);

  client->send_queue.push_back(std::move(message_buffer));

  // Check if we can start sending immediately (if queue was empty)
  // and register for POLLOUT events if needed
  int poll_idx = find_poll_index(client->socketfd);
  if (poll_idx != -1) {
    // If the send queue was empty before adding this message,
    // we might be able to send immediately
    if (client->send_queue.size() == 1 && client->current_send_offset == 0) {
      handle_client_write(client);  // Attempt initial send
    }

    // Make sure POLLOUT is registered if there's data pending
    if (!client->send_queue.empty()) {
      if (!(poll_fds[poll_idx].events & POLLOUT))
        poll_fds[poll_idx].events |= POLLOUT;
    } else {
      // Should not happen right after adding, but for completeness:
      // If queue becomes empty, remove POLLOUT
      if (poll_fds[poll_idx].events & POLLOUT)
        poll_fds[poll_idx].events &= ~POLLOUT;
    }
  } else {
    cerr << "ERROR: Cannot find client fd " << client->socketfd
         << " in poll set to register POLLOUT." << endl;
  }
}

void ClientConnections::handle_client_read(shared_ptr<ClientState> client) {
  if (!client || client->socketfd < 0) return;

  char read_buf[4096];
  int bytes_read =
      recv_nonblocking(client->socketfd, read_buf, sizeof(read_buf));

  if (bytes_read == -1) {
    cerr << "Error reading from client fd " << client->socketfd
         << " (ID: " << client->id << "). Disconnecting." << endl;
    disconnect_tcp_client(client->socketfd);
  } else if (bytes_read == 0) {  // Connection closed by peer
    disconnect_tcp_client(client->socketfd);
  } else if (bytes_read == -2) {
    // EAGAIN / EWOULDBLOCK - no data available right now
    return;
  } else {  // Data received
    client->recv_buffer.insert(client->recv_buffer.end(), read_buf,
                               read_buf + bytes_read);
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
        poll_fds[poll_idx].events &= ~POLLOUT;
      }
    }
    return;
  }

  // Get the current message chunk to send
  vector<char>& current_message = client->send_queue.front();
  size_t bytes_to_send = current_message.size() - client->current_send_offset;
  const char* send_ptr = current_message.data() + client->current_send_offset;

  int bytes_sent = send_nonblocking(client->socketfd, send_ptr, bytes_to_send);

  if (bytes_sent == -1) {  // Genuine error
    cerr << "Error writing to client fd " << client->socketfd
         << " (ID: " << client->id << "). Disconnecting." << endl;
    disconnect_tcp_client(client->socketfd);  // Error -> disconnect
  } else if (bytes_sent == 0) {  // EAGAIN / EWOULDBLOCK (or sent 0)
    int poll_idx = find_poll_index(client->socketfd);
    if (poll_idx != -1 && !(poll_fds[poll_idx].events & POLLOUT)) {
      poll_fds[poll_idx].events |= POLLOUT;
    }
  } else {  // Successfully sent some bytes
    client->current_send_offset += bytes_sent;

    // Check if the current message is fully sent
    if (client->current_send_offset == current_message.size()) {
      client->send_queue.pop_front();
      client->current_send_offset = 0;

      // If the queue is now empty, remove POLLOUT interest
      if (client->send_queue.empty()) {
        int poll_idx = find_poll_index(client->socketfd);
        if (poll_idx != -1 && (poll_fds[poll_idx].events & POLLOUT))
          poll_fds[poll_idx].events &= ~POLLOUT;
      } else {
        int poll_idx = find_poll_index(client->socketfd);
        if (poll_idx != -1 && !(poll_fds[poll_idx].events & POLLOUT)) {
          poll_fds[poll_idx].events |= POLLOUT;
        }
      }
    } else {
      // Message partially sent, ensure POLLOUT is set to try again later
      int poll_idx = find_poll_index(client->socketfd);
      if (poll_idx != -1 && !(poll_fds[poll_idx].events & POLLOUT)) {
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
    size_t header_size = sizeof(uint16_t) + sizeof(uint32_t);

    // Need at least enough data for the header
    if (buf_size < header_size) {
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
      ChatPacket received_packet;
      memset(&received_packet, 0, sizeof(received_packet));
      received_packet.len = payload_len;
      received_packet.type = msg_type;

      // Safely copy payload, respecting buffer sizes
      size_t copy_len = payload_len;
      if (copy_len > sizeof(received_packet.payload)) {
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
      break;  // Not enough data for the full message yet
    }

  } while (message_processed);  // Loop if we processed a message, as there
                                // might be another one
}

void ClientConnections::process_tcp_command(shared_ptr<ClientState> client,
                                            const ChatPacket& packet) {
  if (!client) return;

  if (!client->connected) {
    if (packet.type == MSG_ID) {
      // Validate ID length (payload length from header)
      if (packet.len == 0 || packet.len > MAX_CLIENT_ID_SIZE) {
        cerr << "ERROR: Invalid ID length (" << packet.len << ") from fd "
             << client->socketfd << ". Disconnecting." << endl;
        disconnect_tcp_client(client->socketfd);
        return;
      }

      char client_id_cstr[MAX_CLIENT_ID_SIZE + 1];
      size_t copy_len = packet.len;
      if (copy_len > MAX_CLIENT_ID_SIZE + 1) copy_len = MAX_CLIENT_ID_SIZE + 1;

      strncpy(client_id_cstr, packet.payload.id, copy_len);
      client_id_cstr[copy_len > 0 ? copy_len - 1 : 0] = '\0';
      string client_id_str(client_id_cstr);

      // Check if ID is already connected
      if (client_by_id.count(client_id_str)) {
        shared_ptr<ClientState> existing_client = client_by_id[client_id_str];
        if (existing_client && existing_client->connected) {
          // Client ID already in use and connected
          printf("Client %s already connected.\n", client_id_str.c_str());

          // Send error message back
          ChatPacket err_packet;
          memset(&err_packet, 0, sizeof(err_packet));
          const char* errMsg = "EINUSE";
          err_packet.len = strlen(errMsg) + 1;
          err_packet.type = MSG_ERROR;
          strncpy(err_packet.payload.text, errMsg, err_packet.len);
          // Queue the error message (non-blocking)
          send_message_to_client(client, err_packet);
          disconnect_tcp_client(client->socketfd);
          return;
        } else {
          // Reconnect client
          cerr << "Client " << client_id_str << " reconnected." << endl;

          if (existing_client && existing_client->socketfd >= 0 &&
              existing_client->socketfd != client->socketfd) {
            cerr << "WARN: Found old socket fd " << existing_client->socketfd
                 << " for reconnecting client " << client_id_str
                 << ". Cleaning up." << endl;
            // Use the existing disconnect logic which handles poll removal etc.
            disconnect_tcp_client(existing_client->socketfd);
          }

          existing_client->socketfd = client->socketfd;
          existing_client->connected = true;
          existing_client->recv_buffer.clear();
          existing_client->send_queue.clear();
          existing_client->current_send_offset = 0;

          client_by_fd[client->socketfd] = existing_client;

          // Print connection message
          struct sockaddr_in peer_addr;
          socklen_t peer_len = sizeof(peer_addr);
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

          return;
        }
      } else {
        // New client connection
        client->id = client_id_str;
        client->connected = true;
        client_by_id[client_id_str] = client;

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

        return;
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

  // Create a local mutable copy for potential modification (e.g., null term)
  char payload_buffer[MAX_MSG_SIZE + 1];
  size_t copy_len = packet.len;
  if (copy_len > MAX_MSG_SIZE) {
    copy_len = MAX_MSG_SIZE;
  }
  if (copy_len == 0 && packet.len > 0) {
    cerr << "WARN: packet.len > 0 but copy_len became 0." << endl;
  } else if (copy_len > 0) {
    memcpy(payload_buffer, packet.payload.text, copy_len);
  }
  if (packet.len > 0 && packet.len <= MAX_MSG_SIZE) {
    payload_buffer[packet.len - 1] =
        '\0';  // Null terminate at the end of the string data
  } else {
    payload_buffer[0] = '\0';
  }

  // Use payload_buffer for subsequent operations instead of packet.payload.text
  char* payload_text = payload_buffer;

  switch (packet.type) {
    case MSG_SUBSCRIBE:
    case MSG_UNSUBSCRIBE: {
      string full_command(payload_text);  // Use the local buffer
      size_t first_space_pos = full_command.find(' ');

      if (first_space_pos == string::npos) {
        cerr << "Error: Malformed subscribe/unsubscribe command from "
             << client->id << ": '" << full_command << "'. No space found."
             << endl;
        break;
      }

      string command = full_command.substr(0, first_space_pos);
      string topic_pattern = full_command.substr(first_space_pos + 1);

      if (topic_pattern.empty()) {
        cerr << "Error: Subscribe/unsubscribe command from " << client->id
             << " has empty topic pattern." << endl;
        break;  // Skip invalid command
      }
      if (topic_pattern.find(' ') != string::npos) {
        cerr << "Error: Topic pattern contains spaces from client "
             << client->id << ": '" << topic_pattern << "'" << endl;
        break;  // Skip invalid command
      }

      if (packet.type == MSG_SUBSCRIBE) {
        subscribe_client_to_topic(client, topic_pattern);
      } else if (packet.type == MSG_UNSUBSCRIBE) {
        unsubscribe_client_from_topic(client, topic_pattern);
      } else {
        cerr << "Error: Unknown message type for subscribe/unsubscribe from "
             << client->id << ": " << packet.type << endl;
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

    case MSG_ID:
      cerr << "WARN: Received unexpected MSG_ID from already connected client "
           << client->id << ". Ignoring." << endl;
      break;

    case MSG_ERROR:
      cerr << "WARN: Received MSG_ERROR from client " << client->id
           << ". Ignoring." << endl;
      break;
    case MSG_UDP_FORWARD:
      cerr << "WARN: Received MSG_UDP_FORWARD from client " << client->id
           << ". Ignoring." << endl;
      break;
    default:
      cerr << "WARN: Received unknown/unexpected message type (" << packet.type
           << ") from client " << client->id << ". Ignoring." << endl;
      break;
  }
}

void ClientConnections::subscribe_client_to_topic(
    shared_ptr<ClientState> client, const string& topic_pattern) {
  if (!client || client->id.empty()) {
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
}

void ClientConnections::unsubscribe_client_from_topic(
    shared_ptr<ClientState> client, const string& topic_pattern) {
  if (!client || client->id.empty()) {  // Need valid client and ID
    cerr << "Error: Attempted to unsubscribe invalid client." << endl;
    return;
  }

  client->subscriptions.erase(topic_pattern);

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
}

int ClientConnections::pollAll() {
  int poll_timeout_ms = 100;
  int rc = poll(poll_fds, num_fds, poll_timeout_ms);

  if (rc < 0) {
    if (errno == EINTR) {
      cerr << "poll interrupted, retrying..." << endl;
      return 0;
    }
    perror("poll");
    return 1;  // Signal exit on critical poll error
  }

  if (rc == 0) {
    // timeout, no events
    return 0;
  }

  for (int i = num_fds - 1; i >= 0; --i) {
    if (i >= num_fds) {
      continue;
    }

    struct pollfd current_pollfd = poll_fds[i];
    int current_fd = current_pollfd.fd;

    // Check if the fd is still valid (might have been closed and removed by a
    // previous iteration)
    auto type_it = fd_to_type.find(current_fd);
    if (type_it == fd_to_type.end()) {
      continue;
    }
    SocketType type = type_it->second;

    if (current_pollfd.revents == 0) {
      continue;  // No events for this fd
    }

    if (current_pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
      cerr << "Error/Hangup/Invalid event on fd " << current_fd
           << " (type: " << type << ", revents: " << current_pollfd.revents
           << ")" << endl;
      if (type == TCP_LISTEN_SOCKET || type == UDP_LISTEN_SOCKET ||
          type == STDIN_SOCKET) {
        fprintf(stderr,
                "Critical error on listening socket or stdin. Exiting.\n");
        onExit();  // Notify clients
        return 1;  // Signal exit
      } else if (type == TCP_CLIENT_SOCKET) {
        // Error on client socket, treat as disconnect
        disconnect_tcp_client(current_fd);  // This calls remove_fd
      } else {
        // Should not happen
        close(current_fd);
        remove_fd(current_fd);
      }
      continue;
    }

    if (current_pollfd.revents & POLLIN) {
      if (type == TCP_LISTEN_SOCKET) {
        connect_tcp_client();
      } else if (type == UDP_LISTEN_SOCKET) {
        while (true) {
          handle_udp_message(current_fd);
          // Check errno after handle_udp_message returns if it hit EAGAIN
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            errno = 0;  // Reset errno
            break;      // No more datagrams for now
          }
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

    // Must check AFTER potential disconnect in POLLIN handler
    // Check if fd still exists in map before proceeding
    if (fd_to_type.count(current_fd) &&
        fd_to_type[current_fd] == TCP_CLIENT_SOCKET &&
        (current_pollfd.revents & POLLOUT)) {
      auto it = client_by_fd.find(current_fd);
      if (it != client_by_fd.end()) {
        handle_client_write(it->second);
      } else {
        cerr << "WARN: POLLOUT event for fd " << current_fd
             << " but no client state found in client_by_fd. Removing POLLOUT."
             << endl;
        // Find index and remove POLLOUT event bit - careful with iterating
        // backwards
        int poll_idx_now = find_poll_index(current_fd);
        if (poll_idx_now != -1 && poll_idx_now < num_fds) {
          poll_fds[poll_idx_now].events &= ~POLLOUT;
        }
      }
    }
  }

  return 0;
}
