#include <iostream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h> // For inet_pton (TCP/IP)
#include <sys/un.h>    // For struct sockaddr_un (UNIX Domain Sockets)
#include <unistd.h>    // For read/write/close
#include <cstring>     // For memset

// Function to connect to the server using UNIX domain socket
int connectUnixSocket(const std::string& socketPath) {
    int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd == -1) {
        std::cerr << "Failed to create UNIX domain socket\n";
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "Failed to connect to UNIX domain socket\n";
        close(sockfd);
        return -1;
    }

    return sockfd;
}

// Function to connect to the server using TCP socket
int connectTcpSocket(const std::string& ip, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        std::cerr << "Failed to create TCP socket\n";
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
        std::cerr << "Invalid IP address format\n";
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        std::cerr << "Failed to connect to TCP socket\n";
        close(sockfd);
        return -1;
    }

    return sockfd;
}


// Read one '\n'-terminated response. The protocol is lockstep (one
// response in flight), but a response can be far larger than any fixed
// buffer -- SCAN returns up to 10k length-prefixed pairs on one line.
static bool readResponse(int sockfd, std::string &out) {
    out.clear();
    char buf[65536];
    while (true) {
        ssize_t n = read(sockfd, buf, sizeof(buf));
        if (n <= 0) {
            return false;
        }
        out.append(buf, n);
        if (out.back() == '\n') {
            out.pop_back();
            return true;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <IP/SocketPath> <Port (only for TCP)> <optinal direct command>\n";
        return 1;
    }

    std::string ipOrSocketPath = argv[1];
    int sockfd;

    // Decide whether to use UNIX domain socket or TCP socket based on the number of arguments
    if (argc == 3 && ipOrSocketPath != "127.0.0.1") {
        int port = std::stoi(argv[2]);
        sockfd = connectTcpSocket(ipOrSocketPath, port);
    } else {
        ipOrSocketPath = "/tmp/server.sock";
        sockfd = connectUnixSocket(ipOrSocketPath);
    }

    if (sockfd == -1) return 1; // Connection failed

    if ( argv[3] ) {
        std::string command = argv[3];
        if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1) {
            std::cerr << "Failed to send command\n";
            close(sockfd);
            return 1;
        }
        std::string response;
        if (!readResponse(sockfd, response)) {
            std::cerr << "Failed to receive response or connection closed by server\n";
            close(sockfd);
            return 1;
        }
        std::cout << response << std::endl;
    } else {

    std::cout << "Connected to server. Type your commands below:\n> ";
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command.empty()) continue; // Skip empty commands
            if (send(sockfd, (command + "\n").c_str(), command.length() + 1, 0) == -1) {
                std::cerr << "Failed to send command\n";
                break;
            }
            std::string response;
            if (!readResponse(sockfd, response)) {
                std::cerr << "Failed to receive response or connection closed by server\n";
                break;
            }
            std::cout << response << "\n> ";
        }
    }

    close(sockfd);
    return 0;
}
