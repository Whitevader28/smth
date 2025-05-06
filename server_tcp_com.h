#ifndef _SERVER_TCP_COM_H
#define _SERVER_TCP_COM_H

#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdint.h> // Required for uint16_t

#include <iostream>
#include <memory> // For shared_ptr
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque> // For output buffer queue

#include "common.h" // Includes ChatPacket, MessageType etc.

using namespace std;

// --- Function Declarations for Socket Creation (moved from original server_tcp_com.cpp) ---
/**
 * @brief Creates a TCP listening socket bound to the specified IP and port.
 * Enables SO_REUSEADDR and disables Nagle's algorithm.
 *
 * @param port The port number to bind to (host byte order).
 * @param ip The IP address string to bind to (e.g., "0.0.0.0").
 * @return The listening socket file descriptor, or terminates on error.
 */
int create_tcp_listenfd(uint16_t port, const char *ip);

/**
 * @brief Creates a UDP listening socket bound to the specified IP and port.
 * Enables SO_REUSEADDR.
 *
 * @param port The port number to bind to (host byte order).
 * @param ip The IP address string to bind to (e.g., "0.0.0.0").
 * @return The listening socket file descriptor, or terminates on error.
 */
int create_udp_listenfd(uint16_t port, const char *ip);
// --- End Function Declarations ---


// Forward declaration
class ClientTCP; // This seems unused in the refactored version, consider removing if true.

// Structure to hold client state including buffers
struct ClientState {
    int socketfd = -1;
    string id;
    bool connected = false;
    vector<char> recv_buffer; // Buffer for partially received messages
    deque<vector<char>> send_queue; // Queue of messages waiting to be sent
    size_t current_send_offset = 0; // How much of the front message has been sent
    unordered_set<string> subscriptions; // Topics this client is subscribed to (exact or pattern)

    ClientState(int fd = -1, string client_id = "") : socketfd(fd), id(client_id) {}
};

// Enum to differentiate socket types in poll array
enum SocketType { STDIN_SOCKET, TCP_LISTEN_SOCKET, UDP_LISTEN_SOCKET, TCP_CLIENT_SOCKET };

class ClientConnections {
   private:
    int listenfd_tcp;
    int listenfd_udp;
    int stdinfd;
    struct pollfd poll_fds[MAX_TCP_CONNECTIONS + 3]; // +3 for TCP listen, UDP listen, stdin
    int num_fds;

    // Maps for managing clients
    unordered_map<int, shared_ptr<ClientState>> client_by_fd; // fd -> client state
    unordered_map<string, shared_ptr<ClientState>> client_by_id; // id -> client state (for reconnects)
    unordered_map<int, SocketType> fd_to_type; // fd -> socket type

    // Subscription management: topic_pattern -> set of client IDs subscribed
    unordered_map<string, unordered_set<string>> topic_subscribers;


    // Internal helper methods
    void add_fd(int fd, short int events, SocketType type);
    void remove_fd(int fd); // Removes fd from poll_fds and associated maps
    int find_poll_index(int fd); // Helper to find index of fd in poll_fds

    void connect_tcp_client(); // Handles accepting new TCP connections
    int handle_stdin(); // Handles input from stdin ("exit")
    void handle_udp_message(int udp_fd); // Handles incoming UDP datagrams
    void handle_tcp_client_event(int client_fd_index); // Handles events (read/write) for a specific TCP client

    // Non-blocking I/O handlers
    void handle_client_read(shared_ptr<ClientState> client); // Reads from client socket
    void handle_client_write(shared_ptr<ClientState> client); // Writes to client socket (if data queued)
    void process_received_data(shared_ptr<ClientState> client); // Processes complete messages in recv_buffer
    void process_tcp_command(shared_ptr<ClientState> client, const ChatPacket& packet); // Processes commands like subscribe/unsubscribe

    // Message sending and subscription logic
    void send_message_to_client(shared_ptr<ClientState> client, const ChatPacket& packet); // Queues a message for sending
    void broadcast_udp_message(const string& udp_topic, const ChatPacket& tcp_packet); // Sends UDP data to relevant TCP clients
    void subscribe_client_to_topic(shared_ptr<ClientState> client, const string& topic_pattern);
    void unsubscribe_client_from_topic(shared_ptr<ClientState> client, const string& topic_pattern);
    void remove_client_subscriptions(shared_ptr<ClientState> client); // Clean up subscriptions on disconnect

    void disconnect_tcp_client(int fd); // Disconnects a client, updates state, cleans maps
    void onExit(); // Sends exit message to all connected clients

    // Wildcard matching helper
    bool match_topic(const string &topic, const string &pattern);

   public:
    ClientConnections(int tcp_sock, int udp_sock)
        : listenfd_tcp(tcp_sock), listenfd_udp(udp_sock), stdinfd(STDIN_FILENO), num_fds(0) {
        memset(poll_fds, 0, sizeof(poll_fds));
        // Add initial FDs to poll set
        add_fd(listenfd_tcp, POLLIN, TCP_LISTEN_SOCKET);
        add_fd(listenfd_udp, POLLIN, UDP_LISTEN_SOCKET);
        add_fd(stdinfd, POLLIN, STDIN_SOCKET);
    }

    ~ClientConnections() {
        // Consider calling onExit() or similar cleanup if needed
    }

    int pollAll(); // Main event loop using poll
};

#endif // _SERVER_TCP_COM_H
