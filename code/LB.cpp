/*
 * lb_round_robin.cpp
 *
 * A minimal blocking, single‑threaded load balancer that listens on port 80
 * and forwards each 2‑byte request to backend servers using a round‑robin
 * policy.  Designed to run inside the Mininet lab (lb1 node).
 *
 * Build: g++ -std=c++17 -Wall -O2 lb_round_robin.cpp -o lb
 *
 * Limitations:
 *  - Processes one client at a time (blocking accept/recv/send).
 *  - Opens a fresh TCP connection to the chosen server per request.
 *  - No error‑handling beyond basic checks.
 *  - Suitable only for demonstrating socket API usage.
 */

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

struct Server {
    std::string ip;
    uint16_t port;
};

// read exactly n bytes (or return <=0 on error / disconnect)
ssize_t read_n(int fd, void* buf, size_t n) {
    size_t left = n;
    char* p = static_cast<char*>(buf);
    while (left > 0) {
        ssize_t r = recv(fd, p, left, 0);
        if (r <= 0) return r;
        left -= r;
        p += r;
    }
    return n;
}

// write exactly n bytes (or return <=0 on error)
ssize_t write_n(int fd, const void* buf, size_t n) {
    size_t left = n;
    const char* p = static_cast<const char*>(buf);
    while (left > 0) {
        ssize_t w = send(fd, p, left, 0);
        if (w <= 0) return w;
        left -= w;
        p += w;
    }
    return n;
}

int connect_to_server(const Server& s) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s.port);
    if (inet_pton(AF_INET, s.ip.c_str(), &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main() {
    // Backend pool – adjust IPs if your topology differs
    std::vector<Server> servers = {
        {"192.168.0.101", 80},
        {"192.168.0.102", 80},
        {"192.168.0.103", 80},
    };

    // Create listening socket on port 80
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in lb_addr{};
    lb_addr.sin_family = AF_INET;
    lb_addr.sin_addr.s_addr = INADDR_ANY;  // bind to all interfaces (eth1 in lb1)
    lb_addr.sin_port = htons(80);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&lb_addr), sizeof(lb_addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        return 1;
    }

    std::cout << "[LB] Listening on 0.0.0.0:80\n";

    size_t rr_index = 0;
    for (;;) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        char req[2];
        if (read_n(client_fd, req, 2) != 2) {
            close(client_fd);
            continue;
        }

        // Choose backend using round robin
        const Server& backend = servers[rr_index];
        rr_index = (rr_index + 1) % servers.size();

        int serv_fd = connect_to_server(backend);
        if (serv_fd < 0) {
            std::cerr << "[LB] Failed to connect to server "
                      << backend.ip << ":" << backend.port << "\n";
            close(client_fd);
            continue;
        }

        if (write_n(serv_fd, req, 2) != 2) {
            close(serv_fd);
            close(client_fd);
            continue;
        }

        // Response is at most 1024 bytes (servers send a short line + '\n')
        char resp[1024];
        ssize_t n = recv(serv_fd, resp, sizeof(resp), 0);
        if (n > 0) {
            write_n(client_fd, resp, static_cast<size_t>(n));
        }

        close(serv_fd);
        close(client_fd);
    }

    close(listen_fd);
    return 0;
}
