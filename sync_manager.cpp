#include "sync_manager.h"
#include <sys/inotify.h>
#include <unistd.h>
#include <iostream>
#include <filesystem>
#include <chrono>
#include <map>
#include <fstream>
#include <vector>
#include <thread>

namespace fs = std::filesystem;

SyncManager::SyncManager(NetworkManager& net, FileManager& fileMgr)
    : network(net), fileManager(fileMgr), running(false), isUploading(false) {}

SyncManager::~SyncManager() {
    stopSync();
}

bool SyncManager::startSync(const std::string& username) {
    if (running) return false;

    // Create sync_dir if it doesn't exist
    std::string syncDir = "sync_dir_" + username;
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

bool SyncManager::uploadFile(const std::string& username, const std::string& filepath) {
    std::cout << "\n[UPLOAD] Starting upload process for: " << filepath << std::endl;
    
    // Lock for upload synchronization
    std::unique_lock<std::mutex> uploadLock(uploadMutex);
    uploadCV.wait(uploadLock, [this]{ return !isUploading; });
    isUploading = true;
    uploadLock.unlock();
    
    std::cout << "[UPLOAD] Upload lock acquired" << std::endl;

    try {
        std::cout << "[UPLOAD] Starting upload for file: " << filepath << std::endl;

        // Lock file operations
        std::lock_guard<std::mutex> fileLock(fileMutex);
        
        // First, copy the file to sync_dir if it doesn't exist
        std::string filename = fs::path(filepath).filename().string();
        std::string syncDir = "sync_dir_" + username;
        std::string syncPath = syncDir + "/" + filename;
        
        std::cout << "[UPLOAD] Checking if file exists in sync directory: " << syncPath << std::endl;
        
        // Only copy if the file doesn't exist in sync directory
        if (!fs::exists(syncPath)) {
            try {
                std::cout << "[UPLOAD] Copying file to sync directory: " << syncPath << std::endl;
                fs::copy_file(filepath, syncPath, fs::copy_options::overwrite_existing);
                std::cout << "[UPLOAD] File copied successfully" << std::endl;
            } catch (const fs::filesystem_error& e) {
                std::cerr << "[UPLOAD] Error copying file to sync directory: " << e.what() << std::endl;
                isUploading = false;
                uploadLock.lock();
                uploadLock.unlock();
                uploadCV.notify_one();
                return false;
            }
        } else {
            std::cout << "[UPLOAD] File already exists in sync directory" << std::endl;
        }

        // Lock network operations
        std::lock_guard<std::mutex> netLock(networkMutex);

        // Send CMD_UPLOAD command
        packet pkt;
        pkt.type = PACKET_TYPE_CMD;
        pkt.seqn = 0;
        pkt.total_size = 1;
        uint16_t cmd = CMD_UPLOAD;
        pkt.length = sizeof(cmd);
        memcpy(pkt.payload, &cmd, sizeof(cmd));

        std::cout << "[UPLOAD] Sending CMD_UPLOAD command" << std::endl;
        if (!network.sendPacket(pkt)) {
            std::cerr << "[UPLOAD] Failed to send command packet" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        // Wait for ACK
        packet ack;
        if (!network.receivePacket(ack) || ack.type != PACKET_TYPE_ACK) {
            std::cerr << "[UPLOAD] Failed to receive ACK for CMD_UPLOAD" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        // Send filename as FILE packet
        pkt.type = PACKET_TYPE_FILE;
        pkt.seqn = 0;
        pkt.total_size = 1;
        pkt.length = filename.length();
        memcpy(pkt.payload, filename.c_str(), filename.length());
        
        std::cout << "[UPLOAD] Sending filename: " << filename << std::endl;
        if (!network.sendPacket(pkt)) {
            std::cerr << "[UPLOAD] Failed to send filename packet" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        // Wait for ACK
        if (!network.receivePacket(ack) || ack.type != PACKET_TYPE_ACK) {
            std::cerr << "[UPLOAD] Failed to receive ACK for filename" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        // Verify file exists and is readable
        if (!fs::exists(filepath)) {
            std::cerr << "[UPLOAD] Error: File does not exist: " << filepath << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        // Read file size
        std::error_code ec;
        auto fileSize = fs::file_size(filepath, ec);
        if (ec) {
            std::cerr << "[UPLOAD] Error getting file size: " << ec.message() << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        if (fileSize == 0) {
            std::cerr << "[UPLOAD] Error: File is empty" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        std::cout << "[UPLOAD] Reading file content (size: " << fileSize << " bytes)" << std::endl;
        
        // Open file with exclusive access
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[UPLOAD] Error: Could not open file for reading" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        // Read file content
        std::vector<char> buffer(fileSize);
        file.read(buffer.data(), fileSize);
        if (file.gcount() != fileSize) {
            std::cerr << "[UPLOAD] Error: Could not read entire file (read " << file.gcount() << " of " << fileSize << " bytes)" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }
        file.close();
        std::cout << "[UPLOAD] File content read successfully" << std::endl;

        // Send file size as FILE packet
        pkt.type = PACKET_TYPE_FILE;
        pkt.seqn = 0;
        pkt.total_size = 1;
        pkt.length = sizeof(fileSize);
        memcpy(pkt.payload, &fileSize, sizeof(fileSize));
        
        std::cout << "[UPLOAD] Sending file size: " << fileSize << " bytes" << std::endl;
        if (!network.sendPacket(pkt)) {
            std::cerr << "[UPLOAD] Failed to send file size" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }

        // Wait for ACK
        if (!network.receivePacket(ack) || ack.type != PACKET_TYPE_ACK) {
            std::cerr << "[UPLOAD] Failed to receive ACK for file size" << std::endl;
            isUploading = false;
            uploadLock.lock();
            uploadLock.unlock();
            uploadCV.notify_one();
            return false;
        }
        std::cout << "[UPLOAD] Received ACK for file size" << std::endl;

        // Send file content in chunks
        size_t bytesSent = 0;
        while (bytesSent < fileSize) {
            size_t remaining = fileSize - bytesSent;
            size_t chunkSize = std::min(remaining, static_cast<size_t>(MAX_PAYLOAD_SIZE));
            
            pkt.type = PACKET_TYPE_DATA;
            pkt.seqn = bytesSent / MAX_PAYLOAD_SIZE;
            pkt.total_size = (fileSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE;
            pkt.length = chunkSize;
            memcpy(pkt.payload, buffer.data() + bytesSent, chunkSize);
            
            std::cout << "[UPLOAD] Sending chunk " << (bytesSent / MAX_PAYLOAD_SIZE + 1) 
                      << " of " << pkt.total_size << " (size: " << chunkSize << " bytes)" << std::endl;
            
            if (!network.sendPacket(pkt)) {
                std::cerr << "[UPLOAD] Failed to send file chunk" << std::endl;
                isUploading = false;
                uploadLock.lock();
                uploadLock.unlock();
                uploadCV.notify_one();
                return false;
            }

            // Wait for ACK
            if (!network.receivePacket(ack) || ack.type != PACKET_TYPE_ACK) {
                std::cerr << "[UPLOAD] Failed to receive ACK for chunk" << std::endl;
                isUploading = false;
                uploadLock.lock();
                uploadLock.unlock();
                uploadCV.notify_one();
                return false;
            }
            std::cout << "[UPLOAD] Received ACK for chunk" << std::endl;
            
            bytesSent += chunkSize;
        }

        std::cout << "[UPLOAD] File upload completed successfully" << std::endl;
        isUploading = false;
        uploadLock.lock();
        uploadLock.unlock();
        uploadCV.notify_one();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[UPLOAD] Exception during upload: " << e.what() << std::endl;
        isUploading = false;
        uploadLock.lock();
        uploadLock.unlock();
        uploadCV.notify_one();
        return false;
    } catch (...) {
        std::cerr << "[UPLOAD] Unknown exception during upload" << std::endl;
        isUploading = false;
        uploadLock.lock();
        uploadLock.unlock();
        uploadCV.notify_one();
        return false;
    }
}

bool SyncManager::downloadFile(const std::string& username, const std::string& filename) {
    packet pkt;
    pkt.type = PACKET_TYPE_CMD;
    pkt.seqn = 0;
    pkt.total_size = 1;
    uint16_t cmd = CMD_DOWNLOAD;
    pkt.length = sizeof(cmd);
    memcpy(pkt.payload, &cmd, sizeof(cmd));

    if (!network.sendPacket(pkt)) {
        return false;
    }

    // Send filename
    pkt.length = filename.length();
    memcpy(pkt.payload, filename.c_str(), filename.length());
    
    if (!network.sendPacket(pkt)) {
        return false;
    }

    // Receive file content
    return network.receiveFile(filename);
}

bool SyncManager::deleteFile(const std::string& username, const std::string& filename) {
    packet pkt;
    pkt.type = PACKET_TYPE_CMD;
    pkt.seqn = 0;
    pkt.total_size = 1;
    uint16_t cmd = CMD_DELETE;
    pkt.length = sizeof(cmd);
    memcpy(pkt.payload, &cmd, sizeof(cmd));

    if (!network.sendPacket(pkt)) {
        return false;
    }

    // Send filename
    pkt.length = filename.length();
    memcpy(pkt.payload, filename.c_str(), filename.length());
    
    return network.sendPacket(pkt);
}

bool SyncManager::listServerFiles(const std::string& username, std::vector<file_metadata>& files) {
    packet pkt;
    pkt.type = PACKET_TYPE_CMD;
    pkt.seqn = 0;
    pkt.total_size = 1;
    uint16_t cmd = CMD_LIST_SERVER;
    pkt.length = sizeof(cmd);
    memcpy(pkt.payload, &cmd, sizeof(cmd));

    if (!network.sendPacket(pkt)) {
        return false;
    }

    // Receive file list
    if (!network.receivePacket(pkt)) {
        return false;
    }

    // Parse file list from payload
    size_t offset = 0;
    while (offset < pkt.length) {
        file_metadata meta;
        memcpy(&meta, pkt.payload + offset, sizeof(file_metadata));
        files.push_back(meta);
        offset += sizeof(file_metadata);
    }

    return true;
}

bool SyncManager::listClientFiles(const std::string& username, std::vector<file_metadata>& files) {
    std::string syncDir = "sync_dir_" + username;
    for (const auto& entry : fs::directory_iterator(syncDir)) {
        if (entry.is_regular_file()) {
            file_metadata meta = get_file_metadata(entry.path().string());
            files.push_back(meta);
        }
    }
    return true;
}

void SyncManager::handleClientConnection(const std::string& username) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    userSessions[username].push_back(network.getClientSocket());
}

void SyncManager::broadcastFileChange(const std::string& username, const std::string& filename, bool isDelete) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    auto it = userSessions.find(username);
    if (it != userSessions.end()) {
        for (int socket : it->second) {
            packet pkt;
            pkt.type = PACKET_TYPE_CMD;
            pkt.seqn = 0;
            pkt.total_size = 1;
            uint16_t cmd = isDelete ? CMD_DELETE : CMD_UPLOAD;
            pkt.length = sizeof(cmd);
            memcpy(pkt.payload, &cmd, sizeof(cmd));

            // Send filename
            pkt.length = filename.length();
            memcpy(pkt.payload, filename.c_str(), filename.length());
            
            send(socket, &pkt, sizeof(packet), 0);
        }
    }
}

void SyncManager::handleFileChange(const std::string& username, const std::string& filepath, bool isDelete) {
    std::cout << "[HANDLE] Starting file change handler" << std::endl;
    std::cout << "  - Filepath: " << filepath << std::endl;
    std::cout << "  - Is delete: " << (isDelete ? "yes" : "no") << std::endl;
    std::cout << "  - Is uploading: " << (isUploading ? "yes" : "no") << std::endl;

    // If already uploading, queue the operation
    if (isUploading) {
        std::cout << "[HANDLE] Queueing operation - upload in progress" << std::endl;
        FileOperation op{filepath, isDelete, std::chrono::system_clock::now()};
        std::lock_guard<std::mutex> lock(queueMutex);
        fileOpQueue.push(op);
        fileChangeCV.notify_one();
        return;
    }

    std::string filename = filepath.substr(filepath.find_last_of("/") + 1);
    std::cout << "[PROCESS] Processing file operation" << std::endl;
    std::cout << "  - Filepath: " << filepath << std::endl;
    std::cout << "  - Is delete: " << (isDelete ? "yes" : "no") << std::endl;

    if (isDelete) {
        deleteFile(username, filename);
    } else {
        // For modifications, check if file content has actually changed
        std::error_code ec;
        auto currentSize = fs::file_size(filepath, ec);
        if (ec) {
            std::cerr << "[HANDLE] Error getting file size: " << ec.message() << std::endl;
            return;
        }

        // Read file content
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "[HANDLE] Error: Could not open file for reading" << std::endl;
            return;
        }

        std::vector<char> buffer(currentSize);
        file.read(buffer.data(), currentSize);
        if (file.gcount() != currentSize) {
            std::cerr << "[HANDLE] Error: Could not read entire file" << std::endl;
            return;
        }
        file.close();

        // Calculate a simple hash of the content
        size_t contentHash = 0;
        for (size_t i = 0; i < currentSize; i++) {
            contentHash = contentHash * 31 + buffer[i];
        }

        // Check if this file was recently processed
        static std::map<std::string, std::pair<size_t, std::chrono::system_clock::time_point>> lastProcessed;
        auto now = std::chrono::system_clock::now();
        
        auto it = lastProcessed.find(filepath);
        if (it != lastProcessed.end()) {
            // If file was processed in the last second and content hasn't changed, skip
            if (now - it->second.second < std::chrono::seconds(1) && it->second.first == contentHash) {
                std::cout << "[HANDLE] Skipping duplicate file change" << std::endl;
                return;
            }
        }

        // Update last processed info
        lastProcessed[filepath] = {contentHash, now};

        std::cout << "[HANDLE] File content changed, uploading..." << std::endl;
        uploadFile(username, filepath);
    }
}

void SyncManager::processFileOperation(const std::string& username, const FileOperation& op) {
    std::cout << "\n[PROCESS] Processing file operation" << std::endl;
    std::cout << "  - Filepath: " << op.filepath << std::endl;
    std::cout << "  - Is delete: " << (op.isDelete ? "yes" : "no") << std::endl;
    
    if (op.isDelete) {
        std::cout << "[PROCESS] Deleting file from server: " << op.filepath << std::endl;
        deleteFile(username, fs::path(op.filepath).filename().string());
    } else {
        // Add a small delay to ensure file is completely written
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Retry logic for file reading
        const int MAX_RETRIES = 3;
        const int RETRY_DELAY_MS = 100;
        bool success = false;
        std::error_code ec;
        size_t fileSize = 0;

        for (int retry = 0; retry < MAX_RETRIES && !success; retry++) {
            if (retry > 0) {
                std::cout << "[PROCESS] Retry " << retry << " reading file..." << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(RETRY_DELAY_MS));
            }

            // Lock file operations
            std::lock_guard<std::mutex> fileLock(fileMutex);

            // Verify file exists and is readable
            if (!fs::exists(op.filepath)) {
                std::cerr << "[PROCESS] Error: File does not exist: " << op.filepath << std::endl;
                continue;
            }

            // Read file size
            fileSize = fs::file_size(op.filepath, ec);
            if (ec) {
                std::cerr << "[PROCESS] Error getting file size: " << ec.message() << std::endl;
                continue;
            }

            if (fileSize == 0) {
                std::cerr << "[PROCESS] Error: File is empty (attempt " << (retry + 1) << ")" << std::endl;
                continue;
            }

            // Try to open and read the file
            std::ifstream file(op.filepath);
            if (!file.is_open()) {
                std::cerr << "[PROCESS] Error: Could not open file for reading" << std::endl;
                continue;
            }

            // Read first few bytes to verify file is readable
            char buffer[32];
            file.read(buffer, sizeof(buffer));
            if (file.gcount() > 0) {
                success = true;
            }
            file.close();
        }

        if (!success) {
            std::cerr << "[PROCESS] Failed to read file after " << MAX_RETRIES << " attempts" << std::endl;
            return;
        }

        std::cout << "[PROCESS] File details:" << std::endl;
        std::cout << "  - Size: " << fileSize << " bytes" << std::endl;
        std::cout << "  - Last modified: " << fs::last_write_time(op.filepath).time_since_epoch().count() << std::endl;
        
        // Read and log file content
        std::ifstream file(op.filepath);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
            std::cout << "  - Content: " << content << std::endl;
            file.close();
        }
        
        std::cout << "[PROCESS] Starting file upload to server: " << op.filepath << std::endl;
        
        if (uploadFile(username, op.filepath)) {
            std::cout << "[PROCESS] File uploaded successfully" << std::endl;
        } else {
            std::cerr << "[PROCESS] Failed to upload file" << std::endl;
        }
    }
    std::cout << "[PROCESS] File operation completed" << std::endl;
}

void SyncManager::syncLoop(const std::string& username) {
    std::string syncDir = "sync_dir_" + username;
    std::cout << "[SYNC] Initializing sync loop for directory: " << syncDir << std::endl;
    
    // Initialize inotify
    int inotifyFd = inotify_init();
    if (inotifyFd < 0) {
        std::cerr << "[SYNC] Error initializing inotify: " << strerror(errno) << std::endl;
        return;
    }
    std::cout << "[SYNC] inotify initialized successfully" << std::endl;

    // Add watch for the directory - only watch for CLOSE_WRITE and DELETE events
    int watchFd = inotify_add_watch(inotifyFd, syncDir.c_str(),
                                  IN_CLOSE_WRITE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (watchFd < 0) {
        std::cerr << "[SYNC] Error adding watch: " << strerror(errno) << std::endl;
        close(inotifyFd);
        return;
    }
    std::cout << "[SYNC] Watch added successfully with fd: " << watchFd << std::endl;

    // Map to store last modification times
    std::map<std::string, std::filesystem::file_time_type> lastModified;

    // Initialize lastModified times for existing files
    for (const auto& entry : fs::directory_iterator(syncDir)) {
        if (entry.is_regular_file()) {
            lastModified[entry.path().string()] = fs::last_write_time(entry.path());
            std::cout << "[SYNC] Initial file: " << entry.path().string() 
                      << " last modified: " << fs::last_write_time(entry.path()).time_since_epoch().count() << std::endl;
        }
    }

    std::cout << "[SYNC] Started monitoring directory: " << syncDir << std::endl;

    char buffer[4096];
    while (running) {
        // Check for inotify events with a timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(inotifyFd, &readfds);
        
        struct timeval tv;
        tv.tv_sec = 1;  // 1 second timeout
        tv.tv_usec = 0;

        int ret = select(inotifyFd + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            std::cerr << "[SYNC] Error in select: " << strerror(errno) << std::endl;
            break;
        }
        
        if (ret > 0 && FD_ISSET(inotifyFd, &readfds)) {
            // Handle inotify events
            int length = read(inotifyFd, buffer, sizeof(buffer));
            if (length < 0) {
                std::cerr << "[SYNC] Error reading inotify events: " << strerror(errno) << std::endl;
                break;
            }

            int i = 0;
            while (i < length) {
                struct inotify_event* event = (struct inotify_event*)&buffer[i];
                if (event->len) {
                    std::string filename(event->name);
                    std::string filepath = syncDir + "/" + filename;

                    std::cout << "\n[SYNC] Inotify event detected:" << std::endl;
                    std::cout << "  - Filename: " << filename << std::endl;
                    std::cout << "  - Full path: " << filepath << std::endl;
                    std::cout << "  - Event mask: " << event->mask << std::endl;
                    
                    // Only handle CLOSE_WRITE and DELETE events
                    if (event->mask & IN_CLOSE_WRITE) {
                        std::cout << "[SYNC] File written (inotify): " << filename << std::endl;
                        // Add a small delay to ensure file is fully written
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        handleFileChange(username, filepath, false);
                    }
                    else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
                        std::cout << "[SYNC] File deleted (inotify): " << filename << std::endl;
                        handleFileChange(username, filepath, true);
                    }
                }
                i += sizeof(struct inotify_event) + event->len;
            }
        }

        // Process queued file operations
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

        // Periodic check for file modifications
        for (const auto& entry : fs::directory_iterator(syncDir)) {
            if (entry.is_regular_file()) {
                std::string filepath = entry.path().string();
                auto currentTime = fs::last_write_time(entry.path());
                
                // Check if file is new or modified
                if (lastModified.find(filepath) == lastModified.end()) {
                    std::cout << "\n[SYNC] New file detected: " << filepath << std::endl;
                    lastModified[filepath] = currentTime;
                    handleFileChange(username, filepath, false);
                }
                else if (lastModified[filepath] != currentTime) {
                    std::cout << "\n[SYNC] File modification detected (timestamp):" << std::endl;
                    std::cout << "  - File: " << filepath << std::endl;
                    std::cout << "  - Old timestamp: " << lastModified[filepath].time_since_epoch().count() << std::endl;
                    std::cout << "  - New timestamp: " << currentTime.time_since_epoch().count() << std::endl;
                    
                    lastModified[filepath] = currentTime;
                    handleFileChange(username, filepath, false);
                }
            }
        }

        // Remove entries for deleted files
        for (auto it = lastModified.begin(); it != lastModified.end();) {
            if (!fs::exists(it->first)) {
                std::cout << "\n[SYNC] File deleted (timestamp): " << it->first << std::endl;
                handleFileChange(username, it->first, true);
                it = lastModified.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::cout << "[SYNC] Stopping file monitoring" << std::endl;
    inotify_rm_watch(inotifyFd, watchFd);
    close(inotifyFd);
    std::cout << "[SYNC] File monitoring stopped" << std::endl;
} 
