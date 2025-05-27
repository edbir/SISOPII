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

bool FileManager::listFiles(const std::string& username, std::vector<file_metadata>& files) {
    std::cout << "[FileManager] Listing files for user: " << username << std::endl;

    std::lock_guard<std::mutex> lock(fileMutex);

    std::string userDir = baseDirectory + "/" + username;

    DIR* dir = opendir(userDir.c_str());
    if (!dir) {
        std::cerr << "[FileManager] Failed to open directory: " << userDir << std::endl;
        return false;
    }

    struct dirent* entry;
    files.clear();

    while ((entry = readdir(dir)) != nullptr) {
        // Ignore "." and ".."
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        std::string filePath = userDir + "/" + entry->d_name;

        struct stat st;
        if (stat(filePath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            file_metadata meta;
            meta.filename = entry->d_name;
            meta.size = st.st_size;
            meta.mtime = st.st_mtime;
            meta.atime = st.st_atime;
            meta.ctime = st.st_ctime;

            files.push_back(meta);
        }
    }

    closedir(dir);
    std::cout << "[FileManager] Found " << files.size() << " file(s) for user: " << username << std::endl;
    return true;
}
