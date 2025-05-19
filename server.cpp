#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <cstring>
#include "network.h"
#include "file_manager.h"
#include "sync_manager.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class Server {
public:
    Server(int port) : port(port), fileManager("server_data") {
        if (!network.initServer(port)) {
            throw std::runtime_error("Failed to initialize server");
        }
    }

    void run() {
        std::cout << "[SERVER] Server running on port " << port << std::endl;

        while (true) {
            if (!network.acceptConnection()) {
                std::cerr << "[SERVER] Error accepting connection" << std::endl;
                continue;
            }

            // Create a new NetworkManager for this client
            NetworkManager* clientNetwork = new NetworkManager();
            clientNetwork->setClientSocket(network.getClientSocket());
            
            // Handle client connection in a new thread
            std::thread clientThread(&Server::handleClient, this, clientNetwork);
            clientThread.detach();
        }
    }

private:
    int port;
    NetworkManager network;  // Main server network manager
    FileManager fileManager;
    std::map<std::string, std::pair<SyncManager*, NetworkManager*>> userSyncManagers;
    std::mutex syncManagerMutex;

    void handleClient(NetworkManager* clientNetwork) {
        std::string username;
        try {
            // Receive username
            packet pkt;
            if (!clientNetwork->receivePacket(pkt) || pkt.type != PACKET_TYPE_CMD) {
                throw std::runtime_error("Failed to receive username");
            }
            username = std::string(pkt.payload, pkt.length);
            std::cout << "[SERVER] New client connected: " << username << std::endl;

            // Send ACK for username
            pkt.type = PACKET_TYPE_ACK;
            pkt.length = 0;
            if (!clientNetwork->sendPacket(pkt)) {
                throw std::runtime_error("Failed to send username ACK");
            }

            // Create user directory
            if (!fileManager.createUserDir(username)) {
                throw std::runtime_error("Failed to create user directory");
            }

            // Setup sync manager for user
            {
                std::lock_guard<std::mutex> lock(syncManagerMutex);
                if (userSyncManagers.find(username) == userSyncManagers.end()) {
                    userSyncManagers[username] = std::make_pair(
                        new SyncManager(*clientNetwork, fileManager),
                        clientNetwork
                    );
                } else {
                    // Update existing user's network manager
                    delete userSyncManagers[username].second;
                    userSyncManagers[username].second = clientNetwork;
                }
            }

            // Handle client commands
            handleClientCommands(username, clientNetwork);

        } catch (const std::exception& e) {
            std::cerr << "[SERVER] Error handling client " << username << ": " << e.what() << std::endl;
        }

        // Cleanup
        {
            std::lock_guard<std::mutex> lock(syncManagerMutex);
            if (userSyncManagers.find(username) != userSyncManagers.end()) {
                delete userSyncManagers[username].first;
                delete userSyncManagers[username].second;
                userSyncManagers.erase(username);
            }
        }
        std::cout << "[SERVER] Client " << username << " disconnected" << std::endl;
    }

    void handleClientCommands(const std::string& username, NetworkManager* clientNetwork) {
        bool inUpload = false;
        std::string currentFilename;
        size_t expectedChunks = 0;
        size_t receivedChunks = 0;
        packet pkt;

        while (true) {
            if (!clientNetwork->receivePacket(pkt)) {
                throw std::runtime_error("Failed to receive packet");
            }

            switch (pkt.type) {
                case PACKET_TYPE_CMD: {
                    if (inUpload) {
                        std::cerr << "[SERVER] Received command while upload in progress" << std::endl;
                        continue;
                    }

                    uint16_t cmd;
                    memcpy(&cmd, pkt.payload, sizeof(cmd));

                    if (cmd == CMD_UPLOAD) {
                        inUpload = true;
                        receivedChunks = 0;
                        expectedChunks = 0;
                        currentFilename.clear();
                        
                        // Send ACK for upload command
                        pkt.type = PACKET_TYPE_ACK;
                        pkt.length = 0;
                        if (!clientNetwork->sendPacket(pkt)) {
                            throw std::runtime_error("Failed to send upload ACK");
                        }
                    } 
                    else if (cmd == CMD_LIST_SERVER) {
                        std::string userDir = fileManager.getUserDir(username);
                        std::string fileList;
                    
                        for (const auto& entry : fs::directory_iterator(userDir)) {
                            if (entry.is_regular_file()) {
                                fileList += entry.path().filename().string() + "\n";
                            }
                        }
                    
                        // Enviar como pacote do tipo DATA
                        packet resp;
                        resp.type = PACKET_TYPE_DATA;
                        resp.seqn = 0;
                        resp.total_size = 1;
                        resp.length = fileList.length();
                        memcpy(resp.payload, fileList.c_str(), fileList.length());
                    
                        if (!clientNetwork->sendPacket(resp)) {
                            throw std::runtime_error("Failed to send file list");
                        }
                    } else if (cmd == CMD_DELETE) {
                        std::cout << "[SERVER][DELETE] Received DELETE command" << std::endl;
    
                        // 1. Expect filename IMMEDIATELY (no intermediate ACK)
                        std::cout << "[SERVER][NET] Expecting filename packet next...\n";
                        if (!clientNetwork->receivePacket(pkt) || pkt.type != PACKET_TYPE_FILE) {
                            std::cerr << "[SERVER][ERROR] Expected filename packet (got type=" 
                                    << pkt.type << ")\n";
                            //sendNack();
                            break;
                        }

                        std::string filename(pkt.payload, pkt.length);
                        std::string filepath = fileManager.getUserDir(username) + "/" + filename;
                        std::cout << "[SERVER][DEBUG] Full deletion path: " << fs::absolute(filepath) << std::endl;

                        // 2. Attempt deletion
                        bool success = false;
                        try {
                            std::cout << "[SERVER][FS] Pre-deletion check: " << fs::exists(filepath) << std::endl;
                            success = fs::remove(filepath);
                            std::cout << "[SERVER][FS] Post-deletion check: " << fs::exists(filepath) << std::endl;
                            std::cout << "[SERVER][DELETE] Deletion " << (success ? "succeeded" : "failed") 
                                    << " for: " << filepath << std::endl;
                        } catch (const fs::filesystem_error& e) {
                            std::cerr << "[SERVER][ERROR] Filesystem exception: " << e.what() 
                                    << " (code: " << e.code() << ")" << std::endl;
                        }

                        // 3. Send response
                        pkt.type = success ? PACKET_TYPE_ACK : PACKET_TYPE_NACK;
                        pkt.length = 0;
                        std::cout << "[SERVER][NET] Sending " << (success ? "ACK" : "NACK") << std::endl;
                        if (!clientNetwork->sendPacket(pkt)) {
                            std::cerr << "[SERVER][ERROR] Failed to send response" << std::endl;
                        }
                    } else if (cmd == CMD_EXIT) {
                        return;
                    }
                    break;
                }
                case PACKET_TYPE_FILE: {
                    if (!inUpload) {
                        throw std::runtime_error("Received file packet without upload command");
                    }

                    if (currentFilename.empty()) {
                        // This is the filename packet
                        currentFilename = std::string(pkt.payload, pkt.length);
                        std::cout << "[SERVER] Received filename: " << currentFilename << std::endl;
                        
                        // Send ACK for filename
                        pkt.type = PACKET_TYPE_ACK;
                        pkt.length = 0;
                        if (!clientNetwork->sendPacket(pkt)) {
                            throw std::runtime_error("Failed to send filename ACK");
                        }
                    } else if (expectedChunks == 0) {
                        // This is the file size packet
                        size_t fileSize;
                        memcpy(&fileSize, pkt.payload, sizeof(fileSize));
                        expectedChunks = (fileSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
                        
                        // Create/truncate file
                        std::string filepath = fileManager.getUserDir(username) + "/" + currentFilename;
                        std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
                        if (!file.is_open()) {
                            throw std::runtime_error("Failed to create file: " + std::string(strerror(errno)));
                        }
                        file.close();

                        // Send ACK for file size
                        pkt.type = PACKET_TYPE_ACK;
                        pkt.length = 0;
                        if (!clientNetwork->sendPacket(pkt)) {
                            throw std::runtime_error("Failed to send file size ACK");
                        }
                    }
                    break;
                }
                case PACKET_TYPE_DATA: {
                    if (!inUpload || currentFilename.empty() || expectedChunks == 0) {
                        throw std::runtime_error("Received data without upload in progress");
                    }

                    // Write chunk to file
                    std::string filepath = fileManager.getUserDir(username) + "/" + currentFilename;
                    std::ofstream file(filepath, std::ios::binary | std::ios::app);
                    if (!file.is_open()) {
                        throw std::runtime_error("Failed to open file for writing: " + std::string(strerror(errno)));
                    }

                    file.write(pkt.payload, pkt.length);
                    if (file.fail()) {
                        throw std::runtime_error("Failed to write chunk: " + std::string(strerror(errno)));
                    }
                    file.close();

                    receivedChunks++;

                    // Send ACK for chunk
                    pkt.type = PACKET_TYPE_ACK;
                    pkt.length = 0;
                    if (!clientNetwork->sendPacket(pkt)) {
                        throw std::runtime_error("Failed to send chunk ACK");
                    }

                    // Check if upload is complete
                    if (receivedChunks >= expectedChunks) {
                        std::cout << "[SERVER] Upload completed for " << username << ": " << currentFilename << std::endl;
                        inUpload = false;
                        currentFilename.clear();
                        expectedChunks = 0;
                        receivedChunks = 0;
                    }
                    break;
                }
                default:
                    throw std::runtime_error("Unknown packet type: " + std::to_string(pkt.type));
            }
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }

    try {
        int port = std::stoi(argv[1]);
        Server server(port);
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
} 