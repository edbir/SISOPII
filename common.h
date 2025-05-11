#ifndef COMMON_H
#define COMMON_H

#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <cstring>
#include <ctime>

// Packet types
#define PACKET_TYPE_CMD 1
#define PACKET_TYPE_DATA 2
#define PACKET_TYPE_ACK 3
#define PACKET_TYPE_FILE 4

// Command types
#define CMD_UPLOAD 1
#define CMD_DOWNLOAD 2
#define CMD_DELETE 3
#define CMD_LIST_SERVER 4
#define CMD_LIST_CLIENT 5
#define CMD_GET_SYNC_DIR 6
#define CMD_EXIT 7

// Packet and payload sizes
#define MAX_PACKET_SIZE 1024
#define MAX_PAYLOAD_SIZE (MAX_PACKET_SIZE - sizeof(uint16_t) - sizeof(uint16_t) - sizeof(uint32_t) - sizeof(uint16_t))
#define MAX_FILENAME_LENGTH 256

// Packet structure for communication
#pragma pack(push, 1)  // Ensure no padding
struct packet {
    uint16_t type;        // Type of packet (CMD, DATA, ACK, ERROR)
    uint16_t seqn;        // Sequence number
    uint32_t total_size;  // Total number of fragments
    uint16_t length;      // Length of payload
    char payload[MAX_PAYLOAD_SIZE]; // Payload data
};
#pragma pack(pop)

// File metadata structure
#pragma pack(push, 1)  // Ensure no padding
struct file_metadata {
    char filename[MAX_FILENAME_LENGTH];
    time_t mtime;  // Modification time
    time_t atime;  // Access time
    time_t ctime;  // Creation/Change time
    size_t size;   // File size
};
#pragma pack(pop)

// Function to get file metadata
inline file_metadata get_file_metadata(const std::string& path) {
    file_metadata meta;
    struct stat st;
    
    if (stat(path.c_str(), &st) == 0) {
        strncpy(meta.filename, path.c_str(), MAX_FILENAME_LENGTH - 1);
        meta.filename[MAX_FILENAME_LENGTH - 1] = '\0';  // Ensure null termination
        meta.mtime = st.st_mtime;
        meta.atime = st.st_atime;
        meta.ctime = st.st_ctime;
        meta.size = st.st_size;
    }
    
    return meta;
}

#endif // COMMON_H 