## Brokenithm-Android-Server (Evolved)

The Windows server of [Brokenithm-Android](https://github.com/tindy2013/Brokenithm-Android).

This fork is a **performance-focused refactoring** of the original [esterTion/Brokenithm-Android-Server](https://github.com/esterTion/Brokenithm-Android-Server). It keeps full protocol and `segatools` DLL compatibility while significantly improving real-time performance, signal quality, and code maintainability.

---

# Build

```bash
# Only tested with MSYS2 with MinGW
cmake -G "MSYS Makefiles" .
make
```

MSVC is also supported natively (see differences below).

---

# Differences from the Original Project

## 1. Architecture: From Monolith to Layered Modules

| Aspect | Original | This Fork |
|--------|----------|-----------|
| File structure | Single `main.cpp` (549 lines) with 20+ global variables | Modular: `main.cpp` + `shared_memory.cpp` + `signal_processor.cpp` + `utils.h` |
| Maintainability | Any change risks side effects across I/O, protocol, and memory | Clear separation of concerns: network I / O, signal processing, and shared memory are isolated |

**New files:**
- `src/shared_memory.h/.cpp` — Encapsulates Windows named shared memory (`Local\BROKENITHM_SHARED_BUFFER`).
- `src/signal_processor.h/.cpp` — Dedicated signal filtering and debouncing layer.
- `src/utils.h` — Network helpers, logging, time formatting, and hex dump utilities.

## 2. Signal Processing: Three-Stage Filtering

The original project directly `memcpy`'d Android sensor data into shared memory, forwarding touch noise and jitter straight to the game. This fork introduces a **configurable signal processor**:

### Slider (32 channels): Deadzone + EMA Smoothing
- **Deadzone**: Raw values ≤ 5 are forced to 0, eliminating touch panel底噪.
- **EMA (Exponential Moving Average)**: `y[n] = α·x[n] + (1-α)·y[n-1]` with `α = 0.40`. Requires only one multiply-add per channel per frame, with no large window buffer.

### Air Sensors (6 channels): Schmitt Trigger (Hysteresis)
- **Rising threshold**: 50
- **Falling threshold**: 25
- **Effect**: When a hand hovers at the critical infrared height, minor jitter no longer causes high-frequency toggling of the air state.

### Buttons (Test / Service): Debounce Counter
- Output only changes after **2 consecutive frames** of identical raw input, eliminating sporadic false triggers from the Android side.

## 3. Network I/O Optimizations

### TCP Stream Reassembly: Lock-Free Ring Buffer
The original code used `std::string remains` for TCP stream buffering, calling `remains.erase(0, packet_len)` for every packet. This is **O(n) data movement** and causes heap allocation jitter under high-frequency input.

This fork replaces it with a **fixed 4096-byte ring buffer** (`TcpRingBuffer`):
- Uses bit-mask indexing (`idx & (kCap - 1)`) instead of division/modulo.
- Provides `append` → `peekPacket` → `consume` semantics for **zero-copy stream reassembly**.
- `compact()` (via `memmove`) is only triggered when the buffer is nearly full; amortized complexity approaches **O(1)**.

### LED Broadcasting: Stack-Based Fixed Arrays
The original code constructed a new `std::string` on every frame to carry 96 bytes of RGB data. This fork uses `std::array<uint8_t, 100>` pre-allocated on the thread stack, completely eliminating per-frame heap allocation and destruction.

## 4. Shared Memory Synchronization: Explicit Memory Fences

The original code performed naked pointer writes to shared memory with no synchronization primitives. Under compiler reordering and CPU out-of-order execution, `segatools` DLLs could potentially read half-written states.

This fork inserts **Release-Acquire semantics**:

```cpp
// Producer (input thread)
memcpy(mem_->sliderIoStatus, slider, 32);
std::atomic_thread_fence(std::memory_order_release);

// Consumer (LED broadcast / segatools)
std::atomic_thread_fence(std::memory_order_acquire);
memcpy(out, mem_->ledRgbData, 96);
```

- `release` guarantees that all writes before the fence are visible before the fence itself.
- `acquire` guarantees that all reads after the fence observe the complete prior writes.
- On x86/x64 the runtime cost is negligible (primarily a compiler barrier), but it provides correct semantics across architectures.

## 5. Protocol Parsing Safety

All packet parsing in the original used `reinterpret_cast<PacketType*>(buffer)`, which is **Undefined Behavior (UB)** if the receive buffer is not sufficiently aligned.

This fork uses **safe `memcpy`** for every packet type:

```cpp
PacketInput pkt;
std::memcpy(&pkt, buf, sizeof(pkt));  // No alignment assumptions
```

The `IPCMemoryInfo` structure and all `Packet*` structs remain **bit-identical** to the original, ensuring full backward compatibility with existing Android clients and segatools DLLs.

## 6. Native MSVC Compatibility

| Dependency | Original | This Fork |
|------------|----------|-----------|
| `getopt` / `unistd.h` | Required (POSIX) | **Removed** — hand-written argument parser (`-p`, `-T`, `-r`) |
| `gettimeofday` | Used for millisecond timestamps | **Removed** — replaced with `localtime_s` + standard `time` |
| `std::min/max` | Could conflict with Windows `min/max` macros | **Protected** — `CMakeLists.txt` defines `NOMINMAX` |
| Source charset | Default ANSI | **Explicit `/utf-8`** compiler flag for Unicode literals |

## 7. DLL Compatibility Preserved

> **Constraint**: `IPCMemoryInfo` layout and `Local\BROKENITHM_SHARED_BUFFER` name are **unchanged byte-for-byte**.
>
> The `segatools/chuniio/chuniio.c` and `segatools/aimeio/aimeio.c` hooks continue to work without recompilation.

---

# Performance Summary

| Metric | Original | Optimized | Principle |
|--------|----------|-----------|-----------|
| TCP stream reassembly | O(n) `std::string::erase` | O(1) ring buffer | Zero-copy, no heap allocation |
| LED broadcast | Per-frame `std::string` heap alloc | Stack `std::array` | Zero heap allocation |
| Slider noise | Direct passthrough | Deadzone + EMA | Eliminates底噪 and transient jitter |
| Air jitter | Direct passthrough | Schmitt trigger | Hysteresis debounce |
| Shared memory sync | No barrier | Release/acquire fence | Prevents torn reads/writes |
| Protocol parsing | `reinterpret_cast` (UB risk) | `memcpy` | Safe unaligned access |

---

。

---

## Updates (2026-04-25)

On top of the 2026-04-23 refactoring, the following additional optimizations were applied:

### S-1: LED Broadcast Finer Timing
- `Sleep(10)` → `Sleep(1)` in the LED broadcast thread.
- The existing skip-count keepalive mechanism (send unchanged LED every 50 cycles) is preserved.

### S-2: Correct Memory Ordering in `readLed()`
- Moved `std::atomic_thread_fence(std::memory_order_acquire)` to the **very top** of `readLed()`, before the null-guard check.
- **Rationale**: In the old code, the compiler could reorder the `if (!mem_ || !out) return;` guard ahead of the fence, causing the null-pointer check to read a potentially stale value. Placing the fence first ensures all subsequent reads (including `mem_`) observe the complete prior writes.

### S-3: SignalProcessor CLI Configurability
- Added command-line flags for runtime tuning without recompilation:
  - `--slider-alpha=FLOAT`
  - `--slider-deadzone=N`
  - `--air-threshold-on=N`
  - `--air-threshold-off=N`
  - `--button-debounce-frames=N`
  - `-h` / `--help`
- Default values remain identical to the hardcoded originals (`α=0.40`, deadzone=5, on=50, off=25, debounce=2).

### S-4: TCP Connection Cooldown
- Added `Sleep(100)` after `input_thread.join()` in the TCP accept loop.
- Added an architectural comment documenting the single-client FIFO design.
- Prevents connection-storm issues on rapid disconnect/reconnect.

### S-5: IPCMemoryInfo Compile-Time Size Verification
- Added `_Static_assert(sizeof(struct chuni_io_ipc_memory_info) == 143, ...)` in `segatools/chuniio/chuniio.c`.
- Added `_Static_assert(sizeof(struct aime_io_ipc_memory_info) == 143, ...)` in `segatools/aimeio/aimeio.c`.
- If the shared-memory layout ever diverges between the server and the DLLs, the build will fail at compile time rather than silently corrupting game memory at runtime.

---

# License

Same as the original project.
