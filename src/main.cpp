#include <string>
#include <atomic>
#include <thread>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <array>
#include <vector>
#include <cinttypes>

#include "socket.h"
#include "defer.h"
#include "version.h"
#include "struct.h"
#include "shared_memory.h"
#include "signal_processor.h"
#include "utils.h"

#include <windows.h>
#include <conio.h>

// ============================================================================
// 全局配置与状态
// ============================================================================
static uint16_t g_server_port = 52468;
static uint16_t g_remote_port = 52468;
static std::string g_remote_address;
static bool g_tcp_mode = false;
static size_t g_tcp_buffer_size = 96;
static size_t g_tcp_receive_threshold = 48;

static std::atomic_bool g_exit_flag{ false };
static std::atomic_bool g_connected{ false };

static uint32_t g_last_input_packet_id = 0;

static constexpr uint8_t kLedHead[4] = { 0x63, 'L', 'E', 'D' };

// ============================================================================
// 命令行解析（手写，兼容 MSVC / MinGW）
// ============================================================================
static void parseArgs(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc) {
            g_server_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-T") {
            g_tcp_mode = true;
        } else if (arg == "-r" && i + 1 < argc) {
            g_tcp_receive_threshold = static_cast<size_t>(std::atoi(argv[++i]));
            g_tcp_buffer_size = g_tcp_receive_threshold * 2;
        }
    }
}

// ============================================================================
// 信息输出
// ============================================================================
static void printInfo() {
    printf("=================================================\n");
    printf("=          Brokenithm-Evolved-Android:          =\n");
    printf("=     Brokenithm with full IO over network      =\n");
    printf("=               " VERSION " by XTindy                =\n");
    printf("=              Original: esterTion              =\n");
    printf("=================================================\n\n");
}

// ============================================================================
// 包序号检测
// ============================================================================
static void updatePacketId(uint32_t newPacketId) {
    if (g_last_input_packet_id > newPacketId) {
        printErr("[WARN] Packet #%" PRIu32 " came too late\n", newPacketId);
    } else if (newPacketId > g_last_input_packet_id + 1) {
        printErr("[WARN] Packets between #%" PRIu32 " and #%" PRIu32 " total %" PRIu32 " missing\n",
                 g_last_input_packet_id, newPacketId, newPacketId - g_last_input_packet_id - 1);
    } else if (newPacketId == g_last_input_packet_id) {
        printErr("[WARN] Packet #%" PRIu32 " duplicated\n", newPacketId);
    }
    g_last_input_packet_id = newPacketId;
}

static void getSocksAddress(const PacketConnect* pkt, std::string& address, uint16_t& port) {
    char cAddr[128] = {};
    port = ntohs(pkt->port);
    switch (pkt->addrType) {
    case 1:
        inet_ntop(AF_INET, pkt->addr.addr4.addr, cAddr, 127);
        break;
    case 2:
        inet_ntop(AF_INET6, pkt->addr.addr6, cAddr, 127);
        break;
    }
    address.assign(cAddr);
}

enum { FUNCTION_COIN = 1, FUNCTION_CARD };
enum { CARD_AIME, CARD_FELICA };

static void printCardInfo(uint8_t cardType, uint8_t* cardId) {
    switch (cardType) {
    case CARD_AIME:
        printErr("[INFO] Card Type: Aime\t\tID: ");
        dump(cardId, 10, true);
        break;
    case CARD_FELICA:
        printErr("[INFO] Card Type: FeliCa\tIDm: ");
        dump(cardId, 8, true);
        break;
    }
}

// ============================================================================
// TCP 流重组环形缓冲区（替代 std::string，消除 O(n) 数据搬移）
// ============================================================================
class TcpRingBuffer {
public:
    static constexpr size_t kCap = 4096; // 2^12，支持位运算取模

    void append(const char* data, size_t len) {
        if (len > writable()) compact();
        if (len > writable()) return;
        size_t w = w_ & (kCap - 1);
        size_t first = std::min(len, kCap - w);
        std::memcpy(buf_.data() + w, data, first);
        if (first < len) {
            std::memcpy(buf_.data(), data + first, len - first);
        }
        w_ += len;
    }

    // 查看下一个完整包，但不消费
    bool peekPacket(char* out, size_t maxLen, size_t& outLen) const {
        size_t readable = w_ - r_;
        if (readable == 0) return false;
        uint8_t ps = static_cast<uint8_t>(buf_[r_ & (kCap - 1)]);
        size_t total = static_cast<size_t>(ps) + 1;
        if (readable < total || total > maxLen) return false;
        size_t r = r_ & (kCap - 1);
        size_t first = std::min(total, kCap - r);
        std::memcpy(out, buf_.data() + r, first);
        if (first < total) {
            std::memcpy(out + first, buf_.data(), total - first);
        }
        outLen = total;
        return true;
    }

    void consume(size_t len) { r_ += len; }
    size_t readableCount() const { return w_ - r_; }

private:
    std::array<char, kCap> buf_{};
    size_t r_ = 0, w_ = 0;

    size_t writable() const { return kCap - (w_ - r_); }

    void compact() {
        size_t count = w_ - r_;
        if (count == 0) { r_ = w_ = 0; return; }
        size_t r = r_ & (kCap - 1);
        if (r + count <= kCap) {
            std::memmove(buf_.data(), buf_.data() + r, count);
        } else {
            std::array<char, kCap> tmp;
            size_t first = kCap - r;
            std::memcpy(tmp.data(), buf_.data() + r, first);
            std::memcpy(tmp.data() + first, buf_.data(), count - first);
            std::memcpy(buf_.data(), tmp.data(), count);
        }
        r_ = 0;
        w_ = count;
    }
};

// ============================================================================
// PING 处理（需要 socket，不在 processPacket 内处理）
// ============================================================================
static bool handlePing(const char* buf, size_t len, SOCKET sHost) {
    if (len < sizeof(PacketPing)) return false;
    if (buf[1] != 'P' || buf[2] != 'I' || buf[3] != 'N') return false;
    if (!g_connected) return true; // 识别为 PING，但此时未连接，不回复

    char response[12];
    std::memcpy(response, buf, 12);
    response[2] = 'O';
    sockaddr_in addr = makeIPv4Addr(g_remote_address, g_remote_port);
    socketSendTo(sHost, addr, response, 12);
    return true;
}

// ============================================================================
// 统一包处理（安全 memcpy，避免未对齐 reinterpret_cast）
// ============================================================================
static void processPacket(const char* buf, size_t len,
                          SharedMemory* memory,
                          SignalProcessor* processor) {
    if (len < 4) return;

    // INP / IPT：输入数据
    if (buf[1] == 'I') {
        if (buf[2] == 'N' && buf[3] == 'P' && len >= sizeof(PacketInput)) {
            PacketInput pkt;
            std::memcpy(&pkt, buf, sizeof(pkt));

            std::array<uint8_t, 6> air{};
            std::array<uint8_t, 32> slider{};
            uint8_t test = 0, service = 0;

            processor->process(pkt.airIoStatus, pkt.sliderIoStatus,
                               pkt.testBtn, pkt.serviceBtn,
                               ntohl(pkt.packetId),
                               air.data(), slider.data(), test, service);
            memory->writeInput(air.data(), slider.data(), test, service);
            updatePacketId(ntohl(pkt.packetId));
        }
        else if (buf[2] == 'P' && buf[3] == 'T' && len >= sizeof(PacketInputNoAir)) {
            PacketInputNoAir pkt;
            std::memcpy(&pkt, buf, sizeof(pkt));

            std::array<uint8_t, 6> air_in_zero{};
            std::array<uint8_t, 6> air{};
            std::array<uint8_t, 32> slider{};
            uint8_t test = 0, service = 0;

            processor->process(air_in_zero.data(), pkt.sliderIoStatus,
                               pkt.testBtn, pkt.serviceBtn,
                               ntohl(pkt.packetId),
                               air.data(), slider.data(), test, service);
            memory->writeInput(air.data(), slider.data(), test, service);
            updatePacketId(ntohl(pkt.packetId));
        }
        return;
    }

    // FNC：功能按钮
    if (buf[1] == 'F' && buf[2] == 'N' && buf[3] == 'C' && len >= sizeof(PacketFunction)) {
        PacketFunction pkt;
        std::memcpy(&pkt, buf, sizeof(pkt));
        switch (pkt.funcBtn) {
        case FUNCTION_COIN:  memory->writeCoin(1); break;
        case FUNCTION_CARD:  memory->writeCardRead(1); break;
        }
        return;
    }

    // CON / CRD：都首字节为 'C'
    if (buf[1] == 'C') {
        if (buf[2] == 'O' && buf[3] == 'N' && len >= sizeof(PacketConnect)) {
            g_last_input_packet_id = 0;
            PacketConnect pkt;
            std::memcpy(&pkt, buf, sizeof(pkt));
            getSocksAddress(&pkt, g_remote_address, g_remote_port);
            printErr("[INFO] Device %s:%d connected.\n", g_remote_address.data(), g_remote_port);
            g_connected = true;
            return;
        }
        if (buf[2] == 'R' && buf[3] == 'D' && len >= sizeof(PacketCard)) {
            PacketCard pkt;
            std::memcpy(&pkt, buf, sizeof(pkt));
            static uint8_t lastId[10] = {};
            if (pkt.remoteCardRead) {
                if (std::memcmp(lastId, pkt.remoteCardId, 10) != 0) {
                    printErr("[INFO] Got remote card.\n");
                    printCardInfo(pkt.remoteCardType, pkt.remoteCardId);
                    std::memcpy(lastId, pkt.remoteCardId, 10);
                }
            } else {
                const auto* raw = memory->raw();
                if (raw && raw->remoteCardRead) {
                    printErr("[INFO] Remote card removed.\n");
                    std::memset(lastId, 0, 10);
                }
            }
            memory->writeRemoteCard(pkt.remoteCardRead, pkt.remoteCardType, pkt.remoteCardId);
            return;
        }
        return;
    }

    // DIS：断开
    if (buf[1] == 'D' && buf[2] == 'I' && buf[3] == 'S' && len >= 4) {
        g_connected = false;
        if (g_tcp_mode) {
            g_exit_flag = true;
            printErr("[INFO] Device disconnected!\n");
        } else if (!g_remote_address.empty()) {
            printErr("[INFO] Device %s:%d disconnected.\n", g_remote_address.data(), g_remote_port);
            g_remote_address.clear();
        }
        return;
    }
}

// ============================================================================
// LED 广播线程
// ============================================================================
void threadLEDBroadcast(SOCKET sHost, const SharedMemory* memory) {
    std::array<uint8_t, 32 * 3> prev_led{};
    std::array<uint8_t, 32 * 3> curr_led{};
    std::array<uint8_t, 4 + 32 * 3> tx_buf{};
    std::memcpy(tx_buf.data(), kLedHead, 4);

    int skip_count = 0;
    sockaddr_in addr = makeIPv4Addr(g_remote_address, g_remote_port);

    while (!g_exit_flag) {
        if (!g_connected) {
            Sleep(50);
            continue;
        }

        memory->readLed(curr_led.data());
        bool changed = (std::memcmp(prev_led.data(), curr_led.data(), prev_led.size()) != 0);
        if (changed) {
            prev_led = curr_led;
            skip_count = 0;
        }

        bool should_send = changed;
        if (!changed && ++skip_count > 50) {
            should_send = true;
            skip_count = 0;
        }

        if (should_send) {
            std::memcpy(tx_buf.data() + 4, curr_led.data(), 32 * 3);
            if (socketSendTo(sHost, addr, tx_buf.data(), tx_buf.size()) < 0) {
                printErr("[Error] Cannot send packet: error %lu\n", GetLastError());
                if (g_tcp_mode) {
                    if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN) {
                        continue;
                    } else {
                        printErr("[INFO] Device disconnected!\n");
                        g_connected = false;
                        g_exit_flag = true;
                        break;
                    }
                }
            }
        }

        Sleep(10);
    }
}

// ============================================================================
// 输入接收线程（UDP / TCP）
// ============================================================================
void threadInputReceive(SOCKET sHost, SharedMemory* memory, SignalProcessor* processor) {
    if (!g_tcp_mode) {
        // ---- UDP 模式 ----
        std::array<char, 512> recv_buf{};
        while (!g_exit_flag) {
            int recv_len = recvfrom(sHost, recv_buf.data(), static_cast<int>(recv_buf.size()),
                                    0, nullptr, nullptr);
            if (recv_len <= 0) continue;

            uint8_t real_len = static_cast<uint8_t>(recv_buf[0]);
            size_t packet_len = static_cast<size_t>(real_len) + 1;
            if (packet_len > static_cast<size_t>(recv_len)) continue;

            if (handlePing(recv_buf.data(), packet_len, sHost)) continue;
            processPacket(recv_buf.data(), packet_len, memory, processor);
        }
    } else {
        // ---- TCP 模式 ----
        std::vector<char> recv_buf(g_tcp_buffer_size);
        TcpRingBuffer ring;
        std::array<char, 256> pkt_buf{};

        while (!g_exit_flag) {
            if (ring.readableCount() < g_tcp_receive_threshold) {
                int recv_len = recv(sHost, recv_buf.data(), static_cast<int>(recv_buf.size()), 0);
                if (recv_len == 0) {
                    printErr("[INFO] TCP peer closed connection.\n");
                    g_exit_flag = true;
                    break;
                }
                if (recv_len > 0) {
                    ring.append(recv_buf.data(), static_cast<size_t>(recv_len));
                }
            }

            size_t pkt_len = 0;
            while (ring.peekPacket(pkt_buf.data(), pkt_buf.size(), pkt_len)) {
                if (!handlePing(pkt_buf.data(), pkt_len, sHost)) {
                    processPacket(pkt_buf.data(), pkt_len, memory, processor);
                }
                ring.consume(pkt_len);
            }
        }
    }
}

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char* argv[]) {
    parseArgs(argc, argv);
    SetConsoleTitle("Brokenithm-Evolved-Android Server");
    printInfo();

    WSAData wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printErr("[ERROR] WSA startup failed!\n");
        return -1;
    }
    defer(WSACleanup());

    SharedMemory memory;
    if (!memory.init()) {
        printErr("[ERROR] Shared memory init failed! error: %lu\n", GetLastError());
        return -1;
    }

    SignalProcessor processor;

    if (!g_tcp_mode) {
        printErr("[INFO] Mode: UDP\n");
        SOCKET sHost = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sHost == INVALID_SOCKET) {
            printErr("[ERROR] socket() failed!\n");
            return -1;
        }
        defer(closesocket(sHost));

        socketSetTimeout(sHost, 2000);
        if (socketBind(sHost, htonl(INADDR_ANY), g_server_port) != 0) {
            printErr("[ERROR] bind() failed!\n");
            return -1;
        }

        printErr("[INFO] Waiting for device on port %d...\n", g_server_port);
        auto led_thread = std::thread(threadLEDBroadcast, sHost, &memory);
        auto input_thread = std::thread(threadInputReceive, sHost, &memory, &processor);

        while (_getwch() != L'q');
        printErr("[INFO] Exiting gracefully...\n");
        g_last_input_packet_id = 0;
        g_exit_flag = true;
        led_thread.join();
        input_thread.join();
    } else {
        printErr("[INFO] Mode: TCP\n");
        printErr("[INFO] TCP receive buffer size: %zu\n", g_tcp_buffer_size);
        printErr("[INFO] TCP receive threshold: %zu\n", g_tcp_receive_threshold);

        SOCKET sHost = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sHost == INVALID_SOCKET) {
            printErr("[ERROR] socket() failed!\n");
            return -1;
        }
        defer(closesocket(sHost));

        socketSetTimeout(sHost, 50);
        if (socketBind(sHost, htonl(INADDR_ANY), g_server_port) != 0) {
            printErr("[ERROR] bind() failed!\n");
            return -1;
        }
        listen(sHost, 10);

        while (true) {
            printErr("[INFO] Waiting for device on port %d...\n", g_server_port);
            sockaddr_in user_socket = {};
            int sock_size = sizeof(sockaddr_in);
            SOCKET acc_socket = accept(sHost, reinterpret_cast<sockaddr*>(&user_socket), &sock_size);
            if (acc_socket == INVALID_SOCKET) continue;
            defer(closesocket(acc_socket));

            char addr_buf[20] = {};
            const char* user_address = inet_ntop(AF_INET, &user_socket.sin_addr, addr_buf, 20);
            if (user_address) {
                printErr("[INFO] Device %s:%d connected.\n", user_address, user_socket.sin_port);
            }

            g_connected = true;
            g_exit_flag = false;
            auto led_thread = std::thread(threadLEDBroadcast, acc_socket, &memory);
            auto input_thread = std::thread(threadInputReceive, acc_socket, &memory, &processor);
            led_thread.join();
            input_thread.join();

            printErr("[INFO] Exiting gracefully...\n");
            g_last_input_packet_id = 0;
            g_exit_flag = true;
            g_connected = false;
        }
    }

    return 0;
}
