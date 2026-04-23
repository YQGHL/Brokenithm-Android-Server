#pragma once

#include <string>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstdarg>
#include "socket.h"

// ---- 网络辅助 ----

inline void socketSetTimeout(SOCKET sHost, int timeout) {
    setsockopt(sHost, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(int));
    setsockopt(sHost, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(int));
}

inline int socketBind(SOCKET sHost, long addr, uint16_t port) {
    sockaddr_in srcaddr = {};
    srcaddr.sin_family = AF_INET;
    srcaddr.sin_addr.s_addr = addr;
    srcaddr.sin_port = htons(port);
    return bind(sHost, reinterpret_cast<sockaddr*>(&srcaddr), sizeof(srcaddr));
}

inline sockaddr_in makeBroadcastAddr(uint16_t port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    addr.sin_port = htons(port);
    return addr;
}

inline sockaddr_in makeIPv4Addr(const std::string& host, uint16_t port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, host.data(), &addr.sin_addr);
    addr.sin_port = htons(port);
    return addr;
}

inline int socketSendTo(SOCKET sHost, const sockaddr_in& addr,
                        const void* data, size_t len) {
    return sendto(sHost, static_cast<const char*>(data),
                  static_cast<int>(len), 0,
                  reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

// ---- 日志 ----

inline std::string getTime(int type) {
    time_t lt;
    char tmpbuf[32];
    std::string format;
    lt = time(nullptr);
    struct tm local_tm;
    localtime_s(&local_tm, &lt);
    switch (type) {
    case 1:  format = "%Y%m%d-%H%M%S"; break;
    case 2:  format = "%Y/%m/%d %a %H:%M:%S"; break;
    case 3:  format = "%Y-%m-%d %H:%M:%S"; break;
    default: format = "%Y-%m-%d %H:%M:%S"; break;
    }
    strftime(tmpbuf, sizeof(tmpbuf), format.data(), &local_tm);
    return std::string(tmpbuf);
}

template <typename... Args>
inline void printErr(const char* format, Args... args) {
    std::string t = "[" + getTime(2) + "] ";
    fprintf(stderr, "%s", t.data());
    fprintf(stderr, format, args...);
}

template <typename... Args>
inline void dprintf(const char* format, Args... args) {
    fprintf(stderr, format, args...);
}

inline void dump(const void* ptr, size_t nbytes, bool hex_string = false) {
    const uint8_t* bytes = static_cast<const uint8_t*>(ptr);
    if (nbytes == 0) {
        dprintf("\t--- Empty ---\n");
        return;
    }
    if (hex_string) {
        for (size_t i = 0; i < nbytes; ++i) {
            dprintf("%02x", bytes[i]);
        }
        dprintf("\n");
        return;
    }
    for (size_t i = 0; i < nbytes; i += 16) {
        dprintf("    %08x:", static_cast<int>(i));
        size_t j;
        for (j = 0; i + j < nbytes && j < 16; ++j) {
            dprintf(" %02x", bytes[i + j]);
        }
        while (j < 16) {
            dprintf("   ");
            ++j;
        }
        dprintf(" ");
        for (j = 0; i + j < nbytes && j < 16; ++j) {
            uint8_t c = bytes[i + j];
            dprintf("%c", (c < 0x20 || c >= 0x7F) ? '.' : static_cast<char>(c));
        }
        dprintf("\n");
    }
    dprintf("\n");
}
