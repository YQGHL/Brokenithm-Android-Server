#include "signal_processor.h"
#include <algorithm>

SignalProcessor::SignalProcessor(const Config& cfg) : cfg_(cfg) {}

void SignalProcessor::reset() {
    slider_ema_.fill(0.0f);
    air_state_.fill(false);
    test_btn_ = {};
    service_btn_ = {};
    last_packet_id_ = 0;
    has_last_packet_ = false;
}

void SignalProcessor::updateButton(ButtonState& state, uint8_t raw, uint8_t& out) {
    if (raw == state.last_raw) {
        if (state.counter < cfg_.button_debounce_frames) {
            ++state.counter;
        } else {
            state.confirmed = raw;
        }
    } else {
        state.last_raw = raw;
        state.counter = 1; // 已经有一帧新值
    }
    out = state.confirmed;
}

void SignalProcessor::process(const uint8_t air_in[6], const uint8_t slider_in[32],
                              uint8_t test_in, uint8_t service_in,
                              uint32_t packet_id,
                              uint8_t air_out[6], uint8_t slider_out[32],
                              uint8_t& test_out, uint8_t& service_out) {
    // ---- 丢包检测（仅日志/状态跟踪） ----
    if (has_last_packet_ && packet_id != 0) {
        if (packet_id == last_packet_id_) {
            // 重复包：理论上Android不会发重复packetId，若出现则忽略
            return;
        }
        // 注意：UDP可能乱序，此处仅作记录，不阻断处理
    }
    last_packet_id_ = packet_id;
    has_last_packet_ = true;

    // ---- 滑条：死区 + EMA平滑 ----
    for (size_t i = 0; i < 32; ++i) {
        float val = static_cast<float>(slider_in[i]);

        // 死区：消除底噪
        if (val <= static_cast<float>(cfg_.slider_deadzone)) {
            val = 0.0f;
        }

        // EMA：y[n] = alpha * x[n] + (1-alpha) * y[n-1]
        if (cfg_.slider_ema_alpha > 0.0f) {
            slider_ema_[i] = cfg_.slider_ema_alpha * val
                           + (1.0f - cfg_.slider_ema_alpha) * slider_ema_[i];
            val = slider_ema_[i];
        }

        // 钳位到 [0,255]
        slider_out[i] = static_cast<uint8_t>(
            std::min(std::max(val, 0.0f), 255.0f));
    }

    // ---- 空中传感器：施密特触发器（滞回） ----
    for (size_t i = 0; i < 6; ++i) {
        uint8_t v = air_in[i];
        bool& st = air_state_[i];
        if (!st && v >= cfg_.air_threshold_on) {
            st = true;
        } else if (st && v <= cfg_.air_threshold_off) {
            st = false;
        }
        // 输出保留原始强度（若判定为ON），否则置0
        air_out[i] = st ? v : 0;
    }

    // ---- 按钮：防抖 ----
    updateButton(test_btn_, test_in, test_out);
    updateButton(service_btn_, service_in, service_out);
}
