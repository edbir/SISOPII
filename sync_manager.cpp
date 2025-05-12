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
    
    // Wait for any ongoing upload to complete
    std::unique_lock<std::mutex> uploadLock(uploadMutex);
    uploadCV.wait(uploadLock, [this]{ return !isUploading; });
    isUploading = true;
    uploadLock.unlock();

    try {
        // Copy file to sync directory if needed
        std::string filename = fs::path(filepath).filename().string();
        std::string syncPath = syncDir + "/" + filename;
        
        if (!fs::exists(syncPath)) {
            fs::copy_file(filepath, syncPath, fs::copy_options::overwrite_existing);
        }

        // Send upload command sequence:
        // 1. Send CMD_UPLOAD command
        // 2. Send filename
        // 3. Send file size
        // 4. Send file content in chunks
        std::lock_guard<std::mutex> netLock(networkMutex);
        packet pkt;
        
        // Step 1: Send upload command
        pkt.type = PACKET_TYPE_CMD;
        pkt.seqn = 0;
        pkt.total_size = 1;
        uint16_t cmd = CMD_UPLOAD;
        pkt.length = sizeof(cmd);
        memcpy(pkt.payload, &cmd, sizeof(cmd));
        
        if (!network.sendPacket(pkt) || !waitForAck()) {
            throw std::runtime_error("Failed to send upload command");
        }

        // Step 2: Send filename
        pkt.type = PACKET_TYPE_FILE;
        pkt.length = filename.length();
        memcpy(pkt.payload, filename.c_str(), filename.length());
        
        if (!network.sendPacket(pkt) || !waitForAck()) {
            throw std::runtime_error("Failed to send filename");
        }

        // Step 3: Read file and prepare for sending
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            throw std::runtime_error("Could not open file");
        }

        auto fileSize = fs::file_size(filepath);
        std::vector<char> buffer(fileSize);
        file.read(buffer.data(), fileSize);
        file.close();

        // Send file size
        pkt.type = PACKET_TYPE_FILE;
        pkt.length = sizeof(fileSize);
        memcpy(pkt.payload, &fileSize, sizeof(fileSize));
        
        if (!network.sendPacket(pkt) || !waitForAck()) {
            throw std::runtime_error("Failed to send file size");
        }

        // Step 4: Send file content in chunks
        size_t bytesSent = 0;
        while (bytesSent < fileSize) {
            size_t remaining = fileSize - bytesSent;
            size_t chunkSize = std::min(remaining, static_cast<size_t>(MAX_PAYLOAD_SIZE));
            
            pkt.type = PACKET_TYPE_DATA;
            pkt.seqn = bytesSent / MAX_PAYLOAD_SIZE;
            pkt.total_size = (fileSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
            pkt.length = chunkSize;
            memcpy(pkt.payload, buffer.data() + bytesSent, chunkSize);
            
            if (!network.sendPacket(pkt) || !waitForAck()) {
                throw std::runtime_error("Failed to send file chunk");
            }
            
            bytesSent += chunkSize;
        }

        isUploading = false;
        uploadCV.notify_one();
        return true;

    } catch (const std::exception& e) {
        std::cerr << "[UPLOAD] Error: " << e.what() << std::endl;
        isUploading = false;
        uploadCV.notify_one();
        return false;
    }
}

// Handles file change events from inotify
// If a file is being uploaded, queues the operation
// Otherwise, processes the change immediately
void SyncManager::handleFileChange(const std::string& username, const std::string& filepath, bool isDelete) {
   

    if (isUploading) {
        // Queue the operation if an upload is in progress
        FileOperation op{filepath, false, std::chrono::system_clock::now()};
        std::lock_guard<std::mutex> lock(queueMutex);
        fileOpQueue.push(op);
        fileChangeCV.notify_one();
        return;
    }

    uploadFile(username, filepath);
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
// Uses inotify to detect file modifications in real-time
void SyncManager::syncLoop(const std::string& username) {
    // Initialize inotify instance
    int inotifyFd = inotify_init();
    if (inotifyFd < 0) {
        std::cerr << "[SYNC] Error initializing inotify: " << strerror(errno) << std::endl;
        return;
    }

    // Set up watch for the sync directory
    // IN_CLOSE_WRITE: Triggered when a file is closed after being opened for writing
    // IN_DELETE: Triggered when a file is deleted
    // IN_MOVED_FROM: Triggered when a file is moved out of the watched directory
    // IN_MOVED_TO: Triggered when a file is moved into the watched directory
    int watchFd = inotify_add_watch(inotifyFd, syncDir.c_str(),
                                  IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (watchFd < 0) {
        std::cerr << "[SYNC] Error adding watch: " << strerror(errno) << std::endl;
        close(inotifyFd);
        return;
    }

    // Initialize last modified times for existing files
    std::map<std::string, std::filesystem::file_time_type> lastModified;
    for (const auto& entry : fs::directory_iterator(syncDir)) {
        if (entry.is_regular_file()) {
            lastModified[entry.path().string()] = fs::last_write_time(entry.path());
        }
    }

    // Main monitoring loop
    char buffer[4096];
    while (running) {
        // Set up select for non-blocking inotify read
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(inotifyFd, &readfds);
        
        struct timeval tv = {1, 0};  // 1 second timeout
        if (select(inotifyFd + 1, &readfds, NULL, NULL, &tv) > 0) {
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

        // Check for file modifications that might have been missed by inotify
        for (const auto& entry : fs::directory_iterator(syncDir)) {
            if (entry.is_regular_file()) {
                std::string filepath = entry.path().string();
                auto currentTime = fs::last_write_time(entry.path());
                
                // Handle new or modified files
                if (lastModified.find(filepath) == lastModified.end()) {
                    lastModified[filepath] = currentTime;
                    handleFileChange(username, filepath, false);
                }
                else if (lastModified[filepath] != currentTime) {
                    lastModified[filepath] = currentTime;
                    handleFileChange(username, filepath, false);
                }
            }
        }

        // Clean up entries for deleted files
        for (auto it = lastModified.begin(); it != lastModified.end();) {
            if (!fs::exists(it->first)) {
                handleFileChange(username, it->first, true);
                it = lastModified.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Cleanup inotify resources
    inotify_rm_watch(inotifyFd, watchFd);
    close(inotifyFd);
}

// Helper method to wait for and verify ACK packets
bool SyncManager::waitForAck() {
    packet ack;
    return network.receivePacket(ack) && ack.type == PACKET_TYPE_ACK;
} 
