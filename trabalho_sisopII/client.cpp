#include <iostream>
#include <fstream>
#include <cstring>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <filesystem>

#define PORT 4000

int main(int argc, char *argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <username> <server_ip> <file_path>\n";
        return 1;
    }

    const char* username = argv[1];
    const char* server_ip = argv[2];
    const char* file_path = argv[3];

    // abre o arquivo local para leitura binária
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "ERROR: could not open file '" << file_path << "'\n";
        return 1;
    }

    // extrai o nome do arquivo a partir do caminho
    std::string filename = std::filesystem::path(file_path).filename().string();

    int sockfd;
    struct sockaddr_in serv_addr{};
    struct hostent *server;

    server = gethostbyname(server_ip);
    if (server == nullptr) {
        std::cerr << "ERROR: no such host\n";
        return 1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "ERROR opening socket\n";
        return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    serv_addr.sin_addr = *((struct in_addr *)server->h_addr);
    std::memset(&(serv_addr.sin_zero), 0, 8);

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "ERROR connecting\n";
        close(sockfd);
        return 1;
    }

    // envia o nome de usuário
    if (write(sockfd, username, strlen(username) + 1) < 0) {
        std::cerr << "ERROR sending username\n";
        close(sockfd);
        return 1;
    }

    usleep(1000);

    // envia o tamanho do nome do arquivo
    uint32_t filename_len = filename.size();
    if (write(sockfd, &filename_len, sizeof(filename_len)) < 0) {
        std::cerr << "ERROR sending filename length\n";
        close(sockfd);
        return 1;
    }

    // envia o nome do arquivo com terminador nulo
    int n = write(sockfd, filename.c_str(), filename.size() + 1);  // +1 para incluir '\0'
    if (n < 0) {
        std::cerr << "ERROR sending filename\n";
        close(sockfd);
        return 1;
    }

    // envia o conteúdo do arquivo
    char buffer[4096];
    while (!file.eof()) {
        file.read(buffer, sizeof(buffer));
        std::streamsize bytesRead = file.gcount();

        if (write(sockfd, buffer, bytesRead) != bytesRead) {
            std::cerr << "ERROR sending file data\n";
            close(sockfd);
            return 1;
        }
    }

    std::cout << "File '" << filename << "' sent successfully.\n";

    file.close();
    close(sockfd);
    return 0;
}
