#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <string>
#include <vector>
#include <ctime>
#include <sys/stat.h>
#include <cstring>

// Packet types
#define PACKET_TYPE_CMD 1
#define PACKET_TYPE_ACK 2
#define PACKET_TYPE_FILE 3
#define PACKET_TYPE_DATA 4

// Commands
#define CMD_UPLOAD 1
#define CMD_LIST_SERVER 2  
#define CMD_EXIT 5

// Maximum payload size
#define MAX_PAYLOAD_SIZE 1024

// Packet structure
struct packet {
    uint8_t type;
    uint32_t seqn;
    uint32_t total_size;
    uint32_t length;
    char payload[MAX_PAYLOAD_SIZE];
};

// File metadata structure
struct file_metadata {
    std::string filename;
    size_t size;
    time_t mtime;  // Last modification time
    time_t atime;  // Last access time
    time_t ctime;  // Creation time
};

// Function to get file metadata
inline file_metadata get_file_metadata(const std::string& path) {
    file_metadata meta;
    struct stat st;
    
    if (stat(path.c_str(), &st) == 0) {
        meta.filename = path;
        meta.mtime = st.st_mtime;
        meta.atime = st.st_atime;
        meta.ctime = st.st_ctime;
        meta.size = st.st_size;
    }
    
    return meta;
}

#endif // COMMON_H 