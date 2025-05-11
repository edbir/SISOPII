CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread
LDFLAGS = -pthread

SRCS = server.cpp client.cpp network.cpp file_manager.cpp sync_manager.cpp
SERVER_OBJS = server.o network.o file_manager.o sync_manager.o
CLIENT_OBJS = client.o network.o file_manager.o sync_manager.o

all: server client

server: $(SERVER_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

client: $(CLIENT_OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) server client

.PHONY: all clean 