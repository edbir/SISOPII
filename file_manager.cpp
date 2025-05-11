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
    std::lock_guard<std::mutex> lock(fileMutex);
    std::string userDir = baseDirectory + "/" + username;
    return mkdir(userDir.c_str(), 0755) == 0 || errno == EEXIST;
}

bool FileManager::deleteFile(const std::string& username, const std::string& filename) {
    std::lock_guard<std::mutex> lock(fileMutex);
    std::string filepath = getUserDir(username) + "/" + filename;
    return remove(filepath.c_str()) == 0;
}

bool FileManager::listFiles(const std::string& username, std::vector<file_metadata>& files) {
    std::lock_guard<std::mutex> lock(fileMutex);
    std::string userDir = getUserDir(username);
    DIR* dir = opendir(userDir.c_str());
    
    if (!dir) {
        std::cerr << "Error opening directory: " << userDir << std::endl;
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_REG) { // Regular file
            std::string filepath = userDir + "/" + entry->d_name;
            file_metadata meta = get_file_metadata(filepath);
            files.push_back(meta);
        }
    }

    closedir(dir);
    return true;
}

bool FileManager::getFileMetadata(const std::string& username, const std::string& filename, file_metadata& meta) {
    std::lock_guard<std::mutex> lock(fileMutex);
    std::string filepath = getUserDir(username) + "/" + filename;
    meta = get_file_metadata(filepath);
    return meta.size > 0; // Return true if file exists and has size > 0
}

std::string FileManager::getUserDir(const std::string& username) const {
    return baseDirectory + "/" + username;
} 