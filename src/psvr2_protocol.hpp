#pragma once
// PSVR2 Toolkit IPC（TCP 127.0.0.1:3364）的纯协议层：帧构造、流式分帧、解析与清洗。
//
// 线格式一手核对自上游 C# 客户端 PSVR2Toolkit.VRCFT 的 IpcProtocol.cs / IpcClient.cs
// （MIT 许可，仅参考未复制源码）；逐字段与「已确认/未确认」标注见 docs/protocol.md。
// 要点：
//   - 帧头 8 字节：u16 type + 2 字节填充 + i32 data_len，小端（C# 默认序列化布局）。
//   - 握手：ClientRequestHandshake(2) 带 u16 ipc 版本 + u32 进程号；
//     ServerHandshakeResult(3) 回 u8 结果 + u16 服务端版本。
//   - 眼追：ClientRequestGazeData(4) 空载轮询（约 120Hz），ServerGazeDataResult(5)
//     按版本回 66 字节（v1，双眼各 33）或 76 字节（v2，双眼各 38，多 open 字段）。
//   - 单位：gazeOriginMm 毫米、pupilDiaMm 毫米（ABI 的 pupil_diameter_mm 同为毫米，
//     不换算）；gazeDirNorm 名义归一，仍重归一并按 OpenXR 极化（-Z 前）。
// 本文件不依赖 winsock，可在无网络环境下做字节级单测。

#include "fgb_module_api.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace psvr2 {

// ---- 常量（值来自上游枚举的声明顺序，见 docs/protocol.md）----
constexpr std::uint16_t kCmdClientPing = 0;
constexpr std::uint16_t kCmdServerPong = 1;
constexpr std::uint16_t kCmdClientRequestHandshake = 2;
constexpr std::uint16_t kCmdServerHandshakeResult = 3;
constexpr std::uint16_t kCmdClientRequestGazeData = 4;
constexpr std::uint16_t kCmdServerGazeDataResult = 5;

constexpr std::uint16_t kIpcVersion = 2;         // 上游客户端声明的 IPC 版本
constexpr std::size_t kHeaderBytes = 8;          // u16 type + pad2 + i32 len
constexpr std::size_t kHandshakeRequestBytes = 8;  // u16 ver + pad2 + u32 pid
constexpr std::size_t kHandshakeResultBytes = 4;   // u8 result + pad1 + u16 ver
constexpr std::size_t kEyeBytesV1 = 33;          // Pack=1，bool/f32 混排无填充
constexpr std::size_t kEyeBytesV2 = 38;          // v2 多出 isOpenEnabled + open
constexpr std::size_t kGazeResultBytesV1 = 66;   // 左右眼各 33
constexpr std::size_t kGazeResultBytesV2 = 76;   // 左右眼各 38
constexpr std::int32_t kMaxDataBytes = 4096;     // 分帧保护：超过即判为流损坏

// ---- 握手结果码（上游 EHandshakeResult : byte）----
constexpr std::uint8_t kHandshakeFailed = 0;
constexpr std::uint8_t kHandshakeSuccess = 1;
constexpr std::uint8_t kHandshakeOutdated = 2;

constexpr std::uint64_t kBackoffBaseNs = 500ull * 1000000ull;  // 首次重连退避 0.5s
constexpr std::uint64_t kBackoffCapNs = 10ull * 1000000000ull; // 上限 10s

// 第 attempt 次（从 1 起）连接失败后的等待时长；指数退避封顶。纯函数便于单测。
std::uint64_t ReconnectDelayNs(std::uint32_t attempt);

// ---- 小端读写（x64 本机就是小端，仍显式转换保证测试可移植）----
std::uint16_t ReadU16Le(const std::uint8_t* p);
std::int32_t ReadI32Le(const std::uint8_t* p);
float ReadFloatLe(const std::uint8_t* p);
void WriteU16Le(std::uint8_t* p, std::uint16_t v);
void WriteI32Le(std::uint8_t* p, std::int32_t v);
void WriteFloatLe(std::uint8_t* p, float v);
bool IsFinite(float v);

// ---- 请求构造 ----
std::vector<std::uint8_t> BuildHandshakeRequest(std::uint32_t process_id);
std::vector<std::uint8_t> BuildGazeDataRequest();
std::vector<std::uint8_t> BuildPingRequest();

// ---- 帧头与单条消息 ----
struct Header {
    std::uint16_t type = 0;
    std::int32_t data_len = 0;
};
bool DecodeHeader(const std::uint8_t* p, Header& out);

// TCP 是字节流，需要自己分帧。Push 把任意切分的收包喂进来，产出完整消息；
// data_len 为负或超限视为协议违规（流已错位，调用方应断开重连）。
struct Message {
    std::uint16_t type = 0;
    std::vector<std::uint8_t> payload;
};

class MessageFramer {
public:
    void Reset() { buffer_.clear(); }

    // 返回 false 表示协议违规：out 里可能已有违规前的完整消息，error 说明原因。
    bool Push(const std::uint8_t* data, std::size_t size, std::vector<Message>& out,
              std::string& error);

    std::size_t BufferedBytes() const { return buffer_.size(); }

private:
    std::vector<std::uint8_t> buffer_;
};

// ---- 解析结果 ----
struct HandshakeResult {
    std::uint8_t code = kHandshakeFailed;
    std::uint16_t ipc_version = kIpcVersion;
};

// data_len 与布局不符（非 4 字节）必须拒收，返回 false。
bool ParseHandshakeResult(const std::uint8_t* data, std::size_t len, HandshakeResult& out,
                          std::string& error);

struct EyeSample {
    bool origin_valid = false;
    float origin_mm[3] = {0.0f, 0.0f, 0.0f};  // 毫米；ABI 帧槽没有原点字段，解析后不使用
    bool dir_valid = false;
    float dir[3] = {0.0f, 0.0f, 0.0f};        // 名义归一方向
    bool pupil_valid = false;
    float pupil_mm = 0.0f;                    // 毫米
    bool blink_valid = false;
    bool blink = false;
    bool open_enabled = false;                // 仅 v2
    float open = 0.0f;                        // 仅 v2，0..1
};

struct GazeFrame {
    EyeSample left;
    EyeSample right;
};

// 按协商出的 ipc 版本解析；len 与版本要求的 66/76 不符返回 false（拒收，不猜布局）。
bool ParseGazeResult(const std::uint8_t* data, std::size_t len, std::uint16_t ipc_version,
                     GazeFrame& out, std::string& error);

// ---- 输出帧（清洗后、可直接写帧槽）----
struct EyeOut {
    bool has_openness = false;
    float openness = 1.0f;
    bool has_pupil = false;
    float pupil_mm = 0.0f;
    bool has_gaze = false;
    float gaze_dir[3] = {0.0f, 0.0f, -1.0f};
    std::string rejected;  // 本眼里被丢弃的字段原因，用于限频告警（可为空）
};

struct OutputFrame {
    EyeOut left;
    EyeOut right;
    bool AnyValid() const {
        return left.has_openness || left.has_pupil || left.has_gaze || right.has_openness ||
               right.has_pupil || right.has_gaze;
    }
};

// 14 点滑动平均（沿用上游 LowPassFilter 的窗口长度），首值即种子，避免从 0 起步。
class OpennessFilter {
public:
    float Filter(float value);
    void Reset();

private:
    float samples_[14] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                          0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::size_t next_ = 0;
    std::size_t filled_ = 0;
};

// 清洗 + 换算 + 平滑。无效/非有限/极化可疑的字段不置位，绝不伪造。
class Packetizer {
public:
    // 逐眼：
    //  openness = blink_valid ? (v2 且 open_enabled ? clamp(open) : blink ? 0 : 1) : 无
    //  pupil    = pupil_valid 且有限且 (0, 20] 毫米
    //  gaze     = dir_valid 且重归一后有限，映射 (-x, y, -z) 后 z 必须为负（-Z 前）
    OutputFrame Build(const GazeFrame& frame);

private:
    OpennessFilter left_filter_;
    OpennessFilter right_filter_;
};

// 把 OutputFrame 写入帧槽：只置真正有数据的属性位。
void WriteFrame(const OutputFrame& frame, FgbFrameSlot* slot);

} // namespace psvr2
