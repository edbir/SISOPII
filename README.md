


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
- `exit` - Close the session

## Project Structure

- `server.cpp` - Server implementation
- `client.cpp` - Client implementation
- `common.h` - Common definitions and structures
- `sync_manager.h/cpp` - File synchronization management
- `file_manager.h/cpp` - File operations management
- `network.h/cpp` - Network communication utilities
