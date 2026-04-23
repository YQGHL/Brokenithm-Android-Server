#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#include "struct.h"

class SharedMemory {
public:
    SharedMemory();
    ~SharedMemory();

    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    bool init();
    bool isReady() const;

    // 写入输入数据（release语义，确保消费者看到完整更新）
    void writeInput(const uint8_t air[6], const uint8_t slider[32], uint8_t test, uint8_t service);
    void writeCoin(uint8_t coin);
    void writeCardRead(uint8_t card);
    void writeRemoteCard(uint8_t read, uint8_t type, const uint8_t id[10]);

    // 读取LED数据（acquire语义）
    void readLed(uint8_t out[32 * 3]) const;

    // 直接原始访问（谨慎使用，主要用于初始化清零）
    IPCMemoryInfo* raw() { return mem_; }
    const IPCMemoryInfo* raw() const { return mem_; }

private:
    HANDLE hMapFile_ = nullptr;
    IPCMemoryInfo* mem_ = nullptr;
};
