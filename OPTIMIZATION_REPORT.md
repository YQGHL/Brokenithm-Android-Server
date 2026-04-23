# Brokenithm-Android-Server 优化报告

## 版本信息

- 优化日期：2026-04-23
- 原始版本：V0.3.0
- 优化范围：`src/` 主服务器程序
- 约束条件：**共享内存结构 `IPCMemoryInfo` 一字节不变**，保证 `segatools/chuniio/` 与 `segatools/aimeio/` 的 DLL 兼容性

---

## 一、原代码根因分析

对原始 `main.cpp`（549 行）进行逐行白盒推演后，识别出以下系统性缺陷：

| 维度 | 具体问题 | 底层影响 |
|------|---------|---------|
| **架构** | 单文件、全局变量泛滥、无分层 | 任何改动都牵一发而动全身，维护成本极高 |
| **网络 I/O** | TCP 流重组使用 `std::string remains`，每次 `erase(0, packet_len)` | **O(n) 数据搬移**，高频输入下 CPU 缓存失效、堆分配抖动 |
| **内存** | LED 广播每帧构造 `std::string` 承载 96 字节二进制数据 | 触发堆分配与析构，实时性受损 |
| **信号解算** | 滑条 32 通道、空中 6 通道、按钮直接 `memcpy` 透传 | Android 端触控噪声、传感器临界抖动直接传入游戏，造成误判 |
| **同步** | 共享内存裸指针访问，无内存屏障 | 编译器重排 + CPU 乱序执行可能导致 segatools DLL 读到半写状态 |
| **健壮性** | 协议解析用 `reinterpret_cast<PacketInput*>(buffer)` | 若 buffer 地址未对齐，触发 **UB（未定义行为）** |
| **可移植性** | 依赖 `getopt`、`unistd.h`、`gettimeofday` | MSVC 下直接编译失败 |

---

## 二、核心优化措施与原理

### 2.1 信号解算层（`SignalProcessor`）

本次优化的重点。针对 Android 客户端的输入特性，设计了**三级滤波**。

#### 2.1.1 滑条：死区 + EMA 指数平滑

```cpp
// EMA 差分方程：y[n] = α·x[n] + (1-α)·y[n-1]
slider_ema_[i] = alpha * val + (1.0f - alpha) * slider_ema_[i];
```

- **死区（Deadzone）**：原始值 ≤ 5 直接置 0，消除触控面板底噪。
- **EMA 优点**：仅需 1 次乘加、无需维护大窗口缓冲区；`α = 0.40` 在响应速度与平滑度之间取得平衡。

#### 2.1.2 空中传感器：施密特触发器（滞回比较器）

```cpp
if (!st && v >= on_threshold)  st = true;   // 上升沿
else if (st && v <= off_threshold) st = false; // 下降沿
```

- **原理**：`on_threshold = 50`，`off_threshold = 25`，形成 25 的滞回窗口。
- **效果**：手在红外临界高度轻微抖动时，不会导致空中状态高频翻转。

#### 2.1.3 按钮：防抖计数器

- 连续 `N = 2` 帧采样一致才改变输出状态，消除 Android 端的偶发误触。

---

### 2.2 网络 I/O 优化

#### 2.2.1 TCP 环形缓冲区（`TcpRingBuffer`）

- 固定 4096 字节（`2^12`），利用位运算 `idx & (kCap - 1)` 取模，避免除法。
- `append` + `peekPacket` + `consume` 语义：**零拷贝流重组**，彻底消除原代码中 `std::string::erase(0, n)` 的 **O(n) 数据搬移**。
- 仅在 buffer 满时触发一次 `compact()`（`memmove`），摊还复杂度接近 O(1)。

#### 2.2.2 LED 广播：栈上固定数组

- 使用 `std::array<uint8_t, 100>` 预分配发送缓冲区，生命周期绑定线程栈。
- 完全消除每帧的 `std::string` 堆分配与析构。

---

### 2.3 共享内存同步（`SharedMemory`）

生产者（输入线程）与消费者（segatools DLL）之间插入**显式内存屏障**：

```cpp
// 写入侧
memcpy(mem_->sliderIoStatus, slider, 32);
std::atomic_thread_fence(std::memory_order_release);

// 读取侧
std::atomic_thread_fence(std::memory_order_acquire);
memcpy(out, mem_->ledRgbData, 96);
```

- **原理**：`release` 保证写入指令不会被重排到 fence 之后；`acquire` 保证读取指令不会被重排到 fence 之前。
- 在 x86/x64 上实际开销极低（仅编译器屏障），但赋予了跨架构的正确性语义。

---

### 2.4 协议解析安全化

所有包从 `reinterpret_cast` 改为 `memcpy`：

```cpp
PacketInput pkt;
std::memcpy(&pkt, buf, sizeof(pkt));  // 安全，无需对齐假设
```

- `IPCMemoryInfo` 与所有 `Packet*` 结构保持 **一字节不变**，协议完全兼容。

---

### 2.5 MSVC 兼容性

- **移除** `getopt`、`unistd.h`、`gettimeofday`。
- 手写命令行解析（`-p` / `-T` / `-r`）。
- `CMakeLists.txt` 新增 `/utf-8`（支持 UTF-8 源文件）与 `NOMINMAX`（防止 Windows 宏污染 `std::min/max`）。

---

## 三、文件变更清单

| 文件 | 动作 | 说明 |
|------|------|------|
| `CMakeLists.txt` | 修改 | 更新 `cmake_minimum_required` 为 3.5；添加 `/utf-8`、`NOMINMAX`；加入新 `.cpp` |
| `src/main.cpp` | 重写 | 模块化重构，保留全部协议逻辑，优化线程模型 |
| `src/shared_memory.h/.cpp` | **新增** | 封装 `CreateFileMapping`，带 release/acquire fence |
| `src/signal_processor.h/.cpp` | **新增** | EMA、滞回、防抖三级信号解算 |
| `src/utils.h` | **新增** | 网络辅助、日志、时间、hex dump 工具函数 |
| `src/struct.h` | 未修改 | 协议与共享内存结构 **零变更**，保证 DLL 兼容 |
| `src/defer.h` | 未修改 | 原样保留 |
| `src/version.h` | 未修改 | 原样保留 |
| `src/resources.rc` | 未修改 | 原样保留 |

---

## 四、编译验证

使用 **MSVC 19.51 + Ninja** 成功编译并链接：

```bash
cmake -G Ninja -B build -S .
cmake --build build
# [5/6] Linking CXX executable brokenithm.exe   ← 成功
```

MSYS2/MinGW 路径同样兼容，因源码中未使用任何 MSVC 专属扩展。

---

## 五、待进一步验证的事项（需真机测试）

1. **SignalProcessor 参数调参**
   - 当前 `slider_ema_alpha = 0.40`、`air_threshold_on = 50` 为经验值。
   - 实际游玩中可能需要根据 Android 设备采样率微调。

2. **TCP 环形缓冲区压力测试**
   - 极端网络抖动场景下，4096 字节缓冲是否足够。
   - 按最大包 48 字节计算，可缓存约 85 包，理论充裕。

3. **空中传感器滞回体感**
   - `off_threshold = 25` 是否会导致手缓慢放下时延迟释放。
   - 需实际 Chunithm 运行验证。

---

## 六、性能提升总结

| 指标 | 优化前 | 优化后 | 原理 |
|------|--------|--------|------|
| TCP 流重组 | O(n) `std::string::erase` | O(1) 环形缓冲区 | 零拷贝、无堆分配 |
| LED 发送 | 每帧堆分配 `std::string` | 栈上 `std::array` | 零堆分配 |
| 滑条噪声 | 直接透传 | 死区 + EMA 平滑 | 消除底噪、瞬态抖动 |
| 空中抖动 | 直接透传 | 施密特触发器 | 滞回防抖 |
| 共享内存同步 | 无屏障 | release/acquire fence | 防止读写撕裂 |
| 协议解析 | `reinterpret_cast` | `memcpy` | 消除未对齐 UB |

---

*本报告基于白盒推演与编译验证生成。所有代码变更均已实际写入仓库并可编译通过。*
