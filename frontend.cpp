#include "frontend.h"
#include "common.h" // Needed for packet type definitions
#include <iostream>
#include <functional>
#include <chrono>
#include <iomanip>
#include <sstream>

// Helper function to get a human-readable string for a packet type
static std::string packet_type_to_string(uint8_t type) {
    switch (type) {
        case PACKET_TYPE_CMD: return "CMD";
        case PACKET_TYPE_ACK: return "ACK";
        case PACKET_TYPE_FILE: return "FILE";
        case PACKET_TYPE_DATA: return "DATA";
        case PACKET_TYPE_NACK: return "NACK";
        case CMD_DOWNLOAD: return "DOWNLOAD_CMD";
        case CMD_REPLICATE: return "REPLICATE_CMD";
        default: return "UNKNOWN";
    }
}

// Constructor
Frontend::Frontend(int client_port, int leader_update_port, const std::string& initial_backend_ip, int initial_backend_port)
    : client_port_(client_port),
      leader_update_port_(leader_update_port),
      current_backend_ip_(initial_backend_ip),
      current_backend_port_(initial_backend_port),
      active_connections_(0) {}

// Thread-safe logging method implementation
void Frontend::log(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex_);
    
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "%Y-%m-%d %X");

    std::cout << "[" << ss.str() << "] " << message << std::endl;
}

// Main execution loop for the frontend
void Frontend::run() {
    std::thread leader_thread(&Frontend::leader_update_listener, this);
    leader_thread.detach();

    NetworkManager client_server;
    if (!client_server.initServer(client_port_)) {
        log("[FE-CLIENT] FATAL: Failed to initialize client listener on port " + std::to_string(client_port_));
        return;
    }

    log("[FE-CLIENT] Listening for client connections on port " + std::to_string(client_port_));
    log("[FE-CLIENT] Initial leader is " + current_backend_ip_ + ":" + std::to_string(current_backend_port_));

    while (true) {
        if (client_server.acceptConnection()) {
            int client_socket = client_server.getSocket();
            std::thread(&Frontend::handle_client, this, client_socket).detach();
        } else {
            log("[FE-CLIENT] Error accepting new connection.");
        }
    }
    client_server.closeServer();
}

// Handles a single client's entire session
void Frontend::handle_client(int client_socket) {
    {
        std::lock_guard<std::mutex> lock(connection_count_mutex_);
        active_connections_++;
        log("[FE-CLIENT] New client. Active connections: " + std::to_string(active_connections_));
    }

    std::string backend_ip;
    int backend_port;
    {
        std::lock_guard<std::mutex> lock(backend_address_mutex_);
        backend_ip = current_backend_ip_;
        backend_port = current_backend_port_;
    }

    log("[FE-CLIENT] Routing connection to leader at " + backend_ip + ":" + std::to_string(backend_port));

    NetworkManager backend_connection;
    if (!backend_connection.connectToServer(backend_ip, backend_port)) {
        log("[FE-CLIENT] Could not connect to backend leader. Closing client connection.");
        close(client_socket);
        {
            std::lock_guard<std::mutex> lock(connection_count_mutex_);
            active_connections_--;
            log("[FE-CLIENT] Client disconnected (backend failure). Active connections: " + std::to_string(active_connections_));
        }
        return;
    }
    int backend_socket = backend_connection.getSocket();

    std::thread client_to_backend(&Frontend::forward_packets, this, client_socket, backend_socket, "CLIENT -> SERVER");
    std::thread backend_to_client(&Frontend::forward_packets, this, backend_socket, client_socket, "SERVER -> CLIENT");

    client_to_backend.join();
    backend_to_client.join();

    log("[FE-CLIENT] Session ended. Closing sockets.");
    close(client_socket);

    {
        std::lock_guard<std::mutex> lock(connection_count_mutex_);
        active_connections_--;
        log("[FE-CLIENT] Client session ended. Active connections: " + std::to_string(active_connections_));
    }
}

// Listens for and processes leader update messages
void Frontend::leader_update_listener() {
    NetworkManager leader_listener;
    if (!leader_listener.initServer(leader_update_port_)) {
        log("[FE-LEADER] FATAL: Could not start leader update listener on port " + std::to_string(leader_update_port_));
        return;
    }
    log("[FE-LEADER] Listening for leader updates on port " + std::to_string(leader_update_port_));

    while (true) {
        if (leader_listener.acceptConnection()) {
            int new_leader_socket = leader_listener.getSocket();
            packet update_pkt;
            if (leader_listener.receivePacket(update_pkt) && update_pkt.type == PACKET_TYPE_CMD) {
                std::string new_address(update_pkt.payload, update_pkt.length);
                size_t colon_pos = new_address.find(':');
                if (colon_pos != std::string::npos) {
                    std::string new_ip = new_address.substr(0, colon_pos);
                    int new_port = std::stoi(new_address.substr(colon_pos + 1));
                    {
                        std::lock_guard<std::mutex> lock(backend_address_mutex_);
                        current_backend_ip_ = new_ip;
                        current_backend_port_ = new_port;
                    }
                    log("[FE-LEADER] LEADER UPDATED! New primary server is " + new_ip + ":" + std::to_string(new_port));
                }
            }
            close(new_leader_socket);
        }
    }
}

// Forwards data between sockets and logs the traffic
void Frontend::forward_packets(int source_socket, int dest_socket, const std::string& direction) {
    char buffer[sizeof(packet)];
    while (true) {
        ssize_t bytes_read = recv(source_socket, buffer, sizeof(packet), 0);
        if (bytes_read <= 0) {
            log("[FORWARDER] " + direction + " | Connection closed or error. Shutting down.");
            shutdown(source_socket, SHUT_RDWR);
            shutdown(dest_socket, SHUT_RDWR);
            break;
        }

        // Log packet details
        if (bytes_read >= (ssize_t)(sizeof(packet) - MAX_PAYLOAD_SIZE)) {
            packet* pkt = reinterpret_cast<packet*>(buffer);
            std::string log_msg = "[FORWARDER] " + direction + 
                                  " | Type: " + packet_type_to_string(pkt->type) +
                                  " | Length: " + std::to_string(pkt->length) + " bytes";
            log(log_msg);
        }

        ssize_t bytes_sent = send(dest_socket, buffer, bytes_read, 0);
        if (bytes_sent <= 0) {
            log("[FORWARDER] " + direction + " | Failed to forward packet. Shutting down.");
            shutdown(source_socket, SHUT_RDWR);
            shutdown(dest_socket, SHUT_RDWR);
            break;
        }
    }
}

// Main entry point for the frontend application
int main(int argc, char* argv[]) {
    if (argc != 5) {
        // Direct output here is fine, as it's pre-threading.
        std::cerr << "Usage: " << argv[0] << " <client_port> <leader_update_port> <initial_backend_ip> <initial_backend_port>" << std::endl;
        return 1;
    }
    try {
        int client_port = std::stoi(argv[1]);
        int leader_update_port = std::stoi(argv[2]);
        std::string initial_backend_ip = argv[3];
        int initial_backend_port = std::stoi(argv[4]);

        Frontend frontend(client_port, leader_update_port, initial_backend_ip, initial_backend_port);
        frontend.run();
    } catch (const std::exception& e) {
        std::cerr << "An unexpected error occurred: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
