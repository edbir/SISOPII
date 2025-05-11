# Dropbox-like File Synchronization System

A file synchronization system similar to Dropbox, implemented in C/C++ using TCP sockets.

## Features

- Multiple user support
- Multiple simultaneous sessions per user (up to 2 devices)
- Automatic file synchronization
- File upload/download capabilities
- Real-time file monitoring and synchronization
- Command-line interface

## Building the Project

```bash
# Compile the server
g++ -o server server.cpp -pthread

# Compile the client
g++ -o client client.cpp -pthread
```

## Usage

### Server
```bash
./server <port>
```

### Client
```bash
./client <username> <server_ip_address> <port>
```

## Available Commands

- `upload <path/filename.ext>` - Upload a file to the server
- `download <filename.ext>` - Download a file from the server
- `delete <filename.ext>` - Delete a file from sync_dir
- `list_server` - List files on the server
- `list_client` - List files in sync_dir
- `get_sync_dir` - Create sync_dir and start synchronization
- `exit` - Close the session

## Project Structure

- `server.cpp` - Server implementation
- `client.cpp` - Client implementation
- `common.h` - Common definitions and structures
- `sync_manager.h/cpp` - File synchronization management
- `file_manager.h/cpp` - File operations management
- `network.h/cpp` - Network communication utilities
