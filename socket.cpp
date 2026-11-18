#include "socket.hpp"
#include <algorithm>
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <cerrno>
#endif
namespace net {
bool init(){
#ifdef _WIN32
 WSADATA d{}; return WSAStartup(MAKEWORD(2,2),&d)==0;
#else
 return true;
#endif
}
void cleanup(){
#ifdef _WIN32
 WSACleanup();
#endif
}
bool send_all(socket_t fd, const void* data, std::size_t size) {
    const char* p = static_cast<const char*>(data);
    std::size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        const int part = static_cast<int>(std::min<std::size_t>(size - sent, 64 * 1024));
        const int n = ::send(fd, p + sent, part, 0);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEINTR || e == WSAEWOULDBLOCK || e == WSAETIMEDOUT) continue;
            return false;
        }
#else
        const ssize_t n = ::send(fd, p + sent, size - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_all(socket_t fd, void* data, std::size_t size) {
    char* p = static_cast<char*>(data);
    std::size_t got = 0;
    while (got < size) {
#ifdef _WIN32
        const int part = static_cast<int>(std::min<std::size_t>(size - got, 64 * 1024));
        const int n = ::recv(fd, p + got, part, 0);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEINTR || e == WSAEWOULDBLOCK || e == WSAETIMEDOUT) continue;
            return false;
        }
#else
        const ssize_t n = ::recv(fd, p + got, size - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
#endif
        if (n <= 0) return false;
        got += static_cast<std::size_t>(n);
    }
    return true;
}

bool recv_line(socket_t fd, std::string& line, std::size_t max_len) {
    line.clear();
    char c = 0;
    while (line.size() < max_len) {
#ifdef _WIN32
        const int n = ::recv(fd, &c, 1, 0);
        if (n == SOCKET_ERROR) {
            const int e = WSAGetLastError();
            if (e == WSAEINTR || e == WSAETIMEDOUT) continue;
            return false;
        }
#else
        const ssize_t n = ::recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
#endif
        if (n <= 0) return false;
        if (c == '\n') return true;
        if (c != '\r') line.push_back(c);
    }
    return false;
}
bool set_io_timeout(socket_t fd,int ms){
#ifdef _WIN32
 DWORD v=ms<0?0:(DWORD)ms;return setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(const char*)&v,sizeof(v))==0&&setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,(const char*)&v,sizeof(v))==0;
#else
 timeval tv{};tv.tv_sec=ms/1000;tv.tv_usec=(ms%1000)*1000;return setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv))==0&&setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv))==0;
#endif
}
void close_socket(socket_t fd){if(fd==invalid_socket)return;
#ifdef _WIN32
 closesocket(fd);
#else
 close(fd);
#endif
}
void shutdown_socket(socket_t fd){if(fd==invalid_socket)return;
#ifdef _WIN32
 shutdown(fd,SD_BOTH);
#else
 shutdown(fd,SHUT_RDWR);
#endif
}
std::string peer_address(socket_t fd) {
    sockaddr_in address{};
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return "unknown";
    char buffer[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    return std::string(buffer);
}
}
