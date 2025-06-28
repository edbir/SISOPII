#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <mutex>
#include <cstring>
#include <chrono> // For heartbeat timing
#include "network.h"
#include "file_manager.h"
#include "sync_manager.h"
#include <filesystem>
#include <fstream>
#include <atomic>
#include <algorithm>

namespace fs = std::filesystem;

// Add a new command for heartbeat
#define CMD_HEARTBEAT 8

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

class ReplicationManager {
private:
    std::vector<std::pair<std::string, int>> backupServers;
    std::mutex backupMutex;

public:
    void addBackupServer(const std::string& ip, int port) {
        std::lock_guard<std::mutex> lock(backupMutex);
        backupServers.emplace_back(ip, port);
    }

    void removeBackupServer(const std::string& ip, int port) {
        std::lock_guard<std::mutex> lock(backupMutex);
        backupServers.erase(
            std::remove_if(backupServers.begin(), backupServers.end(),
                [&](const auto& server) {
                    return server.first == ip && server.second == port;
                }),
            backupServers.end());
    }

    std::vector<std::pair<std::string, int>> getBackupServers() {
        std::lock_guard<std::mutex> lock(backupMutex);
        return backupServers;
    }
};

class Server {
public:
    Server(int port, bool isLeader) 
    : port(port), fileManager(isLeader ? "leader_data" : "backup" + std::to_string(port)), 
      isLeader(isLeader), running(true),
      heartbeatInterval(std::chrono::seconds(3)), // Send heartbeat every 3 seconds
      heartbeatTimeout(std::chrono::seconds(10)), // Leader fails if no heartbeat for 10 seconds
      leaderActive(false) // Initially assume leader is not active for backups
      {
    
    // Create server-specific root directory
    if (!fs::exists(fileManager.getBasePath())) {
        fs::create_directory(fileManager.getBasePath());
        std::cout << "Created " << fileManager.getBasePath() << " directory\n";
    }

    // Leader-specific setup
    if (isLeader) {
        // Initialize with default backup servers (can be registered dynamically too)
        replicationManager.addBackupServer("127.0.0.1", 8081);  // Backup 1
        replicationManager.addBackupServer("192.168.0.11", 8082); // Backup 2
        std::cout << "[LEADER] Ready with " << replicationManager.getBackupServers().size() << " backups\n";
        heartbeatThread = std::thread(&Server::sendHeartbeats, this);
    }
    else {
        std::cout << "[BACKUP] Ready at " << fileManager.getBasePath() << "\n";
        leaderActive = false; // Backup starts assuming leader is not active until first heartbeat
        // Backup servers will monitor the leader heartbeat
        heartbeatThread = std::thread(&Server::monitorLeaderHeartbeat, this);
    }

    if (!network.initServer(port)) {
        throw std::runtime_error("Port busy");
    }
}

    ~Server() {
        running = false;
        if (heartbeatThread.joinable()) {
            heartbeatThread.join();
        }
        // Limpar todas as conexões ativas
        std::lock_guard<std::mutex> lock(connectionsMutex);
        for (auto& [username, connections] : userConnections) {
            for (auto& conn : connections) {
                if (conn.syncManager) delete conn.syncManager;
                if (conn.network) { // Ensure network is not null before deleting
                    conn.network->closeConnection();
                    delete conn.network;
                }
                if (conn.clientThread) {
                    if (conn.clientThread->joinable()) {
                        conn.clientThread->join(); // Join detached threads if possible, or detach them properly
                    }
                    delete conn.clientThread;
                }
            }
        }
    }

    // register backup servers
    void registerBackupServer(const std::string& ip, int port) {
        if (isLeader) {
            replicationManager.addBackupServer(ip, port);
            std::cout << "[SERVER] Registered backup server: " << ip << ":" << port << std::endl;
        }
    }

    void run() {
        std::cout << "[SERVER] Server running on port " << port << std::endl;

        while (running) {
            if (!network.acceptConnection()) {
                std::cerr << "[SERVER] Error accepting connection" << std::endl;
                // Small sleep here to prevent busy-waiting on accept failure
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    ReplicationManager replicationManager;
    bool isLeader;

    std::atomic<bool> running; // To control main server loop and heartbeat thread
    std::thread heartbeatThread; // For sending/monitoring heartbeats
    std::chrono::seconds heartbeatInterval; // How often leader sends heartbeats
    std::chrono::seconds heartbeatTimeout;  // How long backup waits for heartbeat
    std::chrono::steady_clock::time_point lastHeartbeatTime; // Last time backup received heartbeat
    std::mutex heartbeatMutex; // Protects lastHeartbeatTime
    std::atomic<bool> leaderActive; // State of the leader from backup's perspective

    // Leader: Sends heartbeats to backups
    void sendHeartbeats() {
        while (running) {
            auto backups = replicationManager.getBackupServers();
            for (const auto& [ip, port] : backups) {
                NetworkManager nm;
                if (nm.connectToServer(ip, port)) {
                    packet pkt;
                    pkt.type = PACKET_TYPE_CMD;
                    uint16_t cmd = CMD_HEARTBEAT;
                    pkt.length = sizeof(cmd);
                    memcpy(pkt.payload, &cmd, sizeof(cmd));
                    if (nm.sendPacket(pkt)) {
                        // std::cout << "[LEADER] Sent heartbeat to " << ip << ":" << port << std::endl; // Too chatty
                    } else {
                        std::cerr << "[LEADER] Failed to send heartbeat to " << ip << ":" << port << std::endl;
                    }
                    nm.closeConnection(); // Close connection after sending heartbeat
                } else {
                    std::cerr << "[LEADER] Could not connect to backup " << ip << ":" << port << " for heartbeat." << std::endl;
                }
            }
            std::this_thread::sleep_for(heartbeatInterval);
        }
    }

    // Backup: Monitors heartbeats from leader
    void monitorLeaderHeartbeat() {
        // For a backup, it will listen on its own server socket for connections from the leader
        // and handle them via handleReplicationCommand.
        // This separate thread is to actively check if the lastHeartbeatTime has exceeded the timeout.
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Check every second

            std::lock_guard<std::mutex> lock(heartbeatMutex);
            if (std::chrono::steady_clock::now() - lastHeartbeatTime > heartbeatTimeout) {
                if (leaderActive) {
                    std::cerr << "[BACKUP] LEADER FAILED! No heartbeat received for " 
                              << heartbeatTimeout.count() << " seconds." << std::endl;
                    leaderActive = false;
                    // Here, you would typically trigger leader election
                }
            } else {
                if (!leaderActive) {
                    std::cout << "[BACKUP] Leader is active." << std::endl;
                    leaderActive = true;
                }
            }
        }
    }

    void handleClient(NetworkManager* clientNetwork) {
        std::string username; // Will remain empty for backup-side connections
        try {
            if (isLeader) {
                // Leader logic: handle regular client connections
                packet pkt;
                if (!clientNetwork->receivePacket(pkt) || pkt.type != PACKET_TYPE_CMD) {
                    std::cerr << "[SERVER] Initial packet was not a CMD or failed to receive on leader. Closing connection." << std::endl;
                    clientNetwork->closeConnection();
                    delete clientNetwork;
                    return;
                }
                username = std::string(pkt.payload, pkt.length);
                std::cout << "[SERVER] New client connected: " << username << std::endl;

                // Check device limit - maximum 2 devices per user
                {
                    std::lock_guard<std::mutex> lock(connectionsMutex);
                    auto& userConnectionsList = userConnections[username];
                    
                    // Count active connections for this user
                    int activeConnections = 0;
                    for (const auto& conn : userConnectionsList) {
                        if (conn.isActive) {
                            activeConnections++;
                        }
                    }
                    
                    if (activeConnections >= 2) {
                        std::cout << "[SERVER] User " << username << " already has " << activeConnections 
                                  << " active connections. Rejecting new connection." << std::endl;
                        
                        // Send rejection packet
                        packet rejectPkt;
                        rejectPkt.type = PACKET_TYPE_CMD;
                        rejectPkt.length = sizeof(uint16_t);
                        uint16_t rejectCmd = CMD_CONNECTION_REJECTED;
                        memcpy(rejectPkt.payload, &rejectCmd, sizeof(rejectCmd));
                        
                        if (clientNetwork->sendPacket(rejectPkt)) {
                            std::cout << "[SERVER] Sent connection rejection to " << username << std::endl;
                        } else {
                            std::cerr << "[SERVER] Failed to send rejection packet to " << username << std::endl;
                        }
                        
                        clientNetwork->closeConnection();
                        delete clientNetwork;
                        return;
                    }
                }

                pkt.type = PACKET_TYPE_ACK;
                pkt.length = 0;
                if (!clientNetwork->sendPacket(pkt)) {
                    std::cerr << "[SERVER] Failed to send username ACK. Closing connection." << std::endl;
                    clientNetwork->closeConnection();
                    delete clientNetwork;
                    return;
                }

                if (!fileManager.createUserDir(username)) {
                    std::cerr << "[SERVER] Failed to create user directory. Closing connection." << std::endl;
                    clientNetwork->closeConnection();
                    delete clientNetwork;
                    return;
                }

                { // Scope for lock_guard
                    std::lock_guard<std::mutex> lock(connectionsMutex);
                    ClientConnection conn;
                    conn.network = clientNetwork;
                    conn.syncManager = new SyncManager(*clientNetwork, fileManager); // Assuming FileManager is available here
                    conn.clientThread = nullptr; // This thread itself
                    conn.isActive = true;
                    conn.connectionId = std::to_string(nextConnectionId++);
                    userConnections[username].push_back(std::move(conn));
                }

                // Handle regular client commands for the leader
                handleClientCommands(username, clientNetwork);

                // Cleanup for regular client connection
                std::lock_guard<std::mutex> lock(connectionsMutex);
                for (auto it = userConnections[username].begin(); it != userConnections[username].end(); ++it) {
                    if (it->network == clientNetwork) {
                        it->isActive = false;
                        // For now, just mark inactive. If full removal is desired, need careful iterator handling.
                        break; 
                    }
                }
                std::cout << "[SERVER] Client " << username << " disconnected" << std::endl;


            } else { // This is a backup server handling an incoming connection
                packet pkt;
                if (!clientNetwork->receivePacket(pkt)) {
                    std::cerr << "[BACKUP] Failed to receive initial packet from leader. Connection likely closed immediately." << std::endl;
                    clientNetwork->closeConnection();
                    delete clientNetwork;
                    return; // Graceful exit for ephemeral connection
                }

                if (pkt.type == PACKET_TYPE_CMD) {
                    uint16_t cmd_val;
                    memcpy(&cmd_val, pkt.payload, sizeof(cmd_val));

                    if (cmd_val == CMD_HEARTBEAT) {
                        std::lock_guard<std::mutex> lock(heartbeatMutex);
                        lastHeartbeatTime = std::chrono::steady_clock::now();
                        if (!leaderActive) {
                            leaderActive = true;
                            std::cout << "[BACKUP] Received first heartbeat. Leader is active." << std::endl;
                        }
                        std::cout << "[BACKUP] Received heartbeat. Leader is active." << std::endl;
                        // std::cout << "[BACKUP] Heartbeat received." << std::endl; // Too chatty for frequent heartbeats
                        // Leader closes connection immediately after sending heartbeat.
                        // No further packets expected on this connection.
                    } else if (cmd_val == CMD_REPLICATE) {
                        std::cout << "[BACKUP] Received replication command." << std::endl;
                        handleReplicationCommand(clientNetwork);
                    } else if (cmd_val == CMD_DELETE) { // Handle replicated deletion
                        std::string user;
                        std::string fname;
                        if (clientNetwork->receivePacket(pkt) && pkt.type == PACKET_TYPE_FILE) {
                            user = std::string(pkt.payload, pkt.length);
                            if (clientNetwork->receivePacket(pkt) && pkt.type == PACKET_TYPE_FILE) {
                                fname = std::string(pkt.payload, pkt.length);
                                std::string filepath = fileManager.getUserDir(user) + "/" + fname;
                                bool success = false;
                                try {
                                    success = fs::remove(filepath);
                                    std::cout << "[BACKUP][DELETE] Replicated deletion " << (success ? "succeeded" : "failed")
                                              << " for: " << filepath << std::endl;
                                } catch (const fs::filesystem_error& e) {
                                    std::cerr << "[BACKUP][ERROR] Filesystem exception during replicated delete: " << e.what() << std::endl;
                                }
                            }
                        }
                    } else {
                        std::cerr << "[BACKUP] Received unexpected command: " << cmd_val << std::endl;
                    }
                } else {
                    std::cerr << "[BACKUP] Received unexpected packet type on initial connect: " << (int)pkt.type << std::endl;
                }
                
                clientNetwork->closeConnection();
                delete clientNetwork;
                // std::cout << "[BACKUP] Incoming connection handled and closed." << std::endl; // Too chatty for heartbeats
                return; // Thread done.

            } // End of !isLeader branch

        } catch (const std::exception& e) {
            std::cerr << "[SERVER] Error handling client " << (username.empty() ? "unidentified/replication/heartbeat" : username) << ": " << e.what() << std::endl;
        }

        // The NetworkManager and socket are cleaned up inside the branches for both leader and backup paths.
    }

    void handleClientCommands(const std::string& username, NetworkManager* clientNetwork) {
     bool inUpload = false;
     std::string currentFilename;
     size_t expectedChunks = 0;
     size_t receivedChunks = 0;
     packet pkt;

    // This function now ONLY handles commands for leader's regular clients.
    // Backup's incoming connections are handled directly in handleClient.

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
                        
                        // If leader, replicate deletion to backups
                        if (isLeader && success) {
                            replicateDeletionToBackups(username, filename);
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
                } else {
                    std::cerr << "[SERVER] Unknown command received: " << cmd << std::endl;
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
                    uploadLock.unlock(); // Release lock before broadcast, as broadcast might try to acquire its own locks
                    broadcastFileChange(username, currentFilename, filepath, clientNetwork->getSocket());
                    uploadLock.lock(); // Reacquire for safety if more processing here
                    
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

void replicateToBackups(const std::string& username, const std::string& filename, 
                       const std::string& filepath) {
    std::thread([this, username, filename, filepath]() {
            // Read file content once
            std::ifstream file(filepath, std::ios::binary);
            if (!file.is_open()) {
                std::cerr << "[REPLICATION] Can't open " << filepath << std::endl;
                return;
            }
            file.seekg(0, std::ios::end);
            size_t fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            
            std::vector<char> buffer(fileSize);
            file.read(buffer.data(), fileSize);
            file.close();

            // Send to each backup
            auto backups = replicationManager.getBackupServers();
            for (const auto& [ip, port] : backups) {
                NetworkManager nm;
                if (!nm.connectToServer(ip, port)) {
                    std::cerr << "[REPLICATION] Can't connect to backup " 
                              << ip << ":" << port << std::endl;
                    continue;
                }
                // Send replication command sequence
                packet pkt;
                
                // Command
                pkt.type = PACKET_TYPE_CMD;
                uint16_t cmd = CMD_REPLICATE;
                pkt.length = sizeof(cmd);
                memcpy(pkt.payload, &cmd, sizeof(cmd));
                if (!nm.sendPacket(pkt)) { nm.closeConnection(); continue; }

                // Username
                pkt.type = PACKET_TYPE_FILE; // Reusing FILE type to indicate metadata like filename/username
                pkt.length = username.size();
                memcpy(pkt.payload, username.c_str(), pkt.length);
                if (!nm.sendPacket(pkt)) { nm.closeConnection(); continue; }

                // Filename
                pkt.length = filename.size();
                memcpy(pkt.payload, filename.c_str(), pkt.length);
                if (!nm.sendPacket(pkt)) { nm.closeConnection(); continue; }

                // File content (as a single packet for simplicity for replication)
                // Note: For very large files, this would need to be chunked like normal file transfer.
                pkt.type = PACKET_TYPE_DATA;
                pkt.length = fileSize;
                // Ensure payload buffer is large enough for fileSize
                if (fileSize > MAX_PAYLOAD_SIZE) {
                    std::cerr << "[REPLICATION] File too large for single packet replication. Skipping." << std::endl;
                    nm.closeConnection();
                    continue;
                }
                memcpy(pkt.payload, buffer.data(), fileSize);
                if (!nm.sendPacket(pkt)) { nm.closeConnection(); continue; }

                std::cout << "[REPLICATION] Sent " << filename << " to backup " << ip << ":" << port << std::endl;
                nm.closeConnection();
            }
        }).detach();
}

void replicateDeletionToBackups(const std::string& username, const std::string& filename) {
    std::thread([this, username, filename]() {
        auto backups = replicationManager.getBackupServers();
        for (const auto& [ip, port] : backups) {
            NetworkManager nm;
            if (!nm.connectToServer(ip, port)) {
                std::cerr << "[REPLICATION][DELETE] Can't connect to backup "
                          << ip << ":" << port << std::endl;
                continue;
            }

            packet pkt;
            // Command: DELETE for replication
            pkt.type = PACKET_TYPE_CMD;
            uint16_t cmd = CMD_DELETE; // Reuse CMD_DELETE, but it's sent as a replication
            pkt.length = sizeof(cmd);
            memcpy(pkt.payload, &cmd, sizeof(cmd));
            if (!nm.sendPacket(pkt)) { nm.closeConnection(); continue; }

            // Username
            pkt.type = PACKET_TYPE_FILE; // Reusing FILE type for metadata
            pkt.length = username.size();
            memcpy(pkt.payload, username.c_str(), pkt.length);
            if (!nm.sendPacket(pkt)) { nm.closeConnection(); continue; }

            // Filename
            pkt.length = filename.size();
            memcpy(pkt.payload, filename.c_str(), pkt.length);
            if (!nm.sendPacket(pkt)) { nm.closeConnection(); continue; }

            std::cout << "[REPLICATION][DELETE] Sent deletion for " << filename
                      << " to backup " << ip << ":" << port << std::endl;
            nm.closeConnection();
        }
    }).detach();
}


// Handler for replication commands (for backup servers)
// This function needs to be aware if it's handling a file, a deletion, or a heartbeat.
void handleReplicationCommand(NetworkManager* clientNetwork) {
    packet pkt;
    
    // The first packet after CMD_REPLICATE should be the username
    if (!clientNetwork->receivePacket(pkt) || pkt.type != PACKET_TYPE_FILE) {
        std::cerr << "[BACKUP][REPLICATION] Expected username packet after CMD_REPLICATE." << std::endl;
        return;
    }
    std::string username(pkt.payload, pkt.length);
    
    // 2. Create user directory if needed
    // This fileManager refers to the Server's fileManager, as handleReplicationCommand is a member of Server.
    if (!fileManager.createUserDir(username)) { 
        std::cerr << "[BACKUP] Failed to create user directory for replication\n";
        return;
    }

    // Now expect the filename
    if (!clientNetwork->receivePacket(pkt) || pkt.type != PACKET_TYPE_FILE) {
        std::cerr << "[BACKUP][REPLICATION] Expected filename packet after username." << std::endl;
        return;
    }
    std::string filename(pkt.payload, pkt.length);

    // After filename, it should be DATA for a file replication
    if (!clientNetwork->receivePacket(pkt) || pkt.type != PACKET_TYPE_DATA) {
        std::cerr << "[BACKUP][REPLICATION] Failed to receive file content." << std::endl;
        return;
    }

    // This is a file replication
    std::string filepath = fileManager.getUserDir(username) + "/" + filename;
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[BACKUP] Failed to create replicated file: " << filepath << "\n";
        return;
    }
    file.write(pkt.payload, pkt.length);
    file.close();
    
    std::cout << "[BACKUP] Stored replicated file " << filepath << "\n";
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
    // If this is the leader, replicate to backups
    if (isLeader) {
        replicateToBackups(sourceUsername, filename, filepath);
    }
}};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <port> <role>\n"
                  << "Roles: 'leader' or 'backup'\n";
        return 1;
    }

    try {
        int port = std::stoi(argv[1]);
        std::string role = argv[2];
        
        if (role != "leader" && role != "backup") {
            std::cerr << "Invalid role. Use 'leader' or 'backup'\n";
            return 1;
        }

        bool isLeader = (role == "leader");
        
        // Simple port validation
        if (port < 1024 || port > 65535) {
            std::cerr << "Port must be between 1024 and 65535\n";
            return 1;
        }

        std::cout << "Starting as " << (isLeader ? "LEADER" : "BACKUP") 
                  << " on port " << port << "\n";
        
        Server server(port, isLeader);
        server.run();
        
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}