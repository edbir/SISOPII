#ifndef FRONTEND_H
#define FRONTEND_H

#include <string>
#include <mutex>
#include <thread>
#include "network.h"

class Frontend {
public:
    /**
     * @param client_port The port to listen for client connections.
     * @param leader_update_port The port to listen for leader update messages.
     * @param initial_backend_ip The IP address of the initial leader server.
     * @param initial_backend_port The port of the initial leader server.
     */
    Frontend(int client_port, int leader_update_port, const std::string& initial_backend_ip, int initial_backend_port);

    void run(); // starts the frontend server

private:
    
    void handle_client(int client_socket); // Handles a single client connection.

    /**
     * @brief Listens for messages announcing a new leader.
     */
    void leader_update_listener();

    /**
     * @brief Forwards packets between two sockets and logs the traffic.
     */
    void forward_packets(int source_socket, int dest_socket, const std::string& direction);

    void log(const std::string& message); // logs a message to the console in a thread-safe manner

    int client_port_;
    int leader_update_port_;

    // Shared state for the current leader server
    std::string current_backend_ip_;
    int current_backend_port_;
    std::mutex backend_address_mutex_;

    // Shared resources for connection tracking and logging
    int active_connections_;
    std::mutex connection_count_mutex_;
    std::mutex log_mutex_; // Mutex dedicated to synchronizing console output
};

#endif // FRONTEND_H
