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
        std::cout << "Server running on port " << port << std::endl;

        while (true) {
            if (!network.acceptConnection()) {
                std::cerr << "Error accepting connection" << std::endl;
                continue;
            }

            // Handle client connection in a new thread
            std::thread clientThread(&Server::handleClient, this);
            clientThread.detach();
        }
    }

private:
    int port;
    NetworkManager network;
    FileManager fileManager;
    std::map<std::string, SyncManager*> userSyncManagers;
    std::mutex syncManagerMutex;

    void handleClient() {
        packet pkt;
        if (!network.receivePacket(pkt)) {
            std::cerr << "[SERVER] Failed to receive initial packet" << std::endl;
            return;
        }

        if (pkt.type != PACKET_TYPE_CMD) {
            std::cerr << "[SERVER] Invalid initial packet type: " << pkt.type << std::endl;
            return;
        }

        std::string username(pkt.payload, pkt.length);
        std::cout << "[SERVER] New client connected: " << username << std::endl;

        // Send ACK for username
        pkt.type = PACKET_TYPE_ACK;
        pkt.length = 0;
        if (!network.sendPacket(pkt)) {
            std::cerr << "[SERVER] Failed to send username ACK" << std::endl;
            return;
        }

        // Create user directory if it doesn't exist
        std::cout << "[SERVER] Creating user directory for: " << username << std::endl;
        fileManager.createUserDir(username);

        // Create sync manager for user if it doesn't exist
        std::lock_guard<std::mutex> lock(syncManagerMutex);
        if (userSyncManagers.find(username) == userSyncManagers.end()) {
            userSyncManagers[username] = new SyncManager(network, fileManager);
        }
        SyncManager* syncManager = userSyncManagers[username];

        // Handle client commands
        bool inUpload = false;
        std::string currentFilename;
        size_t expectedChunks = 0;
        size_t receivedChunks = 0;

        while (true) {
            if (!network.receivePacket(pkt)) {
                std::cerr << "[SERVER] Failed to receive packet" << std::endl;
                break;
            }

            std::cout << "[SERVER] Received packet type: " << pkt.type << std::endl;

            switch (pkt.type) {
                case PACKET_TYPE_CMD: {
                    if (inUpload) {
                        std::cerr << "[SERVER] Received command while upload in progress" << std::endl;
                        continue;
                    }

                    uint16_t cmd;
                    memcpy(&cmd, pkt.payload, sizeof(cmd));
                    std::cout << "[SERVER] Received command: " << cmd << std::endl;

                    switch (cmd) {
                        case CMD_UPLOAD: {
                            std::cout << "[SERVER] Processing CMD_UPLOAD" << std::endl;
                            inUpload = true;
                            receivedChunks = 0;
                            expectedChunks = 0;
                            currentFilename.clear();
                            
                            // Send ACK for CMD_UPLOAD
                            pkt.type = PACKET_TYPE_ACK;
                            pkt.length = 0;
                            if (!network.sendPacket(pkt)) {
                                std::cerr << "[SERVER] Failed to send CMD_UPLOAD ACK" << std::endl;
                                return;
                            }
                            std::cout << "[SERVER] Sent ACK for CMD_UPLOAD" << std::endl;
                            break;
                        }
                        case CMD_DOWNLOAD: {
                            std::cout << "[SERVER] Processing CMD_DOWNLOAD" << std::endl;
                            // Receive filename
                            if (!network.receivePacket(pkt)) {
                                std::cerr << "[SERVER] Failed to receive filename" << std::endl;
                                return;
                            }
                            std::string filename(pkt.payload, pkt.length);
                            std::cout << "[SERVER] Received download request for: " << filename << std::endl;

                            // Send file
                            std::string filepath = fileManager.getUserDir(username) + "/" + filename;
                            if (!network.sendFile(filepath)) {
                                std::cerr << "[SERVER] Failed to send file" << std::endl;
                                return;
                            }
                            std::cout << "[SERVER] File sent successfully" << std::endl;
                            break;
                        }
                        case CMD_DELETE: {
                            std::cout << "[SERVER] Processing CMD_DELETE" << std::endl;
                            // Receive filename
                            if (!network.receivePacket(pkt)) {
                                std::cerr << "[SERVER] Failed to receive filename" << std::endl;
                                return;
                            }
                            std::string filename(pkt.payload, pkt.length);
                            std::cout << "[SERVER] Received delete request for: " << filename << std::endl;

                            // Delete file
                            if (!fileManager.deleteFile(username, filename)) {
                                std::cerr << "[SERVER] Failed to delete file" << std::endl;
                                return;
                            }

                            // Broadcast to other clients
                            syncManager->broadcastFileChange(username, filename, true);
                            break;
                        }
                        case CMD_LIST_SERVER: {
                            std::cout << "[SERVER] Processing CMD_LIST_SERVER" << std::endl;
                            // List files in user directory
                            std::vector<file_metadata> files;
                            if (!fileManager.listFiles(username, files)) {
                                std::cerr << "[SERVER] Failed to list files" << std::endl;
                                return;
                            }

                            // Send file list
                            packet response;
                            response.type = PACKET_TYPE_DATA;
                            response.seqn = 0;
                            response.total_size = 1;
                            response.length = files.size() * sizeof(file_metadata);
                            memcpy(response.payload, files.data(), response.length);
                            if (!network.sendPacket(response)) {
                                std::cerr << "[SERVER] Failed to send file list" << std::endl;
                                return;
                            }
                            std::cout << "[SERVER] Sent file list (" << files.size() << " files)" << std::endl;
                            break;
                        }
                        case CMD_EXIT:
                            return;
                        default:
                            std::cerr << "[SERVER] Unknown command: " << cmd << std::endl;
                            return;
                    }
                    break;
                }
                case PACKET_TYPE_FILE: {
                    if (!inUpload) {
                        std::cerr << "[SERVER] Received file packet without upload command" << std::endl;
                        continue;
                    }

                    // Only accept filename if currentFilename is empty
                    if (currentFilename.empty()) {
                        currentFilename = std::string(pkt.payload, pkt.length);
                        std::cout << "[SERVER] Received filename: " << currentFilename << std::endl;
                        // Send ACK for filename
                        pkt.type = PACKET_TYPE_ACK;
                        pkt.length = 0;
                        if (!network.sendPacket(pkt)) {
                            std::cerr << "[SERVER] Failed to send filename ACK" << std::endl;
                            return;
                        }
                    }
                    // Only accept file size if currentFilename is set and expectedChunks is 0
                    else if (expectedChunks == 0) {
                        size_t fileSize;
                        memcpy(&fileSize, pkt.payload, sizeof(fileSize));
                        std::cout << "[SERVER] Received file size: " << fileSize << " bytes" << std::endl;
                        expectedChunks = (fileSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;

                        // Send ACK for file size
                        pkt.type = PACKET_TYPE_ACK;
                        pkt.length = 0;
                        if (!network.sendPacket(pkt)) {
                            std::cerr << "[SERVER] Failed to send file size ACK" << std::endl;
                            return;
                        }

                        // Create/truncate file for new upload
                        std::string filepath = fileManager.getUserDir(username) + "/" + currentFilename;
                        std::cout << "[SERVER] Creating file: " << filepath << std::endl;
                        std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
                        if (!file.is_open()) {
                            std::cerr << "[SERVER] Failed to create file" << std::endl;
                            return;
                        }
                        file.close();
                    }
                    break;
                }
                case PACKET_TYPE_DATA: {
                    if (!inUpload || currentFilename.empty() || expectedChunks == 0) {
                        std::cerr << "[SERVER] Received data without upload in progress or missing filename/size" << std::endl;
                        continue;
                    }

                    std::string filepath = fileManager.getUserDir(username) + "/" + currentFilename;
                    std::ofstream file(filepath, std::ios::binary | std::ios::app);
                    if (!file.is_open()) {
                        std::cerr << "[SERVER] Failed to open file for writing" << std::endl;
                        return;
                    }

                    file.write(pkt.payload, pkt.length);
                    file.close();

                    std::cout << "[SERVER] Received chunk " << (receivedChunks + 1) << " of " << expectedChunks 
                              << " (size: " << pkt.length << " bytes)" << std::endl;
                    
                    // Log chunk content
                    std::cout << "[SERVER] Chunk content (first 32 bytes): ";
                    for (size_t i = 0; i < std::min(static_cast<size_t>(pkt.length), size_t(32)); i++) {
                        printf("%02x ", (unsigned char)pkt.payload[i]);
                    }
                    std::cout << std::endl;
                    
                    std::string content(pkt.payload, std::min(static_cast<size_t>(pkt.length), size_t(32)));
                    std::cout << "[SERVER] Chunk content as string: " << content << std::endl;

                    receivedChunks++;

                    // Send ACK for chunk
                    pkt.type = PACKET_TYPE_ACK;
                    pkt.length = 0;
                    if (!network.sendPacket(pkt)) {
                        std::cerr << "[SERVER] Failed to send chunk ACK" << std::endl;
                        return;
                    }

                    // Check if upload is complete
                    if (receivedChunks >= expectedChunks) {
                        std::cout << "[SERVER] Upload completed for: " << currentFilename << std::endl;
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

        // Clean up
        network.closeConnection();
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