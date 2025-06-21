# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread
LDFLAGS = -pthread
# Specific linker flag for components that use the C++ filesystem library
FS_LDFLAG = -lstdc++fs

# List of all target executables to be built
TARGETS = server client frontend

# Define the object files required for each executable
SERVER_OBJS = server.o network.o file_manager.o sync_manager.o
CLIENT_OBJS = client.o network.o file_manager.o sync_manager.o
FRONTEND_OBJS = frontend.o network.o

# The default 'all' target builds all executables
all: $(TARGETS)

# --- Build Rules for Executables ---

# Rule to link the server executable
server: $(SERVER_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(FS_LDFLAG)

# Rule to link the client executable
client: $(CLIENT_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^ $(FS_LDFLAG)

# Rule to link the frontend executable
frontend: $(FRONTEND_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

# --- Generic Compilation Rule ---

# Generic rule to compile .cpp files into .o object files
# This will be used for all .cpp sources
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# --- Housekeeping ---

# Rule to clean up all build artifacts
clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) $(FRONTEND_OBJS) $(TARGETS)

# Phony targets are not actual files, they are just names for recipes
.PHONY: all clean

