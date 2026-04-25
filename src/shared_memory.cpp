#include "shared_memory.h"

SharedMemory::SharedMemory() = default;

SharedMemory::~SharedMemory() {
    if (mem_) {
        UnmapViewOfFile(mem_);
    }
    if (hMapFile_) {
        CloseHandle(hMapFile_);
    }
}

bool SharedMemory::init() {
    constexpr const char* kMemName = "Local\\BROKENITHM_SHARED_BUFFER";
    hMapFile_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, kMemName);
    if (!hMapFile_) {
        hMapFile_ = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, 1024, kMemName);
        if (!hMapFile_) {
            return false;
        }
    }

    mem_ = reinterpret_cast<IPCMemoryInfo*>(
        MapViewOfFileEx(hMapFile_, FILE_MAP_ALL_ACCESS, 0, 0, 1024, nullptr));
    if (!mem_) {
        return false;
    }

    std::memset(mem_, 0, sizeof(IPCMemoryInfo));
    return true;
}

bool SharedMemory::isReady() const {
    return mem_ != nullptr;
}

void SharedMemory::writeInput(const uint8_t air[6], const uint8_t slider[32],
                              uint8_t test, uint8_t service) {
    if (!mem_) return;
    std::memcpy(mem_->airIoStatus, air, 6);
    std::memcpy(mem_->sliderIoStatus, slider, 32);
    mem_->testBtn = test;
    mem_->serviceBtn = service;
    std::atomic_thread_fence(std::memory_order_release);
}

void SharedMemory::writeCoin(uint8_t coin) {
    if (!mem_) return;
    mem_->coinInsertion = coin;
    std::atomic_thread_fence(std::memory_order_release);
}

void SharedMemory::writeCardRead(uint8_t card) {
    if (!mem_) return;
    mem_->cardRead = card;
    std::atomic_thread_fence(std::memory_order_release);
}

void SharedMemory::writeRemoteCard(uint8_t read, uint8_t type, const uint8_t id[10]) {
    if (!mem_) return;
    mem_->remoteCardRead = read;
    mem_->remoteCardType = type;
    std::memcpy(mem_->remoteCardId, id, 10);
    std::atomic_thread_fence(std::memory_order_release);
}

void SharedMemory::readLed(uint8_t out[32 * 3]) const {
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!mem_ || !out) return;
    std::memcpy(out, mem_->ledRgbData, 32 * 3);
}
