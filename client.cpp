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

        fileManager.createUserDir(username);
    }

    void run() {
        std::cout << "Connected to server as " << username << std::endl;
        std::cout << "Type 'help' for available commands" << std::endl;

        // Start sync manager
        SyncManager syncManager(network, fileManager);
        if (!syncManager.startSync(username)) {
            std::cerr << "Failed to start sync manager" << std::endl;
            return;
        }

        std::string command;
        while (true) {
            try {
                std::cout << "> ";
                std::getline(std::cin, command);

                if (command == "help") {
                    printHelp();
                } else if (command.substr(0, 6) == "upload") {
                    if (command.length() <= 7) {
                        std::cerr << "Usage: upload <path/filename.ext>" << std::endl;
                        continue;
                    }
                    handleUpload(command.substr(7), syncManager);
                } else if (command == "list_server") {
                    handleListServer();            
                } else if (command == "list_client") {
                    handleListClient();
                } else if (command.substr(0, 6) == "delete") {
                    if (command.length() <= 7) {
                        std::cerr << "Usage: delete <filename>" << std::endl;
                        continue;
                    }
                    std::string filename = command.substr(7);
                    if (!syncManager.deleteFile(username, filename)) {
                        std::cerr << "Error deleting file" << std::endl;
                    }
                } else if (command == "exit") {
                    handleExit(syncManager);
                    break;
                } else {
                    std::cout << "Unknown command. Type 'help' for available commands." << std::endl;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << std::endl;
                std::cerr << "Please try again." << std::endl;
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
        std::cout << "  list_server - List files stored on the server for your user" << std::endl;
        std::cout << "  list_client - List files stored locally for your user" << std::endl;
        std::cout << "  delete - Delete a file to the server" << std::endl;
        std::cout << "  exit - Close the session" << std::endl;
    }

    void handleUpload(const std::string& filepath, SyncManager& syncManager) {
        try {
            if (!syncManager.uploadFile(username, filepath)) {
                std::cerr << "Error uploading file. Please try again." << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "Error during upload: " << e.what() << std::endl;
            std::cerr << "Please try again." << std::endl;
        }
    }

    void handleListServer() {
        packet pkt;
        pkt.type = PACKET_TYPE_CMD;
        pkt.seqn = 0;
        pkt.total_size = 1;
        uint16_t cmd = CMD_LIST_SERVER;
        pkt.length = sizeof(cmd);
        memcpy(pkt.payload, &cmd, sizeof(cmd));
    
        if (!network.sendPacket(pkt)) {
            std::cerr << "Failed to send list_server command" << std::endl;
            return;
        }
    
        // Recebe a resposta do servidor com os nomes dos arquivos
        packet response;
        if (!network.receivePacket(response) || response.type != PACKET_TYPE_DATA) {
            std::cerr << "Failed to receive file list from server" << std::endl;
            return;
        }
    
        std::string fileList(response.payload, response.payload + response.length);
        std::cout << "Files on server:" << std::endl;
        std::cout << fileList << std::endl;
    }   
    
    void handleListClient() {
        std::vector<file_metadata> files;
    
        if (!fileManager.listFiles(username, files)) {
            std::cerr << "Failed to list local files for user " << username << std::endl;
            return;
        }
    
        if (files.empty()) {
            std::cout << "No local files found for user " << username << std::endl;
            return;
        }
    
        std::cout << "Local files for user " << username << ":" << std::endl;
        for (const auto& file : files) {
            std::cout << "  " << file.filename << " (" << file.size << " bytes)" << std::endl;
        }
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