#include "sync_manager.h"
#include <sys/inotify.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include <cstring>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <fstream>
#include <vector>
#include <thread>

namespace fs = std::filesystem;

// Constructor initializes the sync manager with network and file manager references
SyncManager::SyncManager(NetworkManager& net, FileManager& fileMgr)
    : network(net), fileManager(fileMgr), running(false), isUploading(false) {}

SyncManager::~SyncManager() {
    stopSync();
}

// Starts the synchronization process for a given user
// Creates a user-specific sync directory and starts the monitoring thread
bool SyncManager::startSync(const std::string& username) {
    if (running) return false;

    syncDir = "sync_dir_" + username;

    if (!fs::exists(syncDir)) {
        fs::create_directory(syncDir);
    }

    running = true;
    syncThread = std::thread(&SyncManager::syncLoop, this, username);
    return true;
}

void SyncManager::stopSync() {
    running = false;
    if (syncThread.joinable()) {
        syncThread.join();
    }
}

// Handles file upload process with proper synchronization
// Uses mutex and condition variables to prevent concurrent uploads
bool SyncManager::uploadFile(const std::string& username, const std::string& filepath) {
    std::cout << "[UPLOAD] Starting upload for: " << filepath << std::endl;
    
    std::unique_lock<std::mutex> uploadLock(uploadMutex);
    uploadCV.wait(uploadLock, [this]{ return !isUploading; });
    isUploading = true;
    uploadLock.unlock();

    try {
        // 1. First copy the file to sync_dir if it's not already there
        std::string filename = fs::path(filepath).filename().string();
        std::string syncPath = syncDir + "/" + filename;
        
        // Only copy if the source is not already in sync_dir
        if (filepath.find(syncDir) == std::string::npos) {
            std::cout << "[UPLOAD] Copying file to sync_dir: " << syncPath << std::endl;
            fs::copy_file(filepath, syncPath, fs::copy_options::overwrite_existing);
        }

        // 2. Now upload to server
        std::ifstream file(syncPath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file in sync_dir");
        }

        // Rest of the upload code remains the same...
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(fileSize);
        file.read(buffer.data(), fileSize);
        file.close();

        std::lock_guard<std::mutex> netLock(networkMutex);
        packet pkt;
        
        // Send upload command
        pkt.type = PACKET_TYPE_CMD;
        uint16_t cmd = CMD_UPLOAD;
        pkt.length = sizeof(cmd);
        memcpy(pkt.payload, &cmd, sizeof(cmd));
        
        if (!network.sendPacket(pkt)) {
            throw std::runtime_error("Failed to send upload command");
        }
        
        if (!waitForAck()) {
            throw std::runtime_error("No ACK for upload command");
        }

        // Send filename
        pkt.type = PACKET_TYPE_FILE;
        pkt.length = filename.length();
        memcpy(pkt.payload, filename.c_str(), filename.length());
        
        if (!network.sendPacket(pkt)) {
            throw std::runtime_error("Failed to send filename");
        }
        
        if (!waitForAck()) {
            throw std::runtime_error("No ACK for filename");
        }

        // Send file size
        pkt.type = PACKET_TYPE_FILE;
        pkt.length = sizeof(fileSize);
        memcpy(pkt.payload, &fileSize, sizeof(fileSize));
        
        if (!network.sendPacket(pkt)) {
            throw std::runtime_error("Failed to send file size");
        }
        
        if (!waitForAck()) {
            throw std::runtime_error("No ACK for file size");
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
            
            if (!network.sendPacket(pkt)) {
                throw std::runtime_error("Failed to send file chunk");
            }
            
            if (!waitForAck()) {
                throw std::runtime_error("No ACK for file chunk");
            }
            
            bytesSent += chunkSize;
        }

        isUploading = false;
        uploadCV.notify_one();
        std::cout << "[UPLOAD] Successfully uploaded: " << filename << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[UPLOAD] Error: " << e.what() << std::endl;
        isUploading = false;
        uploadCV.notify_one();
        return false;
    }
}



// Helper method to wait for ACK packets with timeout
bool SyncManager::waitForAck() {
    packet ack;
    struct timeval tv;
    tv.tv_sec = 5;  // 5 second timeout
    tv.tv_usec = 0;
    
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(network.getSocket(), &readfds);
    
    if (select(network.getSocket() + 1, &readfds, NULL, NULL, &tv) <= 0) {
        std::cerr << "[UPLOAD] Timeout waiting for ACK" << std::endl;
        return false;
    }
    
    return network.receivePacket(ack) && ack.type == PACKET_TYPE_ACK;
}

std::mutex& SyncManager::getNetworkMutex() {
    return networkMutex;
}

// Handles file change events from inotify
// If a file is being uploaded, queues the operation
// Otherwise, processes the change immediately

void SyncManager::handleFileChange(const std::string& username, const std::string& filepath, bool isDelete) {
    std::cout << "[SYNC] File change detected: " << filepath << std::endl;
    
    // Check if the change is in our sync directory
    bool isInSyncDir = (filepath.find(syncDir) == 0);

    if (isInSyncDir) {
        // This is a change in our sync directory - we need to upload it
        std::cout << "[SYNC] Detected change in sync directory, uploading: " << filepath << std::endl;
        uploadFile(username, filepath);
    } else {
        // This is a change outside sync directory - ignore or handle differently
        std::cout << "[SYNC] Ignoring external file change: " << filepath << std::endl;
    }
}

// Processes a queued file operation
// Adds a small delay to ensure file system stability
void SyncManager::processFileOperation(const std::string& username, const FileOperation& op) {
    if (op.isDelete) {
        std::cout << "[PROCESS] Skipping delete operation" << std::endl;
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uploadFile(username, op.filepath);
}

// Main synchronization loop that monitors file system changes
// Uses inotify to detect file modifications 
void SyncManager::syncLoop(const std::string& username) {
    // Initialize inotify instance
    int inotifyFd = inotify_init();
    if (inotifyFd < 0) {
        std::cerr << "[SYNC] Error initializing inotify: " << strerror(errno) << std::endl;
        return;
    }

    // Set up watch for the sync directory
    int watchFd = inotify_add_watch(inotifyFd, syncDir.c_str(),
                                  IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (watchFd < 0) {
        std::cerr << "[SYNC] Error adding watch: " << strerror(errno) << std::endl;
        close(inotifyFd);
        return;
    }

    // Main monitoring loop
    char buffer[4096];
    while (running) {
        // Set up select for non-blocking inotify read and network
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(inotifyFd, &readfds);
        FD_SET(network.getSocket(), &readfds);
        
        struct timeval tv = {1, 0};  // 1 second timeout
        if (select(std::max(inotifyFd, network.getSocket()) + 1, &readfds, NULL, NULL, &tv) > 0) {
            // Check for network messages first
            if (FD_ISSET(network.getSocket(), &readfds)) {
                std::lock_guard<std::mutex> lock(networkMutex);
                packet pkt;
                if (network.receivePacket(pkt)) {
                    if (pkt.type == PACKET_TYPE_CMD) {
                        uint16_t cmd;
                        memcpy(&cmd, pkt.payload, sizeof(cmd));
                        
                        // In the syncLoop function, modify the CMD_FILE_CHANGED case:
                        if (cmd == CMD_FILE_CHANGED) {
                            std::cout << "[SYNC] Received file change notification" << std::endl;
                            
                            // Send ACK for command
                            pkt.type = PACKET_TYPE_ACK;
                            pkt.length = 0;
                            if (!network.sendPacket(pkt)) {
                                std::cerr << "[SYNC] Failed to send command ACK" << std::endl;
                                continue;
                            }
                            
                            // Receive filename
                            if (!network.receivePacket(pkt) || pkt.type != PACKET_TYPE_FILE) {
                                std::cerr << "[SYNC] Expected filename packet" << std::endl;
                                continue;
                            }
                            std::string filename(pkt.payload, pkt.length);
                            std::string filepath = syncDir + "/" + filename;
                            
                            // Send ACK for filename
                            pkt.type = PACKET_TYPE_ACK;
                            pkt.length = 0;
                            if (!network.sendPacket(pkt)) {
                                std::cerr << "[SYNC] Failed to send filename ACK" << std::endl;
                                continue;
                            }

                            // Receive file size
                            if (!network.receivePacket(pkt) || pkt.type != PACKET_TYPE_FILE) {
                                std::cerr << "[SYNC] Expected file size packet" << std::endl;
                                continue;
                            }
                            size_t fileSize;
                            memcpy(&fileSize, pkt.payload, sizeof(fileSize));
                            
                            // Send ACK for file size
                            pkt.type = PACKET_TYPE_ACK;
                            pkt.length = 0;
                            if (!network.sendPacket(pkt)) {
                                std::cerr << "[SYNC] Failed to send file size ACK" << std::endl;
                                continue;
                            }

                            // Create/truncate file in sync_dir
                            std::ofstream file(filepath, std::ios::binary | std::ios::trunc);
                            if (!file.is_open()) {
                                std::cerr << "[SYNC] Failed to create file: " << filepath << std::endl;
                                continue;
                            }

                            // Receive and write file contents
                            size_t bytesReceived = 0;
                            while (bytesReceived < fileSize) {
                                if (!network.receivePacket(pkt) || pkt.type != PACKET_TYPE_DATA) {
                                    std::cerr << "[SYNC] Expected data packet" << std::endl;
                                    break;
                                }

                                file.write(pkt.payload, pkt.length);
                                bytesReceived += pkt.length;
                            }

                            file.close();
                            
                            if (bytesReceived == fileSize) {
                                std::cout << "[SYNC] Successfully updated file: " << filename << std::endl;
                            } else {
                                std::cerr << "[SYNC] File transfer incomplete, removing: " << filename << std::endl;
                                fs::remove(filepath);
                            }
                        }
                    }
                }
            }

            // Check for inotify events
            if (FD_ISSET(inotifyFd, &readfds)) {
                // Read inotify events
                int length = read(inotifyFd, buffer, sizeof(buffer));
                if (length < 0) break;

                // Process each event
                int i = 0;
                while (i < length) {
                    struct inotify_event* event = (struct inotify_event*)&buffer[i];
                    if (event->len) {
                        std::string filename(event->name);
                        std::string filepath = syncDir + "/" + filename;
                        
                        // Handle different types of events
                        if (event->mask & IN_CLOSE_WRITE) {
                            // File was modified and closed
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            handleFileChange(username, filepath, false);
                        }
                        else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                            // File was deleted or moved out
                            handleFileChange(username, filepath, true);
                        }
                    }
                    i += sizeof(struct inotify_event) + event->len;
                }
            }
        }

        // Process any queued file operations
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            while (!fileOpQueue.empty()) {
                FileOperation op = fileOpQueue.front();
                fileOpQueue.pop();
                lock.unlock();
                processFileOperation(username, op);
                lock.lock();
            }
        }
    }

    // Cleanup inotify resources
    inotify_rm_watch(inotifyFd, watchFd);
    close(inotifyFd);
}


bool SyncManager::deleteFile(const std::string& username, const std::string& filepath) {
    std::cout << "[CLIENT][DELETE] Starting deletion for: " << filepath << std::endl;
    std::cout << "[CLIENT][DEBUG] Thread ID: " << std::this_thread::get_id() << std::endl;
    
    // Wait for any ongoing operations
    std::cout << "[CLIENT][SYNC] Checking for active operations..." << std::endl;
    std::unique_lock<std::mutex> uploadLock(uploadMutex);
    uploadCV.wait(uploadLock, [this]{ 
        std::cout << "[CLIENT][SYNC] isUploading=" << isUploading << std::endl;
        return !isUploading; 
    });
    isUploading = true;
    uploadLock.unlock();
    std::cout << "[CLIENT][SYNC] Operation lock acquired" << std::endl;

    try {
        std::string filename = fs::path(filepath).filename().string();
        std::string syncPath = syncDir + "/" + filename;
        std::cout << "[CLIENT][DEBUG] Full sync path: " << syncPath << std::endl;

        // Verify local file
        std::cout << "[CLIENT][FS] Checking local file existence..." << std::endl;
        if (!fs::exists(syncPath)) {
            std::cerr << "[CLIENT][ERROR] Local file not found in sync_dir" << std::endl;
            throw std::runtime_error("Local file not found");
        }

        // Network protocol
        std::cout << "[CLIENT][NET] Acquiring network lock..." << std::endl;
        std::lock_guard<std::mutex> netLock(networkMutex);
        
        packet pkt;
        
        // Step 1: Send DELETE command
        std::cout << "[CLIENT][NET] Sending DELETE command..." << std::endl;
        pkt.type = PACKET_TYPE_CMD;
        uint16_t cmd = CMD_DELETE;
        pkt.length = sizeof(cmd);
        memcpy(pkt.payload, &cmd, sizeof(cmd));
        
        if (!network.sendPacket(pkt)) {
            std::cerr << "[CLIENT][ERROR] Failed to send command packet" << std::endl;
            throw std::runtime_error("Network send failed");
        }

        // 2. Immediately send filename (NO ACK IN BETWEEN)
        std::cout << "[CLIENT][NET] Immediately sending filename: " << filename << "\n";
        pkt.type = PACKET_TYPE_FILE;
        pkt.length = filename.length();
        memcpy(pkt.payload, filename.c_str(), filename.length());
        if (!network.sendPacket(pkt)) throw std::runtime_error("Filename send failed");

        std::cout << "[CLIENT][NET] Waiting for command ACK..." << std::endl;
        if (!waitForAck()) throw std::runtime_error("No ACK for command");

        // Step 3: Delete local file
        std::cout << "[CLIENT][FS] Attempting local deletion..." << std::endl;
        if (!fs::remove(syncPath)) {
            std::cerr << "[CLIENT][ERROR] fs::remove() failed (errno: " << errno << ")" << std::endl;
            throw std::runtime_error("Local deletion failed");
        }
        std::cout << "[CLIENT][FS] Local deletion successful" << std::endl;

        // Step 4: Wait for server confirmation
        std::cout << "[CLIENT][NET] Waiting for final confirmation..." << std::endl;
        if (!waitForAck()) throw std::runtime_error("Server confirmation failed");

        isUploading = false;
        uploadCV.notify_one();
        std::cout << "[CLIENT][DELETE] Complete success for: " << syncPath << std::endl;
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[CLIENT][ERROR] Exception: " << e.what() << std::endl;
        isUploading = false;
        uploadCV.notify_one();
        return false;
    }
}
