#include "network.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

NetworkManager::NetworkManager() : serverSocket(-1), clientSocket(-1), isServer(false) {}

NetworkManager::~NetworkManager() {
    if (serverSocket != -1) close(serverSocket);
    if (clientSocket != -1) close(clientSocket);
}

bool NetworkManager::initServer(int port) {
    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "Error creating socket" << std::endl;
        return false;
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(port);

    if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Error binding socket" << std::endl;
        return false;
    }

    if (listen(serverSocket, 5) < 0) {
        std::cerr << "Error listening on socket" << std::endl;
        return false;
    }

    isServer = true;
    return true;
}

bool NetworkManager::acceptConnection() {
    if (!isServer) return false;

    socklen_t clientLen = sizeof(clientAddr);
    clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddr, &clientLen);
    
    if (clientSocket < 0) {
        std::cerr << "Error accepting connection" << std::endl;
        return false;
    }
    
    return true;
}

void NetworkManager::closeServer() {
    if (serverSocket != -1) {
        close(serverSocket);
        serverSocket = -1;
    }
}

bool NetworkManager::connectToServer(const std::string& ip, int port) {
    clientSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (clientSocket < 0) {
        std::cerr << "[NETWORK] Socket creation failed (errno: " 
                 << errno << ")\n";
        return false;
    }

    // Set timeout (5 seconds)
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr) <= 0) {
        std::cerr << "[NETWORK] Invalid address " << ip 
                 << " (errno: " << errno << ")\n";
        close(clientSocket);
        return false;
    }

    if (connect(clientSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "[NETWORK] Connection failed to " << ip << ":" << port
                 << " (errno: " << errno << ": " << strerror(errno) << ")\n";
        close(clientSocket);
        return false;
    }

    isServer = false;
    return true;
}

void NetworkManager::disableTimeouts() {
    if (clientSocket != -1) {
        // Remove timeout by setting it to 0 (no timeout)
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
}

void NetworkManager::closeConnection() {
    if (clientSocket != -1) {
        close(clientSocket);
        clientSocket = -1;
    }
}

bool NetworkManager::sendPacket(const packet& pkt) {
    if (pkt.length > MAX_PAYLOAD_SIZE) {
        std::cerr << "Error: Payload size exceeds maximum allowed size" << std::endl;
        return false;
    }

    int socket = clientSocket;  // Always use clientSocket since it's the active connection
    ssize_t totalSent = 0;
    const char* buffer = reinterpret_cast<const char*>(&pkt);
    size_t totalSize = sizeof(packet) - MAX_PAYLOAD_SIZE + pkt.length;

    while (totalSent < totalSize) {
        ssize_t sent = send(socket, buffer + totalSent, totalSize - totalSent, 0);
        if (sent <= 0) {
            std::cerr << "Error sending packet" << std::endl;
            return false;
        }
        totalSent += sent;
    }
    return true;
}

bool NetworkManager::receivePacket(packet& pkt) {
    int socket = isServer ? clientSocket : clientSocket;
    
    // First receive the fixed header part
    ssize_t totalReceived = 0;
    char* buffer = reinterpret_cast<char*>(&pkt);
    size_t headerSize = sizeof(packet) - MAX_PAYLOAD_SIZE;

    while (totalReceived < headerSize) {
        ssize_t received = recv(socket, buffer + totalReceived, headerSize - totalReceived, 0);
        if (received <= 0) {
            std::cerr << "Error receiving packet header" << std::endl;
            return false;
        }
        totalReceived += received;
    }

    // Verify payload length
    if (pkt.length > MAX_PAYLOAD_SIZE) {
        std::cerr << "Error: Received packet with invalid payload size" << std::endl;
        return false;
    }

    // Receive payload if any
    if (pkt.length > 0) {
        totalReceived = 0;
        while (totalReceived < pkt.length) {
            ssize_t received = recv(socket, pkt.payload + totalReceived, pkt.length - totalReceived, 0);
            if (received <= 0) {
                std::cerr << "Error receiving packet payload" << std::endl;
                return false;
            }
            totalReceived += received;
        }
    }

    return true;
}

bool NetworkManager::sendFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file for reading" << std::endl;
        return false;
    }

    // Get file size
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    if (fileSize == 0) {
        std::cerr << "Error: File is empty" << std::endl;
        return false;
    }

    // Read file content
    std::vector<char> buffer(fileSize);
    file.read(buffer.data(), fileSize);
    if (file.gcount() != fileSize) {
        std::cerr << "Error: Could not read entire file" << std::endl;
        return false;
    }
    file.close();

    // Send file size
    packet pkt;
    pkt.type = PACKET_TYPE_FILE;
    pkt.seqn = 0;
    pkt.total_size = 1;
    pkt.length = sizeof(fileSize);
    if (pkt.length > MAX_PAYLOAD_SIZE) {
        std::cerr << "Error: File size too large to send" << std::endl;
        return false;
    }
    memcpy(pkt.payload, &fileSize, sizeof(fileSize));

    if (!sendPacket(pkt)) {
        std::cerr << "Failed to send file size" << std::endl;
        return false;
    }

    // Wait for ACK
    packet ack;
    if (!receivePacket(ack) || ack.type != PACKET_TYPE_ACK) {
        std::cerr << "Failed to receive ACK for file size" << std::endl;
        return false;
    }

    // Send file content
    size_t bytesSent = 0;
    while (bytesSent < fileSize) {
        size_t remaining = fileSize - bytesSent;
        size_t chunkSize = std::min(remaining, static_cast<size_t>(MAX_PAYLOAD_SIZE));
        
        pkt.type = PACKET_TYPE_DATA;
        pkt.seqn = bytesSent / MAX_PAYLOAD_SIZE;
        pkt.total_size = (fileSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
        pkt.length = chunkSize;
        memcpy(pkt.payload, buffer.data() + bytesSent, chunkSize);
        
        if (!sendPacket(pkt)) {
            std::cerr << "Failed to send file chunk" << std::endl;
            return false;
        }

        // Wait for ACK
        if (!receivePacket(ack) || ack.type != PACKET_TYPE_ACK) {
            std::cerr << "Failed to receive ACK for chunk" << std::endl;
            return false;
        }
        
        bytesSent += chunkSize;
    }

    return true;
}

bool NetworkManager::receiveFile(const std::string& filepath) {
    // Receive file size
    packet pkt;
    if (!receivePacket(pkt) || pkt.type != PACKET_TYPE_FILE) {
        std::cerr << "Failed to receive file size" << std::endl;
        return false;
    }

    size_t fileSize;
    memcpy(&fileSize, pkt.payload, sizeof(fileSize));

    // Send ACK for file size
    packet ack;
    ack.type = PACKET_TYPE_ACK;
    ack.seqn = 0;
    ack.total_size = 1;
    ack.length = 0;
    if (!sendPacket(ack)) {
        std::cerr << "Failed to send ACK for file size" << std::endl;
        return false;
    }

    // Create output file
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not create file for writing" << std::endl;
        return false;
    }

    // Receive file content
    size_t bytesReceived = 0;
    while (bytesReceived < fileSize) {
        if (!receivePacket(pkt) || pkt.type != PACKET_TYPE_DATA) {
            std::cerr << "Failed to receive file chunk" << std::endl;
            return false;
        }

        file.write(pkt.payload, pkt.length);
        bytesReceived += pkt.length;

        // Send ACK
        ack.seqn = pkt.seqn;
        if (!sendPacket(ack)) {
            std::cerr << "Failed to send ACK for chunk" << std::endl;
            return false;
        }
    }

    file.close();
    return true;
}

int NetworkManager::getSocket() const {
    return clientSocket;
}

void NetworkManager::setClientSocket(int socket) {
    clientSocket = socket;
} 