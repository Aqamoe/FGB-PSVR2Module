#include "psvr2_protocol.hpp"

#include <cmath>
#include <cstring>

namespace psvr2 {

namespace {

// 解析缓冲里保留的上限：远大于单帧最大值，防止异常 len 把内存撑爆。
constexpr std::size_t kBufferLimit = kMaxDataBytes + kHeaderBytes;

float Clamp01(float value) {
    if (!(value > 0.0f)) {
        return 0.0f;
    }
    return value > 1.0f ? 1.0f : value;
}

} // namespace

std::uint64_t ReconnectDelayNs(std::uint32_t attempt) {
    if (attempt == 0) {
        return kBackoffBaseNs;
    }
    const std::uint32_t shift = attempt - 1 < 5 ? attempt - 1 : 5; // 2^5 * 0.5s = 16s，再被封顶
    std::uint64_t delay = kBackoffBaseNs << shift;
    if (delay > kBackoffCapNs || delay == 0) {
        delay = kBackoffCapNs;
    }
    return delay;
}

std::uint16_t ReadU16Le(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1] << 8);
}

std::int32_t ReadI32Le(const std::uint8_t* p) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) |
                                     (static_cast<std::uint32_t>(p[1]) << 8) |
                                     (static_cast<std::uint32_t>(p[2]) << 16) |
                                     (static_cast<std::uint32_t>(p[3]) << 24));
}

float ReadFloatLe(const std::uint8_t* p) {
    const std::uint32_t bits = static_cast<std::uint32_t>(p[0]) |
                               (static_cast<std::uint32_t>(p[1]) << 8) |
                               (static_cast<std::uint32_t>(p[2]) << 16) |
                               (static_cast<std::uint32_t>(p[3]) << 24);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void WriteU16Le(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}

void WriteI32Le(std::uint8_t* p, std::int32_t v) {
    const std::uint32_t raw = static_cast<std::uint32_t>(v);
    p[0] = static_cast<std::uint8_t>(raw & 0xFFu);
    p[1] = static_cast<std::uint8_t>((raw >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((raw >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>(raw >> 24);
}

void WriteFloatLe(std::uint8_t* p, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    p[0] = static_cast<std::uint8_t>(bits & 0xFFu);
    p[1] = static_cast<std::uint8_t>((bits >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((bits >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>(bits >> 24);
}

bool IsFinite(float v) {
    return std::isfinite(v);
}

namespace {

// 帧头：u16 type + 2 字节填充 + i32 data_len。填充字节上游不初始化，这里统一写 0；
// 接收侧按 C# 布局跳过填充，不校验其值。
std::vector<std::uint8_t> BuildFrame(std::uint16_t type, const std::uint8_t* data,
                                     std::size_t data_len) {
    std::vector<std::uint8_t> frame(kHeaderBytes + data_len, 0);
    WriteU16Le(frame.data(), type);
    WriteI32Le(frame.data() + 4, static_cast<std::int32_t>(data_len));
    if (data != nullptr && data_len > 0) {
        std::memcpy(frame.data() + kHeaderBytes, data, data_len);
    }
    return frame;
}

} // namespace

std::vector<std::uint8_t> BuildHandshakeRequest(std::uint32_t process_id) {
    // 负载 8 字节：u16 ipcVersion + 2 填充 + u32 processId（C# 默认对齐布局）。
    std::uint8_t payload[kHandshakeRequestBytes] = {0};
    WriteU16Le(payload, kIpcVersion);
    payload[4] = static_cast<std::uint8_t>(process_id & 0xFFu);
    payload[5] = static_cast<std::uint8_t>((process_id >> 8) & 0xFFu);
    payload[6] = static_cast<std::uint8_t>((process_id >> 16) & 0xFFu);
    payload[7] = static_cast<std::uint8_t>(process_id >> 24);
    return BuildFrame(kCmdClientRequestHandshake, payload, kHandshakeRequestBytes);
}

std::vector<std::uint8_t> BuildGazeDataRequest() {
    return BuildFrame(kCmdClientRequestGazeData, nullptr, 0);
}

std::vector<std::uint8_t> BuildPingRequest() {
    return BuildFrame(kCmdClientPing, nullptr, 0);
}

bool DecodeHeader(const std::uint8_t* p, Header& out) {
    out.type = ReadU16Le(p);
    out.data_len = ReadI32Le(p + 4);
    return true;
}

bool MessageFramer::Push(const std::uint8_t* data, std::size_t size, std::vector<Message>& out,
                         std::string& error) {
    buffer_.insert(buffer_.end(), data, data + size);
    if (buffer_.size() > kBufferLimit) {
        error = "TCP 流里攒了 " + std::to_string(buffer_.size()) +
                " 字节仍凑不出合法帧，判定流错位";
        buffer_.clear();
        return false;
    }
    for (;;) {
        if (buffer_.size() < kHeaderBytes) {
            return true;
        }
        Header header{};
        DecodeHeader(buffer_.data(), header);
        if (header.data_len < 0 || header.data_len > kMaxDataBytes) {
            error = "帧头 data_len 异常（" + std::to_string(header.data_len) +
                    "），TCP 流已错位";
            buffer_.clear();
            return false;
        }
        const std::size_t total = kHeaderBytes + static_cast<std::size_t>(header.data_len);
        if (buffer_.size() < total) {
            return true; // 半包，等下次 recv
        }
        Message message;
        message.type = header.type;
        message.payload.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes),
                               buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(total));
        out.push_back(std::move(message));
    }
}

bool ParseHandshakeResult(const std::uint8_t* data, std::size_t len, HandshakeResult& out,
                          std::string& error) {
    if (len != kHandshakeResultBytes) {
        error = "握手结果长度 " + std::to_string(len) + " 与布局 (4 字节) 不符";
        return false;
    }
    // u8 result + 1 填充 + u16 ipcVersion（偏移 2）。
    out.code = data[0];
    out.ipc_version = ReadU16Le(data + 2);
    return true;
}

namespace {

EyeSample ParseEye(const std::uint8_t* p, bool version2) {
    // GazeEyeResult / GazeEyeResult2 是 Pack=1 的小端布局，字段偏移固定、无填充。
    EyeSample eye;
    eye.origin_valid = p[0] != 0;
    eye.origin_mm[0] = ReadFloatLe(p + 1);
    eye.origin_mm[1] = ReadFloatLe(p + 5);
    eye.origin_mm[2] = ReadFloatLe(p + 9);
    eye.dir_valid = p[13] != 0;
    eye.dir[0] = ReadFloatLe(p + 14);
    eye.dir[1] = ReadFloatLe(p + 18);
    eye.dir[2] = ReadFloatLe(p + 22);
    eye.pupil_valid = p[26] != 0;
    eye.pupil_mm = ReadFloatLe(p + 27);
    eye.blink_valid = p[31] != 0;
    eye.blink = p[32] != 0;
    if (version2) {
        eye.open_enabled = p[33] != 0;
        eye.open = ReadFloatLe(p + 34);
    }
    return eye;
}

} // namespace

bool ParseGazeResult(const std::uint8_t* data, std::size_t len, std::uint16_t ipc_version,
                     GazeFrame& out, std::string& error) {
    const bool version2 = ipc_version != 1; // 上游：仅显式等于 1 才走旧布局
    const std::size_t expect = version2 ? kGazeResultBytesV2 : kGazeResultBytesV1;
    if (len != expect) {
        error = "眼追回包长度 " + std::to_string(len) + " 与 IPC v" +
                std::to_string(ipc_version) + " 布局 (" + std::to_string(expect) + " 字节) 不符";
        return false;
    }
    const std::size_t eye_bytes = version2 ? kEyeBytesV2 : kEyeBytesV1;
    out.left = ParseEye(data, version2);
    out.right = ParseEye(data + eye_bytes, version2);
    return true;
}

float OpennessFilter::Filter(float value) {
    if (filled_ < 14) {
        // 首值种子：整窗填同一个数，避免启动阶段被 0 拖低（上游 C# 实现从 0 起步）。
        for (std::size_t i = 0; i < 14; ++i) {
            samples_[i] = value;
        }
        filled_ = 14;
        next_ = 1 % 14;
        return value;
    }
    samples_[next_] = value;
    next_ = (next_ + 1) % 14;
    float sum = 0.0f;
    for (std::size_t i = 0; i < 14; ++i) {
        sum += samples_[i];
    }
    return sum / 14.0f;
}

void OpennessFilter::Reset() {
    filled_ = 0;
    next_ = 0;
}

namespace {

void SanitizeEye(const EyeSample& in, OpennessFilter& filter, EyeOut& out) {
    // 睁眼度：blink_valid 才有；v2 的 open_enabled 优先用连续 open 值。
    if (in.blink_valid) {
        if (in.open_enabled) {
            if (!IsFinite(in.open)) {
                out.rejected += "open 值非有限，睁眼度丢弃；";
            } else {
                out.openness = filter.Filter(Clamp01(in.open));
                out.has_openness = true;
            }
        } else {
            out.openness = filter.Filter(in.blink ? 0.0f : 1.0f);
            out.has_openness = true;
        }
    }

    if (in.pupil_valid) {
        // 单位不换算：上游字段名 pupilDiaMm 是毫米，ABI 的 pupil_diameter_mm 同为毫米。
        // 瞳孔直径物理范围约 1..8mm，>20 或 <=0 只可能是脏数据。
        if (!IsFinite(in.pupil_mm) || in.pupil_mm <= 0.0f || in.pupil_mm > 20.0f) {
            out.rejected += "瞳孔直径越界或非有限，丢弃；";
        } else {
            out.pupil_mm = in.pupil_mm;
            out.has_pupil = true;
        }
    }

    if (in.dir_valid) {
        const bool finite = IsFinite(in.dir[0]) && IsFinite(in.dir[1]) && IsFinite(in.dir[2]);
        const float length =
            finite ? std::sqrt(in.dir[0] * in.dir[0] + in.dir[1] * in.dir[1] +
                               in.dir[2] * in.dir[2])
                   : 0.0f;
        if (!finite || !(length > 1e-6f) || !IsFinite(length)) {
            out.rejected += "注视方向非有限或长度为零，丢弃；";
        } else {
            // 极化依据：上游 VRCFT 只取 (x, y) 且 FlipX（x 取反），说明 PSVR2 空间的 x
            // 与 Unity/VRCFT 相反；Unity 是 +Z 前，OpenXR 是 -Z 前，故映射 (-x, y, -z)。
            // z 轴假设未经驱动端一手确认（见 docs/protocol.md），配一道 fail-safe：
            // 映射后仍不朝前（z >= -0.05）就判无效，宁可不输出也不伪造方向。
            const float mapped[3] = {-in.dir[0] / length, in.dir[1] / length,
                                     -in.dir[2] / length};
            if (!IsFinite(mapped[0]) || !IsFinite(mapped[1]) || !IsFinite(mapped[2]) ||
                mapped[2] > -0.05f) {
                out.rejected += "注视方向映射后不朝 -Z（坐标系极化可疑），丢弃；";
            } else {
                out.gaze_dir[0] = mapped[0];
                out.gaze_dir[1] = mapped[1];
                out.gaze_dir[2] = mapped[2];
                out.has_gaze = true;
            }
        }
    }
}

} // namespace

OutputFrame Packetizer::Build(const GazeFrame& frame) {
    OutputFrame out;
    SanitizeEye(frame.left, left_filter_, out.left);
    SanitizeEye(frame.right, right_filter_, out.right);
    return out;
}

void WriteFrame(const OutputFrame& frame, FgbFrameSlot* slot) {
    if (slot == nullptr) {
        return;
    }
    FgbEyeAttributes* eyes[2] = {&slot->left_eye, &slot->right_eye};
    const EyeOut* outs[2] = {&frame.left, &frame.right};
    const std::uint64_t bits_openness[2] = {FGB_ATTR_LEFT_OPENNESS, FGB_ATTR_RIGHT_OPENNESS};
    const std::uint64_t bits_pupil[2] = {FGB_ATTR_LEFT_PUPIL, FGB_ATTR_RIGHT_PUPIL};
    const std::uint64_t bits_gaze[2] = {FGB_ATTR_LEFT_GAZE, FGB_ATTR_RIGHT_GAZE};

    for (int i = 0; i < 2; ++i) {
        FgbEyeAttributes* eye = eyes[i];
        const EyeOut& out = *outs[i];
        if (out.has_openness) {
            eye->openness = Clamp01(out.openness);
            eye->openness_confidence = 1.0f;
            slot->attributes |= bits_openness[i];
        }
        if (out.has_pupil) {
            eye->pupil_diameter_mm = out.pupil_mm;
            slot->attributes |= bits_pupil[i];
        }
        if (out.has_gaze) {
            eye->gaze_dir[0] = out.gaze_dir[0];
            eye->gaze_dir[1] = out.gaze_dir[1];
            eye->gaze_dir[2] = out.gaze_dir[2];
            eye->gaze_confidence = 1.0f;
            slot->attributes |= bits_gaze[i];
        }
    }
}

} // namespace psvr2
