## Brokenithm-Android-Server

[Brokenithm-Android](https://github.com/tindy2013/Brokenithm-Android) 的 Windows 服务端。

本分支是对原版 [esterTion/Brokenithm-Android-Server](https://github.com/esterTion/Brokenithm-Android-Server) 的**性能导向重构**。在保持完整协议及 `segatools` DLL 兼容性的同时，显著提升了实时性能、信号质量和代码可维护性。

---

# 构建

```bash
# 仅在 MSYS2 + MinGW 环境下测试通过
cmake -G "MSYS Makefiles" .
make
```

同时原生支持 MSVC 编译（差异见下文）。

---

# 与原版项目的差异

## 1. 架构：从单体到分层模块

| 方面 | 原版 | 本分支 |
|------|------|--------|
| 文件结构 | 单一 `main.cpp`（549 行）含 20+ 个全局变量 | 模块化：`main.cpp` + `shared_memory.cpp` + `signal_processor.cpp` + `utils.h` |
| 可维护性 | 任何改动都可能影响 I/O、协议和内存等多个层面 | 关注点清晰分离：网络 I/O、信号处理与共享内存各自隔离 |

**新增文件：**
- `src/shared_memory.h/.cpp` — 封装 Windows 命名共享内存（`Local\BROKENITHM_SHARED_BUFFER`）。
- `src/signal_processor.h/.cpp` — 专用信号滤波与消抖层。
- `src/utils.h` — 网络辅助、日志、时间格式化及十六进制转储工具。

## 2. 信号处理：三级滤波

原版项目直接将 Android 传感器的数据 `memcpy` 到共享内存，将触摸噪声和抖动原样传递给游戏。本分支引入了**可配置的信号处理器**：

### 滑条（32 通道）：死区 + EMA 平滑
- **死区**：原始值 ≤ 5 强制归零，消除触摸面板底噪。
- **EMA（指数移动平均）**：`y[n] = α·x[n] + (1-α)·y[n-1]`，其中 `α = 0.40`。每通道每帧仅需一次乘加运算，无需大型滑动窗口缓冲区。

### 空中传感器（6 通道）：施密特触发器（迟滞）
- **上升阈值**：50
- **下降阈值**：25
- **效果**：当手悬停在红外临界高度时，微小的抖动不再导致空中状态的高频翻转。

### 按钮（测试 / 服务）：消抖计数器
- 输出仅在**连续 2 帧**原始输入一致后才发生变化，消除 Android 端偶发的误触发。

## 3. 网络 I/O 优化

### TCP 流重组：无锁环形缓冲区
原版代码使用 `std::string remains` 进行 TCP 流缓冲，每收到一个数据包就调用 `remains.erase(0, packet_len)`。这是 **O(n) 数据搬移**，在高频输入下会导致堆分配抖动。

本分支替换为**固定 4096 字节环形缓冲区**（`TcpRingBuffer`）：
- 使用位掩码索引（`idx & (kCap - 1)`）替代除法/取模。
- 提供 `append` → `peekPacket` → `consume` 语义，实现**零拷贝流重组**。
- `compact()`（通过 `memmove`）仅在缓冲区接近满时触发；摊还复杂度接近 **O(1)**。

### LED 广播：基于栈的固定数组
原版代码每帧构造一个新的 `std::string` 来承载 96 字节的 RGB 数据。本分支预分配 `std::array<uint8_t, 100>` 于线程栈上，完全消除了逐帧的堆分配与销毁。

## 4. 共享内存同步：显式内存屏障

原版代码对共享内存进行裸指针写入，没有任何同步原语。在编译器重排和 CPU 乱序执行下，`segatools` DLL 可能读到半写状态。

本分支插入了 **Release-Acquire 语义**：

```cpp
// 生产者（输入线程）
memcpy(mem_->sliderIoStatus, slider, 32);
std::atomic_thread_fence(std::memory_order_release);

// 消费者（LED 广播 / segatools）
std::atomic_thread_fence(std::memory_order_acquire);
memcpy(out, mem_->ledRgbData, 96);
```

- `release` 保证屏障之前的所有写入在屏障本身之前可见。
- `acquire` 保证屏障之后的所有读取能观察到完整的先前写入。
- 在 x86/x64 上运行时开销可忽略（主要作为编译器屏障），但提供了跨架构的正确语义。

## 5. 协议解析安全性

原版所有数据包解析均使用 `reinterpret_cast<PacketType*>(buffer)`，若接收缓冲区未充分对齐，此为**未定义行为（UB）**。

本分支对所有数据包类型使用**安全的 `memcpy`**：

```cpp
PacketInput pkt;
std::memcpy(&pkt, buf, sizeof(pkt));  // 不依赖对齐假设
```

`IPCMemoryInfo` 结构体及所有 `Packet*` 结构体与原版保持**位级一致**，确保与现有 Android 客户端和 segatools DLL 的完全向后兼容。

## 6. 原生 MSVC 兼容性

| 依赖 | 原版 | 本分支 |
|------|------|--------|
| `getopt` / `unistd.h` | 必需（POSIX） | **已移除** — 手写参数解析器（`-p`、`-T`、`-r`） |
| `gettimeofday` | 用于毫秒时间戳 | **已移除** — 替换为 `localtime_s` + 标准 `time` |
| `std::min/max` | 可能与 Windows `min/max` 宏冲突 | **已保护** — `CMakeLists.txt` 定义 `NOMINMAX` |
| 源文件字符集 | 默认 ANSI | **显式 `/utf-8`** 编译器选项，支持 Unicode 字面量 |

## 7. DLL 兼容性保持不变

> **约束**：`IPCMemoryInfo` 布局及 `Local\BROKENITHM_SHARED_BUFFER` 名称**逐字节不变**。
>
> `segatools/chuniio/chuniio.c` 和 `segatools/aimeio/aimeio.c` 钩子无需重新编译即可继续工作。

---

# 性能概览

| 指标 | 原版 | 优化后 | 原理 |
|------|------|--------|------|
| TCP 流重组 | O(n) `std::string::erase` | O(1) 环形缓冲区 | 零拷贝，无堆分配 |
| LED 广播 | 逐帧 `std::string` 堆分配 | 栈上 `std::array` | 零堆分配 |
| 滑条噪声 | 直通 | 死区 + EMA | 消除底噪与瞬态抖动 |
| 空中抖动 | 直通 | 施密特触发器 | 迟滞消抖 |
| 共享内存同步 | 无屏障 | Release/Acquire 屏障 | 防止撕裂读写 |
| 协议解析 | `reinterpret_cast`（UB 风险） | `memcpy` | 安全的非对齐访问 |

---

## 更新日志（2026-04-25）

在 2026-04-23 重构的基础上，额外应用了以下优化：

### S-1：LED 广播更精细的定时
- LED 广播线程中的 `Sleep(10)` → `Sleep(1)`。
- 保留原有的跳频保活机制（每 50 个周期发送一次未变化的 LED 数据）。

### S-2：`readLed()` 中正确的内存序
- 将 `std::atomic_thread_fence(std::memory_order_acquire)` 移至 `readLed()` 的**最顶部**，在空指针守卫检查之前。
- **理由**：在旧代码中，编译器可能将 `if (!mem_ || !out) return;` 守卫重排到屏障之前，导致空指针检查读到可能过期的值。将屏障置于最前可确保后续所有读取（包括 `mem_`）观察到完整的先前写入。

### S-3：SignalProcessor 命令行可配置性
- 新增命令行标志，支持运行时调优而无需重新编译：
  - `--slider-alpha=FLOAT`
  - `--slider-deadzone=N`
  - `--air-threshold-on=N`
  - `--air-threshold-off=N`
  - `--button-debounce-frames=N`
  - `-h` / `--help`
- 默认值与原硬编码值完全一致（`α=0.40`，deadzone=5，on=50，off=25，debounce=2）。

### S-4：TCP 连接冷却
- 在 TCP accept 循环的 `input_thread.join()` 之后新增 `Sleep(100)`。
- 新增架构注释说明单客户端 FIFO 设计。
- 防止快速断开/重连时的连接风暴问题。

### S-5：IPCMemoryInfo 编译期大小校验
- 在 `segatools/chuniio/chuniio.c` 中新增 `_Static_assert(sizeof(struct chuni_io_ipc_memory_info) == 143, ...)`。
- 在 `segatools/aimeio/aimeio.c` 中新增 `_Static_assert(sizeof(struct aime_io_ipc_memory_info) == 143, ...)`。
- 若共享内存布局在服务端与 DLL 之间发生偏离，构建将在编译期失败，而非在运行时静默损坏游戏内存。

---

# 许可证

与原版项目相同。
