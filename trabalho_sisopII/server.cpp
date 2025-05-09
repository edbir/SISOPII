#include <iostream>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unordered_map>
#include <mutex>
#include <filesystem>
#include <fstream>

#define PORT 4000

std::unordered_map<std::string, int> user_sessions;
std::mutex session_mutex;

void handle_client(int client_sockfd, sockaddr_in client_addr) {
    char username[64];
    std::memset(username, 0, sizeof(username));

    // lê username
    int n = read(client_sockfd, username, sizeof(username) - 1);
    if (n <= 0) {
        std::cerr << "ERROR reading username from socket\n";
        close(client_sockfd);
        return;
    }

    // verifica e limita sessões por usuário
    std::string uname(username);
    bool accepted = false;
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        if (user_sessions[uname] < 2) {
            user_sessions[uname]++;
            accepted = true;
        }
    }

    // se exceder o limite, envia erro e desconecta
    if (!accepted) {
        std::string msg = "ERROR: Too many sessions for user '" + uname + "'";
        write(client_sockfd, msg.c_str(), msg.size());
        close(client_sockfd);
        return;
    }

    // cria diretório do usuário se não existir
    std::string user_dir = "sync_dir_" + uname;
    std::filesystem::create_directories(user_dir);

    // recebe o tamanho do nome do arquivo
    uint32_t filename_len;
    int n2 = read(client_sockfd, &filename_len, sizeof(filename_len));
    if (n2 <= 0) {
        std::cerr << "ERROR reading filename length\n";
        close(client_sockfd);
        return;
    }
    // recebe nome do arquivo
    char filename[256];
    std::memset(filename, 0, sizeof(filename));
    n2 = read(client_sockfd, filename, sizeof(filename) - 1);
    std::cerr << "read() returned n = " << n << "\n";
    if (n2 <= 0) {
        std::cerr << "ERROR reading filename from socket\n";
        close(client_sockfd);
        return;
    }

    std::string filepath = user_dir + "/" + filename;

    // abre arquivo de saída
    std::ofstream outfile(filepath, std::ios::binary);
    if (!outfile.is_open()) {
        std::cerr << "ERROR: could not open file to write: " << filepath << "\n";
        close(client_sockfd);
        return;
    }

    // lê dados do arquivo do socket
    char buffer[4096];
    while ((n = read(client_sockfd, buffer, sizeof(buffer))) > 0) {
        outfile.write(buffer, n);
    }

    outfile.close();

    std::cout << "[User: " << uname << " | IP: "
              << inet_ntoa(client_addr.sin_addr)
              << "] File '" << filename << "' received and saved to '" << filepath << "'\n";

    close(client_sockfd);

    // libera sessão
    {
        std::lock_guard<std::mutex> lock(session_mutex);
        user_sessions[uname]--;
        if (user_sessions[uname] == 0)
            user_sessions.erase(uname);
    }
}

int main() {
    int sockfd;
    struct sockaddr_in serv_addr{}, cli_addr{};
    socklen_t clilen = sizeof(cli_addr);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "ERROR opening socket\n";
        return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    std::memset(&(serv_addr.sin_zero), 0, 8);

    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "ERROR on binding\n";
        close(sockfd);
        return 1;
    }

    listen(sockfd, 5);
    std::cout << "Server listening on port " << PORT << "...\n";

    // loop principal — aceita conexões
    while (true) {
        int newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (newsockfd < 0) {
            std::cerr << "ERROR on accept\n";
            continue;
        }

        std::thread client_thread(handle_client, newsockfd, cli_addr);
        client_thread.detach();
    }

    close(sockfd);
    return 0;
}


