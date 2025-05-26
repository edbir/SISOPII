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
#include <atomic>

namespace fs = std::filesystem;

struct ClientConnection {
    NetworkManager* network;
    SyncManager* syncManager;
    std::thread* clientThread;
    std::atomic<bool> isActive{true};
    std::string connectionId;

    // Default constructor
    ClientConnection() : network(nullptr), syncManager(nullptr), clientThread(nullptr) {}

    // Move constructor
    ClientConnection(ClientConnection&& other) noexcept
        : network(other.network)
        , syncManager(other.syncManager)
        , clientThread(other.clientThread)
        , isActive(other.isActive.load())
        , connectionId(std::move(other.connectionId))
    {
        other.network = nullptr;
        other.syncManager = nullptr;
        other.clientThread = nullptr;
    }

    // Move assignment operator
    ClientConnection& operator=(ClientConnection&& other) noexcept {
        if (this != &other) {
            network = other.network;
            syncManager = other.syncManager;
            clientThread = other.clientThread;
            isActive = other.isActive.load();
            connectionId = std::move(other.connectionId);

            other.network = nullptr;
            other.syncManager = nullptr;
            other.clientThread = nullptr;
        }
        return *this;
    }

    // Delete copy operations
    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;
};

class Server {
public:
    Server(int port) : port(port), fileManager("server_data") {
        if (!network.initServer(port)) {
            throw std::runtime_error("Failed to initialize server");
        }
    }

    ~Server() {
        // Limpar todas as conexões ativas
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (auto& [username, connections] : userConnections) {
            for (auto& conn : connections) {
                if (conn.syncManager) delete conn.syncManager;
                if (conn.network) delete conn.network;
                if (conn.clientThread) delete conn.clientThread;
            }
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
            clientNetwork->setClientSocket(network.getSocket());
            
            // Handle client connection in a new thread
            std::thread clientThread(&Server::handleClient, this, clientNetwork);
            clientThread.detach();
        }
    }

private:
    int port;
    NetworkManager network;  // Main server network manager
    FileManager fileManager;
    std::map<std::string, std::vector<ClientConnection>> userConnections;
    std::mutex connectionsMutex;
    std::atomic<uint64_t> nextConnectionId{0};
    std::mutex uploadMutex;  // Mutex for upload synchronization

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
                std::lock_guard<std::mutex> lock(connectionsMutex);
                if (userConnections.find(username) == userConnections.end()) {
                    ClientConnection conn;
                    conn.network = clientNetwork;
                    conn.syncManager = new SyncManager(*clientNetwork, fileManager);
                    conn.clientThread = nullptr;
                    conn.isActive = true;
                    conn.connectionId = std::to_string(nextConnectionId++);
                    userConnections[username].push_back(std::move(conn));
                } else {
                    // Add new connection for existing user
                    ClientConnection conn;
                    conn.network = clientNetwork;
                    conn.syncManager = new SyncManager(*clientNetwork, fileManager);
                    conn.clientThread = nullptr;
                    conn.isActive = true;
                    conn.connectionId = std::to_string(nextConnectionId++);
                    userConnections[username].push_back(std::move(conn));
                }
            }

            // Handle client commands
            handleClientCommands(username, clientNetwork);

        } catch (const std::exception& e) {
            std::cerr << "[SERVER] Error handling client " << username << ": " << e.what() << std::endl;
        }

        // Cleanup
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            for (auto& conn : userConnections[username]) {
                if (conn.network == clientNetwork) {
                    conn.isActive = false;
                    break;
                }
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
            std::cerr << "[SERVER] Connection lost with client " << username << std::endl;
            break;
        }

        switch (pkt.type) {
            case PACKET_TYPE_CMD: {
                uint16_t cmd;
                memcpy(&cmd, pkt.payload, sizeof(cmd));

                if (cmd == CMD_UPLOAD) {
                    std::unique_lock<std::mutex> uploadLock(uploadMutex);
                    if (inUpload) {
                        std::cerr << "[SERVER] Received new upload command while upload in progress" << std::endl;
                        // Reset upload state
                        inUpload = false;
                        currentFilename.clear();
                        expectedChunks = 0;
                        receivedChunks = 0;
                    }
                    
                    inUpload = true;
                    receivedChunks = 0;
                    expectedChunks = 0;
                    currentFilename.clear();
                    
                    // Send ACK for upload command
                    pkt.type = PACKET_TYPE_ACK;
                    pkt.length = 0;
                    if (!clientNetwork->sendPacket(pkt)) {
                        std::cerr << "[SERVER] Failed to send upload ACK" << std::endl;
                        inUpload = false;
                        break;
                    }
                } else if (cmd == CMD_LIST_SERVER) {
                    std::string userDir = fileManager.getUserDir(username);
                        std::string fileList;
                    
                        for (const auto& entry : fs::directory_iterator(userDir)) {
                            if (entry.is_regular_file()) {
                                fileList += entry.path().filename().string() + "\n";
                            }
                        }
                    
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
                } else if (cmd == CMD_DOWNLOAD) {
                        std::cout << "[SERVER] Received CMD_DOWNLOAD from " << username << std::endl;
                        std::unique_lock<std::mutex> opLock(uploadMutex); // Serialize with upload operations
                        
                        // 1. Send ACK for CMD_DOWNLOAD
                        packet ackPktS;
                        ackPktS.type = PACKET_TYPE_ACK;
                        ackPktS.length = 0;
                        if (!clientNetwork->sendPacket(ackPktS)) {
                            std::cerr << "[SERVER] Failed to send ACK for CMD_DOWNLOAD to " << username << std::endl;
                            opLock.unlock(); // Release mutex before breaking
                            break; 
                        }
                        
                        // 2. Receive filename
                        packet fileReqPkt;
                        if (!clientNetwork->receivePacket(fileReqPkt) || fileReqPkt.type != PACKET_TYPE_FILE) {
                            std::cerr << "[SERVER] Did not receive filename for download from " << username << std::endl;
                            // Optionally send NACK here if client expects it for this specific failure
                            opLock.unlock();
                            break;
                        }
                        std::string requestedFilename(fileReqPkt.payload, fileReqPkt.length);
                        std::cout << "[SERVER] " << username << " requests download for: " << requestedFilename << std::endl;
                        
                        std::string filepath_on_server = fileManager.getUserDir(username) + "/" + requestedFilename;
                        
                        // 3. Check file existence and send ACK/NACK
                        if (fs::exists(filepath_on_server) && fs::is_regular_file(filepath_on_server)) {
                            std::cout << "[SERVER] File " << requestedFilename << " exists. Sending ACK and file." << std::endl;
                            ackPktS.type = PACKET_TYPE_ACK;
                            ackPktS.length = 0;
                            if (!clientNetwork->sendPacket(ackPktS)) {
                                std::cerr << "[SERVER] Failed to send ACK for file existence to " << username << std::endl;
                                opLock.unlock();
                                break;
                            }
                        
                            // 4. Send the file
                            if (!clientNetwork->sendFile(filepath_on_server)) {
                                std::cerr << "[SERVER] Failed to send file " << requestedFilename << " to " << username << std::endl;
                            } else {
                                std::cout << "[SERVER] File " << requestedFilename << " sent successfully to " << username << std::endl;
                            }
                        } else {
                            std::cerr << "[SERVER] File " << requestedFilename << " not found for user " << username << ". Sending NACK." << std::endl;
                            packet nackPkt;
                            nackPkt.type = PACKET_TYPE_NACK;
                            nackPkt.length = 0;
                            clientNetwork->sendPacket(nackPkt); // Ignoring failure to send NACK for now
                        }
                        opLock.unlock(); // Release mutex
                } else if (cmd == CMD_EXIT) {
                    return;
                }
                break;
            }
            case PACKET_TYPE_FILE: {
                std::unique_lock<std::mutex> uploadLock(uploadMutex);
                if (!inUpload) {
                    std::cerr << "[SERVER] Received file packet without upload command" << std::endl;
                    continue;
                }

                if (currentFilename.empty()) {
                    currentFilename = std::string(pkt.payload, pkt.length);
                    std::cout << "[SERVER][DEBUG] Received filename: " << currentFilename << std::endl;
                    
                    pkt.type = PACKET_TYPE_ACK;
                    pkt.length = 0;
                    if (!clientNetwork->sendPacket(pkt)) {
                        std::cerr << "[SERVER] Failed to send filename ACK" << std::endl;
                        inUpload = false;
                        currentFilename.clear();
                        break;
                    }
                } else if (expectedChunks == 0) {
                    size_t fileSize;
                    memcpy(&fileSize, pkt.payload, sizeof(fileSize));
                    expectedChunks = (fileSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
                    std::cout << "[SERVER][DEBUG] Received file size: " << fileSize 
                              << ", expected chunks: " << expectedChunks << std::endl;
                    
                    std::string filepath = fileManager.getUserDir(username) + "/" + currentFilename;
                    std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
                    if (!file.is_open()) {
                        std::cerr << "[SERVER] Failed to create file: " << filepath << std::endl;
                        inUpload = false;
                        currentFilename.clear();
                        expectedChunks = 0;
                        receivedChunks = 0;
                        break;
                    }
                    file.close();

                    pkt.type = PACKET_TYPE_ACK;
                    pkt.length = 0;
                    if (!clientNetwork->sendPacket(pkt)) {
                        std::cerr << "[SERVER] Failed to send file size ACK" << std::endl;
                        inUpload = false;
                        currentFilename.clear();
                        expectedChunks = 0;
                        receivedChunks = 0;
                        break;
                    }
                }
                break;
            }
            case PACKET_TYPE_DATA: {
                std::unique_lock<std::mutex> uploadLock(uploadMutex);
                if (!inUpload || currentFilename.empty() || expectedChunks == 0) {
                    std::cerr << "[SERVER] Received data without upload in progress" << std::endl;
                    continue;
                }

                std::string filepath = fileManager.getUserDir(username) + "/" + currentFilename;
                std::ofstream file(filepath, std::ios::binary | std::ios::app);
                if (!file.is_open()) {
                    std::cerr << "[SERVER] Failed to open file for writing: " << filepath << std::endl;
                    break;
                }

                file.write(pkt.payload, pkt.length);
                if (file.fail()) {
                    std::cerr << "[SERVER] Failed to write chunk" << std::endl;
                    file.close();
                    break;
                }
                file.close();

                receivedChunks++;

                pkt.type = PACKET_TYPE_ACK;
                pkt.length = 0;
                if (!clientNetwork->sendPacket(pkt)) {
                    std::cerr << "[SERVER] Failed to send chunk ACK" << std::endl;
                    break;
                }

                if (receivedChunks >= expectedChunks) {
                    std::cout << "[SERVER] Upload completed for " << username << ": " << currentFilename << std::endl;
                    
                    std::string filepath = fileManager.getUserDir(username) + "/" + currentFilename;
                    uploadLock.unlock();
                    broadcastFileChange(username, currentFilename, filepath, clientNetwork->getSocket());
                    uploadLock.lock();
                    
                    inUpload = false;
                    currentFilename.clear();
                    expectedChunks = 0;
                    receivedChunks = 0;
                }
                break;
            }
            default:
                std::cerr << "[SERVER] Unknown packet type: " << pkt.type << std::endl;
                break;
        }
    }
}


void broadcastFileChange(const std::string& sourceUsername, const std::string& filename, 
                        const std::string& filepath, int originatingSocket = -1) {
    std::lock_guard<std::mutex> lock(connectionsMutex);
    
    auto userIt = userConnections.find(sourceUsername);
    if (userIt == userConnections.end()) {
        return;
    }

    // Read the file content once
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[SERVER] Failed to open file for broadcasting: " << filepath << std::endl;
        return;
    }
    
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<char> buffer(fileSize);
    file.read(buffer.data(), fileSize);
    file.close();

    // Broadcast to all connections for this username
    for (auto& conn : userIt->second) {
        if (!conn.isActive) {
            std::cout << "[SERVER] Skipping inactive connection for " << sourceUsername << std::endl;
            continue;
        }
        
        // Skip the client that originated the change
        if (originatingSocket != -1 && conn.network->getSocket() == originatingSocket) {
            std::cout << "[SERVER] Skipped client that originated the change (socket: " 
                      << originatingSocket << ")" << std::endl;
            continue;
        }
        
        std::cout << "[SERVER] Broadcasting file change to " << sourceUsername 
                  << " (connection " << conn.connectionId << "): " << filename << std::endl;
        
        try {
            packet pkt;
            
            // 1. Send command
            pkt.type = PACKET_TYPE_CMD;
            uint16_t cmd = CMD_FILE_CHANGED;
            pkt.length = sizeof(cmd);
            memcpy(pkt.payload, &cmd, sizeof(cmd));
            if (!conn.network->sendPacket(pkt)) {
                std::cerr << "[SERVER] Failed to send file change command" << std::endl;
                continue;
            }
            
            // 2. Send filename
            pkt.type = PACKET_TYPE_FILE;
            pkt.length = filename.length();
            memcpy(pkt.payload, filename.c_str(), filename.length());
            if (!conn.network->sendPacket(pkt)) {
                std::cerr << "[SERVER] Failed to send filename" << std::endl;
                continue;
            }
            
            // 3. Send file size
            pkt.type = PACKET_TYPE_FILE;
            pkt.length = sizeof(fileSize);
            memcpy(pkt.payload, &fileSize, sizeof(fileSize));
            if (!conn.network->sendPacket(pkt)) {
                std::cerr << "[SERVER] Failed to send file size" << std::endl;
                continue;
            }
            
            // 4. Send file content
            size_t bytesSent = 0;
            while (bytesSent < fileSize) {
                size_t chunkSize = std::min(fileSize - bytesSent, static_cast<size_t>(MAX_PAYLOAD_SIZE));
                
                pkt.type = PACKET_TYPE_DATA;
                pkt.length = chunkSize;
                memcpy(pkt.payload, buffer.data() + bytesSent, chunkSize);
                
                if (!conn.network->sendPacket(pkt)) {
                    std::cerr << "[SERVER] Failed to send file chunk" << std::endl;
                    break;
                }
                
                bytesSent += chunkSize;
            }
            
            std::cout << "[SERVER] Successfully broadcasted file to " << sourceUsername 
                      << " (connection " << conn.connectionId << ")" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[SERVER] Error during broadcast to connection " << conn.connectionId 
                      << ": " << e.what() << std::endl;
        }
    }
}};

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
