#pragma once

#include <cstdint>
#include <array>

// 信号解算器：对Android端传来的原始传感器数据进行滤波、平滑与防抖处理。
// 设计约束：
// - 滑条为32通道8bit压力值，需消除触控噪声与瞬时抖动。
// - 空中传感器为6通道8bit值，需防止临界抖动（滞回）。
// - 按钮（test/service）需防抖。
// - 所有处理必须低延迟（无大窗缓冲区），单帧内完成。
class SignalProcessor {
public:
    struct Config {
        // 滑条EMA平滑系数 (0, 1]，越大响应越快；<=0 关闭
        float slider_ema_alpha = 0.40f;
        // 滑条死区：低于此值的原始输入直接视为0
        uint8_t slider_deadzone = 5;
        // 空中传感器滞回触发阈值
        uint8_t air_threshold_on = 50;
        // 空中传感器滞回释放阈值（必须 < air_threshold_on）
        uint8_t air_threshold_off = 25;
        // 按钮防抖：连续N帧采样一致才改变输出状态
        uint8_t button_debounce_frames = 2;
    };

    explicit SignalProcessor(const Config& cfg = Config{});

    // 处理一帧输入，输出已解算的值。
    // packet_id 用于丢包检测；为0时跳过检测。
    void process(const uint8_t air_in[6], const uint8_t slider_in[32],
                 uint8_t test_in, uint8_t service_in,
                 uint32_t packet_id,
                 uint8_t air_out[6], uint8_t slider_out[32],
                 uint8_t& test_out, uint8_t& service_out);

    // 清空内部状态（断连或超时后调用）
    void reset();

private:
    struct ButtonState {
        uint8_t last_raw = 0;
        uint8_t confirmed = 0;
        uint8_t counter = 0;
    };

    void updateButton(ButtonState& state, uint8_t raw, uint8_t& out);

    Config cfg_;
    std::array<float, 32> slider_ema_{};
    std::array<bool, 6> air_state_{};
    ButtonState test_btn_;
    ButtonState service_btn_;
    uint32_t last_packet_id_ = 0;
    bool has_last_packet_ = false;
};
