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


std::string FileManager::getUserDir(const std::string& username) const {
    return baseDirectory + "/" + username;
} 