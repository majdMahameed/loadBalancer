/*
 * lb_smartLB.cpp – Thread‑per‑client + SERPT scheduling (fixed alias name)
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Steady = std::chrono::steady_clock;

struct Backend {
    enum Role { VIDEO, MUSIC } role;
    std::string ip;
    uint16_t port;
    int fd;
    std::mutex mtx;
    double vfinish;

    Backend(Role r, const std::string& ip_, uint16_t p)
        : role(r), ip(ip_), port(p), fd(-1), vfinish(0) {}

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    Backend(Backend&& other) noexcept : role(other.role), ip(std::move(other.ip)), port(other.port), fd(other.fd), vfinish(other.vfinish) { other.fd = -1; }
    Backend& operator=(Backend&&) = delete;
};

static ssize_t read_n(int fd, void* buf, size_t n) {
    size_t left = n; char* p = static_cast<char*>(buf);
    while (left) { ssize_t r = recv(fd, p, left, 0); if (r <= 0) return r; left -= r; p += r; }
    return n;
}
static ssize_t write_n(int fd, const void* buf, size_t n) {
    size_t left = n; const char* p = static_cast<const char*>(buf);
    while (left) { ssize_t w = send(fd, p, left, 0); if (w <= 0) return w; left -= w; p += w; }
    return n;
}
static int connect_once(const std::string& ip, uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port); inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) { close(s); return -1; }
    return s;
}

static std::vector<Backend> backends;
static std::mutex sched_mtx;
static Steady::time_point start_ts;

static double now_seconds() { return std::chrono::duration<double>(Steady::now() - start_ts).count(); }

static int multiplier(char t, Backend::Role r) {
    if (r == Backend::VIDEO) return t=='M'?2:1;
    // MUSIC server
    if (t=='M') return 1; if (t=='V') return 3; return 2;
}

static size_t pick_backend(char type, int base) {
    std::lock_guard<std::mutex> g(sched_mtx);
    double tnow = now_seconds(), best = 1e100; size_t idx = 0;
    for (size_t i=0;i<backends.size();++i) {
        const Backend& b = backends[i]; double dur = multiplier(type,b.role)*base; double v = (b.vfinish<tnow?tnow:b.vfinish)+dur;
        if (v<best){best=v; idx=i;}
    }
    backends[idx].vfinish = best; return idx;
}
static bool ensure_connected(Backend& b) {
    if (b.fd!=-1) return true; b.fd=connect_once(b.ip,b.port); if(b.fd==-1){std::cerr<<"[LB] cannot connect to "<<b.ip<<":"<<b.port<<"\n";return false;} return true; }

static void handle_client(int cfd) {
    char req[2]; if(read_n(cfd,req,2)!=2){close(cfd);return;} char type=req[0]; int base=req[1]-'0'; if(base<=0||base>9){close(cfd);return;}
    size_t idx=pick_backend(type,base); Backend& b=backends[idx]; if(!ensure_connected(b)){close(cfd);return;}
    std::lock_guard<std::mutex> g(b.mtx);
    if(write_n(b.fd,req,2)!=2){close(b.fd);b.fd=-1;close(cfd);return;}
    char resp[1024]; ssize_t n=recv(b.fd,resp,sizeof(resp),0); if(n>0) write_n(cfd,resp,n); else {close(b.fd);b.fd=-1;}
    close(cfd);
}

int main(){ start_ts=Steady::now(); backends.reserve(3); backends.emplace_back(Backend::VIDEO,"192.168.0.101",80); backends.emplace_back(Backend::VIDEO,"192.168.0.102",80); backends.emplace_back(Backend::MUSIC,"192.168.0.103",80);
    int listen_fd=socket(AF_INET,SOCK_STREAM,0); if(listen_fd<0){perror("socket");return 1;} int opt=1; setsockopt(listen_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt)); sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=INADDR_ANY; addr.sin_port=htons(80); if(bind(listen_fd,(sockaddr*)&addr,sizeof(addr))<0){perror("bind");return 1;} if(listen(listen_fd,128)<0){perror("listen");return 1;}
    std::cout<<"[LB] SmartLB listening on 0.0.0.0:80\n";
    while(true){int cfd=accept(listen_fd,nullptr,nullptr); if(cfd<0){perror("accept");continue;} std::thread(handle_client,cfd).detach();}
}
