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
    bool deleteFile(const std::string& username, const std::string& filepath);
    bool waitForAck();  // Helper method to wait for ACK packets
    std::mutex& getNetworkMutex(); // Added getter

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
    
    // File hash tracking to prevent upload loops
    std::map<std::string, std::string> fileHashes;  // filename -> hash
    std::mutex hashMutex;
    
    // Hash calculation methods
    std::string calculateFileHash(const std::string& filepath);
    bool isFileHashChanged(const std::string& filename, const std::string& filepath);
    void updateFileHash(const std::string& filename, const std::string& filepath);

    void syncLoop(const std::string& username);
    void handleFileChange(const std::string& username, const std::string& filepath, bool isDelete);
    void processFileOperation(const std::string& username, const FileOperation& op);
    
}; 