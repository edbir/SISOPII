#ifndef NETWORK_H
#define NETWORK_H

#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "common.h"

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    // Server functions
    bool initServer(int port);
    bool acceptConnection();
    void closeServer();

    // Client functions
    bool connectToServer(const std::string& ip, int port);
    void closeConnection();

    // Common functions
    bool sendPacket(const packet& pkt);
    bool receivePacket(packet& pkt);
    bool sendFile(const std::string& filepath);
    bool receiveFile(const std::string& filepath);

    int getSocket() const;
    void setClientSocket(int socket);

private:
    int serverSocket;
    int clientSocket;
    struct sockaddr_in serverAddr;
    struct sockaddr_in clientAddr;
    bool isServer;
};

#endif // NETWORK_H 