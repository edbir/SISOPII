#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include "network.h"
#include "file_manager.h"
#include "sync_manager.h"

class Client {
public:
    Client(const std::string& username, const std::string& serverIP, int port)
        : username(username), serverIP(serverIP), port(port), fileManager("client_data") {
        if (!network.connectToServer(serverIP, port)) {
            throw std::runtime_error("Failed to connect to server");
        }

        // Send username to server
        packet pkt;
        pkt.type = PACKET_TYPE_CMD;
        pkt.seqn = 0;
        pkt.total_size = 1;
        pkt.length = username.length();
        memcpy(pkt.payload, username.c_str(), username.length());
        
        if (!network.sendPacket(pkt)) {
            throw std::runtime_error("Failed to send username to server");
        }

        // Wait for server acknowledgment
        packet ack;
        if (!network.receivePacket(ack) || ack.type != PACKET_TYPE_ACK) {
            throw std::runtime_error("Failed to receive server acknowledgment");
        }
    }

    void run() {
        std::cout << "Connected to server as " << username << std::endl;
        std::cout << "Type 'help' for available commands" << std::endl;

        // Start sync manager
        SyncManager syncManager(network, fileManager);
        syncManager.startSync(username);

        std::string command;
        while (true) {
            std::cout << "> ";
            std::getline(std::cin, command);

            if (command == "help") {
                printHelp();
            } else if (command.substr(0, 6) == "upload") {
                handleUpload(command.substr(7), syncManager);
            } else if (command.substr(0, 8) == "download") {
                handleDownload(command.substr(9), syncManager);
            } else if (command.substr(0, 6) == "delete") {
                handleDelete(command.substr(7), syncManager);
            } else if (command == "list_server") {
                handleListServer(syncManager);
            } else if (command == "list_client") {
                handleListClient(syncManager);
            } else if (command == "get_sync_dir") {
                handleGetSyncDir(syncManager);
            } else if (command == "exit") {
                handleExit(syncManager);
                break;
            } else {
                std::cout << "Unknown command. Type 'help' for available commands." << std::endl;
            }
        }
    }

private:
    std::string username;
    std::string serverIP;
    int port;
    NetworkManager network;
    FileManager fileManager;

    void printHelp() {
        std::cout << "Available commands:" << std::endl;
        std::cout << "  upload <path/filename.ext> - Upload a file to the server" << std::endl;
        std::cout << "  download <filename.ext> - Download a file from the server" << std::endl;
        std::cout << "  delete <filename.ext> - Delete a file from sync_dir" << std::endl;
        std::cout << "  list_server - List files on the server" << std::endl;
        std::cout << "  list_client - List files in sync_dir" << std::endl;
        std::cout << "  get_sync_dir - Create sync_dir and start synchronization" << std::endl;
        std::cout << "  exit - Close the session" << std::endl;
    }

    void handleUpload(const std::string& filepath, SyncManager& syncManager) {
        if (!syncManager.uploadFile(username, filepath)) {
            std::cerr << "Error uploading file" << std::endl;
        }
    }

    void handleDownload(const std::string& filename, SyncManager& syncManager) {
        if (!syncManager.downloadFile(username, filename)) {
            std::cerr << "Error downloading file" << std::endl;
        }
    }

    void handleDelete(const std::string& filename, SyncManager& syncManager) {
        if (!syncManager.deleteFile(username, filename)) {
            std::cerr << "Error deleting file" << std::endl;
        }
    }

    void handleListServer(SyncManager& syncManager) {
        std::vector<file_metadata> files;
        if (syncManager.listServerFiles(username, files)) {
            std::cout << "Files on server:" << std::endl;
            for (const auto& file : files) {
                std::cout << "  " << file.filename << std::endl;
                std::cout << "    Size: " << file.size << " bytes" << std::endl;
                std::cout << "    Modified: " << std::ctime(&file.mtime);
                std::cout << "    Accessed: " << std::ctime(&file.atime);
                std::cout << "    Created: " << std::ctime(&file.ctime);
            }
        } else {
            std::cerr << "Error listing server files" << std::endl;
        }
    }

    void handleListClient(SyncManager& syncManager) {
        std::vector<file_metadata> files;
        if (syncManager.listClientFiles(username, files)) {
            std::cout << "Files in sync_dir:" << std::endl;
            for (const auto& file : files) {
                std::cout << "  " << file.filename << std::endl;
                std::cout << "    Size: " << file.size << " bytes" << std::endl;
                std::cout << "    Modified: " << std::ctime(&file.mtime);
                std::cout << "    Accessed: " << std::ctime(&file.atime);
                std::cout << "    Created: " << std::ctime(&file.ctime);
            }
        } else {
            std::cerr << "Error listing client files" << std::endl;
        }
    }

    void handleGetSyncDir(SyncManager& syncManager) {
        std::string syncDir = "sync_dir_" + username;
        if (!std::filesystem::exists(syncDir)) {
            std::filesystem::create_directory(syncDir);
        }
        std::cout << "Sync directory created/verified: " << syncDir << std::endl;
    }

    void handleExit(SyncManager& syncManager) {
        packet pkt;
        pkt.type = PACKET_TYPE_CMD;
        pkt.seqn = 0;
        pkt.total_size = 1;
        uint16_t cmd = CMD_EXIT;
        pkt.length = sizeof(cmd);
        memcpy(pkt.payload, &cmd, sizeof(cmd));
        network.sendPacket(pkt);
        network.closeConnection();
    }
};

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <username> <server_ip_address> <port>" << std::endl;
        return 1;
    }

    try {
        std::string username = argv[1];
        std::string serverIP = argv[2];
        int port = std::stoi(argv[3]);

        Client client(username, serverIP, port);
        client.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
} 