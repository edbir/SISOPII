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
        : username(username), serverIP(serverIP), port(port), fileManager("client_data"), syncManager(network, fileManager) {
            
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
        //SyncManager syncManager(network, fileManager);

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
                 
                    handleUpload(command.substr(7));
                } else if (command.substr(0, 8) == "download") {
                    if (command.length() <= 9) {
                        std::cerr << "Usage: download <filename>" << std::endl;
                        continue;
                    }
                    handleDownload(command.substr(9));
                } else if (command == "list_server") {
                    handleListServer();            
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
                
                    if (!this->syncManager.deleteFile(username, filename)) {
                        std::cerr << "Error deleting file" << std::endl;
                    }
                } else if (command == "exit") {
                    handleExit();
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
    SyncManager syncManager; // Made SyncManager a member
    void printHelp() {
        std::cout << "Available commands:" << std::endl;
        std::cout << "  upload <path/filename.ext> - Upload a file to the server" << std::endl;
        std::cout << "  list_server - List files stored on the server for your user" << std::endl;

        std::cout << "  download <filename> - Download a file from the server to current directory" << std::endl;
        std::cout << "  list_client - List files stored locally for your user" << std::endl;
        std::cout << "  delete - Delete a file to the server" << std::endl;
        std::cout << "  exit - Close the session" << std::endl;
    }

    void handleUpload(const std::string& filepath) {
        try {
            if (!this->syncManager.uploadFile(username, filepath)) { // Use member syncManager
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

    void handleDownload(const std::string& filename) {
        std::cout << "[CLIENT] Attempting to download: " << filename << std::endl;
        std::lock_guard<std::mutex> netLock(syncManager.getNetworkMutex());
        packet pkt;
        // 1. Send CMD_DOWNLOAD
        pkt.type = PACKET_TYPE_CMD;
        uint16_t cmd_code = CMD_DOWNLOAD;
        pkt.length = sizeof(cmd_code);
        memcpy(pkt.payload, &cmd_code, sizeof(cmd_code));
        if (!network.sendPacket(pkt)) {
            std::cerr << "[CLIENT] Failed to send download command" << std::endl;
            return;
        }
        
        // 2. Wait for ACK for CMD_DOWNLOAD
        if (!syncManager.waitForAck()) { // Use syncManager's public waitForAck
            std::cerr << "[CLIENT] No ACK for download command" << std::endl;
            return;
        }
        
        // 3. Send filename
        pkt.type = PACKET_TYPE_FILE; 
        pkt.length = filename.length();
        if (filename.length() >= MAX_PAYLOAD_SIZE) {
            std::cerr << "[CLIENT] Filename too long." << std::endl;
            return;
        }
        memcpy(pkt.payload, filename.c_str(), filename.length());
        if (!network.sendPacket(pkt)) {
            std::cerr << "[CLIENT] Failed to send filename for download" << std::endl;
            return;
        }
        
        // 4. Wait for ACK (file exists, server ready) or NACK (file not found)
        packet responsePkt;
        if (!network.receivePacket(responsePkt)) {
            std::cerr << "[CLIENT] Failed to receive server response for filename" << std::endl;
            return;
        }
        
        if (responsePkt.type == PACKET_TYPE_NACK) {
            std::cerr << "[CLIENT] Server: File '" << filename << "' not found or unable to download." << std::endl;
            return;
        } else if (responsePkt.type != PACKET_TYPE_ACK) {
            std::cerr << "[CLIENT] Unexpected response from server for filename. Type: " << (int)responsePkt.type << std::endl;
            return;
        }
        
        // 5. Server sent ACK, proceed to receive file
        std::cout << "[CLIENT] Server acknowledged. Starting file reception for: " << filename << std::endl;
        if (network.receiveFile(filename)) { // network.receiveFile saves it to 'filename' in CWD
            std::cout << "[CLIENT] File '" << filename << "' downloaded successfully to current directory." << std::endl;
        } else {
            std::cerr << "[CLIENT] Failed to download file '" << filename << "'." << std::endl;
            // fs::remove(filename); // Optional: clean up partial file
        }
    }
       
    
void handleListClient() {
    std::vector<file_metadata> files;
    std::string userDir = "sync_dir_" + username;

    DIR* dir = opendir(userDir.c_str());
    if (!dir) {
        std::cerr << "Failed to open directory: " << userDir << std::endl;
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name != "." && name != "..") {
            std::string fullPath = userDir + "/" + name;
            file_metadata meta = get_file_metadata(fullPath);
            files.push_back(meta);
        }
    }
    closedir(dir);

    std::cout << "Local files:\n";
    for (const auto& file : files) {
        std::cout << " - " << file.filename << " (" << file.size << " bytes)\n";
    }
}


    

    void handleExit() {
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
