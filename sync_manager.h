#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <map>
#include <mutex>
#include "network.h"
#include "file_manager.h"
#include <vector>
#include <condition_variable>
#include <queue>
#include <chrono>

class SyncManager {
public:
    SyncManager(NetworkManager& net, FileManager& fileMgr);
    ~SyncManager();

    // Core sync functions
    bool startSync(const std::string& username);
    void stopSync();
    bool uploadFile(const std::string& username, const std::string& filepath);

private:
    NetworkManager& network;
    FileManager& fileManager;
    std::string syncDir;
    std::thread syncThread;
    std::atomic<bool> running{false};
    std::atomic<bool> isUploading{false};
    
    // Synchronization primitives
    std::mutex fileMutex;                    // Protects file operations
    std::mutex networkMutex;                 // Protects network operations
    std::mutex uploadMutex;                  // Protects upload state
    std::condition_variable uploadCV;        // Signals upload completion
    std::condition_variable fileChangeCV;    // Signals file changes
    
    // File operation queue
    struct FileOperation {
        std::string filepath;
        bool isDelete;
        std::chrono::system_clock::time_point timestamp;
    };
    std::queue<FileOperation> fileOpQueue;
    std::mutex queueMutex;

    void syncLoop(const std::string& username);
    void handleFileChange(const std::string& username, const std::string& filepath, bool isDelete);
    void processFileOperation(const std::string& username, const FileOperation& op);
    bool waitForAck();  // Helper method to wait for ACK packets
}; 