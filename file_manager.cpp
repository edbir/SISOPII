#include "file_manager.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <cstring>
#include <iostream>

FileManager::FileManager(const std::string& baseDir) : baseDirectory(baseDir) {
    // Create base directory if it doesn't exist
    mkdir(baseDir.c_str(), 0755);
}

FileManager::~FileManager() {}

bool FileManager::createUserDir(const std::string& username) {
    std::cout << "[FileManager] Attempting to create user directory for: " << username << std::endl;
    
    {
        std::lock_guard<std::mutex> lock(fileMutex);
        std::cout << "[FileManager] Successfully acquired mutex lock for user: " << username << std::endl;
        
        std::string userDir = baseDirectory + "/" + username;
        
        // Check if directory already exists
        struct stat st;
        if (stat(userDir.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                std::cout << "[FileManager] Directory already exists for user: " << username << std::endl;
                return true;
            } else {
                std::cerr << "[FileManager] Path exists but is not a directory: " << userDir << std::endl;
                return false;
            }
        }
        
        // Create directory
        int result = mkdir(userDir.c_str(), 0755);
        if (result == 0) {
            std::cout << "[FileManager] Successfully created directory for user: " << username << std::endl;
            return true;
        } else {
            std::cerr << "[FileManager] Error creating directory for user " << username 
                      << ": " << strerror(errno) << " (errno: " << errno << ")" << std::endl;
            return false;
        }
    } // Mutex is automatically unlocked here when lock goes out of scope
    std::cout << "[FileManager] Released mutex lock for user: " << username << std::endl;
}

std::string FileManager::getUserDir(const std::string& username) {
    std::lock_guard<std::mutex> lock(fileMutex);
    std::string userDir = baseDirectory + "/" + username;
    std::cout << "[FileManager] Getting user directory: " << userDir << std::endl;
    return userDir;
} 