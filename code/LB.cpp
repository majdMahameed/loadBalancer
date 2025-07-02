/*
 * lb_round_robin_persistent.cpp  –  **persistent‑connection** version
 *
 * Same spirit as the first draft but keeps ONE TCP connection open to each back‑end
 * server for the lifetime of the program. This matches the reference servers’
 * expectations and prevents them from crashing (they close on EOF).
 *
 * Build (old compiler):   g++ -std=c++11 -Wall -O2 lb_round_robin_persistent.cpp -o lb
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

struct Backend {
    std::string ip;
    uint16_t    port;
    int         fd;    // persistent socket, -1 if not connected yet
};

// ──────────────────────────────────────────────────────────────────────────────
// Helper functions
// ──────────────────────────────────────────────────────────────────────────────
static ssize_t read_n(int fd, void* buf, size_t n) {
    size_t left = n;
    char*  p    = static_cast<char*>(buf);
    while (left) {
        ssize_t r = recv(fd, p, left, 0);
        if (r <= 0) return r;   // error or peer closed
        left -= r;
        p    += r;
    }
    return n;
}

static ssize_t write_n(int fd, const void* buf, size_t n) {
    size_t left = n;
    const char* p = static_cast<const char*>(buf);
    while (left) {
        ssize_t w = send(fd, p, left, 0);
        if (w <= 0) return w;
        left -= w;
        p    += w;
    }
    return n;
}

static int connect_once(const std::string& ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        close(sock); return -1;
    }
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock); return -1;
    }
    return sock;
}

static bool ensure_connected(Backend& b) {
    if (b.fd != -1) return true;                 // already connected
    b.fd = connect_once(b.ip, b.port);
    if (b.fd == -1) {
        std::cerr << "[LB] Failed to connect to server "
                  << b.ip << ":" << b.port << "\n";
        return false;
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
int main() {
    std::vector<Backend> backends = {
        {"192.168.0.101", 80, -1},
        {"192.168.0.102", 80, -1},
        {"192.168.0.103", 80, -1},
    };

    // listening socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in lb_addr{};
    lb_addr.sin_family      = AF_INET;
    lb_addr.sin_addr.s_addr = INADDR_ANY;
    lb_addr.sin_port        = htons(80);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&lb_addr), sizeof(lb_addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_fd, 128) < 0) { perror("listen"); return 1; }

    std::cout << "[LB] Listening on 0.0.0.0:80\n";

    size_t rr = 0;   // round‑robin index

    for (;;) {
        // ── accept one client (blocking) ────────────────────────────────────
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) { perror("accept"); continue; }

        char req[2];
        if (read_n(client_fd, req, 2) != 2) { close(client_fd); continue; }

        // ── pick backend (RR) and ensure it’s connected ────────────────────
        Backend& b = backends[rr];
        rr = (rr + 1) % backends.size();

        if (!ensure_connected(b)) { close(client_fd); continue; }

        // If send/recv fails, we close the backend socket and mark fd=-1 so
        // next request triggers reconnect.
        bool backend_ok = true;
        if (write_n(b.fd, req, 2) != 2) backend_ok = false;
        else {
            char resp[1024];
            ssize_t n = recv(b.fd, resp, sizeof(resp), 0);
            if (n > 0) {
                write_n(client_fd, resp, static_cast<size_t>(n));
            } else backend_ok = false;          // peer closed or error
        }

        if (!backend_ok) {
            std::cerr << "[LB] backend " << b.ip << " reset – reconnect next time\n";
            close(b.fd);
            b.fd = -1;
        }

        close(client_fd);
    }

    return 0;   // never reached
}
