#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include "common.h"

class FileManager {
public:
    FileManager(const std::string& baseDir);
    ~FileManager();

    // File operations
    bool createUserDir(const std::string& username);
    bool deleteFile(const std::string& username, const std::string& filename);
    bool listFiles(const std::string& username, std::vector<file_metadata>& files);
    bool getFileMetadata(const std::string& username, const std::string& filename, file_metadata& meta);
    std::string getUserDir(const std::string& username);

private:
    std::string baseDirectory;
    mutable std::mutex fileMutex;
};

#endif // FILE_MANAGER_H 