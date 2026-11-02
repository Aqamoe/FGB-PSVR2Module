// PSVR2 Toolkit IPC 的协议单测：全部针对纯逻辑层（psvr2_protocol.*），
// 用手工拼的小端字节数组做字节级断言，不需要网络，也不需要真实的 Toolkit。
//
// 期望值全部来自一手核对的上游 C# 布局（docs/protocol.md）：
// 帧头 8 字节（u16 type + pad2 + i32 len）、握手负载 8 字节、握手结果 4 字节、
// 眼追回包 v1=66 / v2=76 字节（Pack=1）。

#include "psvr2_protocol.hpp"

#include "fgb/kit/testing/check.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace psvr2;

// 按 Pack=1 布局拼一只眼（version2 时追加 isOpenEnabled + open）。
void PutEye(std::vector<std::uint8_t>& buf, std::size_t at, bool version2, bool origin_valid,
            const float origin[3], bool dir_valid, const float dir[3], bool pupil_valid,
            float pupil_mm, bool blink_valid, bool blink, bool open_enabled, float open) {
    buf[at + 0] = origin_valid ? 1 : 0;
    WriteFloatLe(buf.data() + at + 1, origin[0]);
    WriteFloatLe(buf.data() + at + 5, origin[1]);
    WriteFloatLe(buf.data() + at + 9, origin[2]);
    buf[at + 13] = dir_valid ? 1 : 0;
    WriteFloatLe(buf.data() + at + 14, dir[0]);
    WriteFloatLe(buf.data() + at + 18, dir[1]);
    WriteFloatLe(buf.data() + at + 22, dir[2]);
    buf[at + 26] = pupil_valid ? 1 : 0;
    WriteFloatLe(buf.data() + at + 27, pupil_mm);
    buf[at + 31] = blink_valid ? 1 : 0;
    buf[at + 32] = blink ? 1 : 0;
    if (version2) {
        buf[at + 33] = open_enabled ? 1 : 0;
        WriteFloatLe(buf.data() + at + 34, open);
    }
}

std::vector<std::uint8_t> MakeGazePayload(bool version2) {
    const std::size_t size = version2 ? kGazeResultBytesV2 : kGazeResultBytesV1;
    std::vector<std::uint8_t> payload(size, 0);
    const float origin_l[3] = {12.5f, -3.25f, 42.0f};
    const float dir_l[3] = {0.2f, 0.3f, 0.9f}; // 原坐标 +Z 前（Unity 极化，见 protocol.md）
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    PutEye(payload, 0, version2, true, origin_l, true, dir_l, true, 3.25f, true, false,
           version2, version2 ? 0.87f : 0.0f);
    PutEye(payload, version2 ? kEyeBytesV2 : kEyeBytesV1, version2, false, zero, false, zero,
           false, 0.0f, true, true, false, 0.0f);
    return payload;
}

std::vector<std::uint8_t> Wrap(std::uint16_t type, const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> frame(kHeaderBytes + payload.size(), 0);
    WriteU16Le(frame.data(), type);
    WriteI32Le(frame.data() + 4, static_cast<std::int32_t>(payload.size()));
    std::memcpy(frame.data() + kHeaderBytes, payload.data(), payload.size());
    return frame;
}

// 解析一段手工拼的 v2 回包负载，得到 GazeFrame（复用被测解析器，保证测试数据真实）。
GazeFrame MakeGazeFrameFromPayload(const std::vector<std::uint8_t>& payload) {
    GazeFrame frame{};
    std::string error;
    if (!ParseGazeResult(payload.data(), payload.size(), 2, frame, error)) {
        std::printf("FAIL 测试数据自身解析不过：%s\n", error.c_str());
        ++g_check_failures;
    }
    return frame;
}

bool BytesEqual(const std::vector<std::uint8_t>& actual, std::initializer_list<int> expect) {
    if (actual.size() != expect.size()) {
        return false;
    }
    std::size_t i = 0;
    for (const int byte : expect) {
        if (actual[i++] != static_cast<std::uint8_t>(byte)) {
            return false;
        }
    }
    return true;
}

float NaN() { return std::numeric_limits<float>::quiet_NaN(); }

bool AnyNaN(const float (&v)[3]) {
    return std::isnan(v[0]) || std::isnan(v[1]) || std::isnan(v[2]);
}

} // namespace

int main() {
    // ---- 握手请求帧：16 字节，逐字节断言（含 2 字节填充）----
    const std::vector<std::uint8_t> handshake = BuildHandshakeRequest(0x11223344u);
    CHECK_EQ(handshake.size(), static_cast<std::size_t>(16));
    CHECK(BytesEqual(handshake, {0x02, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00,
                                 0x00, 0x00, 0x44, 0x33, 0x22, 0x11}));

    // ---- 轮询请求帧：8 字节空载 ----
    const std::vector<std::uint8_t> request = BuildGazeDataRequest();
    CHECK(BytesEqual(request, {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    // ---- Ping：type 0、len 0 ----
    CHECK(BytesEqual(BuildPingRequest(), {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

    // ---- 握手结果解析：4 字节（u8 结果 + 填充 + u16 版本）----
    {
        std::uint8_t raw[4] = {0};
        raw[0] = kHandshakeSuccess;
        WriteU16Le(raw + 2, 1);
        HandshakeResult result{};
        std::string error;
        CHECK(ParseHandshakeResult(raw, 4, result, error));
        CHECK_EQ(static_cast<int>(result.code), static_cast<int>(kHandshakeSuccess));
        CHECK_EQ(result.ipc_version, static_cast<std::uint16_t>(1));

        // 长度不符（上游按 dataLen==4 才解析）必须拒收
        CHECK(!ParseHandshakeResult(raw, 3, result, error));
        CHECK(!ParseHandshakeResult(raw, 6, result, error));
    }

    // ---- 眼追回包解析：v2 76 字节 ----
    {
        std::vector<std::uint8_t> payload = MakeGazePayload(true);
        GazeFrame frame{};
        std::string error;
        CHECK(ParseGazeResult(payload.data(), payload.size(), 2, frame, error));
        CHECK(frame.left.origin_valid);
        CHECK_NEAR(frame.left.origin_mm[0], 12.5, 1e-6);  // 毫米，原样保留
        CHECK_NEAR(frame.left.origin_mm[2], 42.0, 1e-6);
        CHECK(frame.left.dir_valid);
        CHECK_NEAR(frame.left.dir[1], 0.3, 1e-6);
        CHECK(frame.left.pupil_valid);
        CHECK_NEAR(frame.left.pupil_mm, 3.25, 1e-6);
        CHECK(frame.left.blink_valid);
        CHECK(!frame.left.blink);
        CHECK(frame.left.open_enabled);
        CHECK_NEAR(frame.left.open, 0.87, 1e-6);
        CHECK(!frame.right.dir_valid);
        CHECK(frame.right.blink_valid);
        CHECK(frame.right.blink);

        // 长度与版本布局不符：66 字节配 v2、76 字节配 v1 都必须拒收
        std::vector<std::uint8_t> v1 = MakeGazePayload(false);
        CHECK(!ParseGazeResult(v1.data(), v1.size(), 2, frame, error));
        CHECK(!ParseGazeResult(payload.data(), payload.size(), 1, frame, error));
        CHECK(ParseGazeResult(v1.data(), v1.size(), 1, frame, error));
        CHECK(!frame.left.open_enabled); // v1 没有 open 字段，不得伪造
    }

    // ---- 分帧：TCP 任意切分都能重组成完整消息 ----
    {
        std::vector<std::uint8_t> stream = Wrap(kCmdServerHandshakeResult, {0x01, 0x00, 0x02, 0x00});
        std::vector<std::uint8_t> gaze = Wrap(kCmdServerGazeDataResult, MakeGazePayload(true));
        stream.insert(stream.end(), gaze.begin(), gaze.end());

        MessageFramer framer;
        std::vector<Message> messages;
        std::string error;
        for (std::size_t i = 0; i < stream.size(); ++i) { // 一次喂一个字节
            CHECK(framer.Push(&stream[i], 1, messages, error));
        }
        CHECK_EQ(messages.size(), static_cast<std::size_t>(2));
        CHECK_EQ(messages[0].type, kCmdServerHandshakeResult);
        CHECK_EQ(messages[0].payload.size(), static_cast<std::size_t>(4));
        CHECK_EQ(messages[1].type, kCmdServerGazeDataResult);
        CHECK_EQ(messages[1].payload.size(), kGazeResultBytesV2);
        CHECK_EQ(framer.BufferedBytes(), static_cast<std::size_t>(0));
    }

    // ---- 异常长度/字节序的帧头：判为流错位，拒收并要求重连 ----
    {
        // data_len 为负
        std::uint8_t negative[8] = {0};
        WriteU16Le(negative, kCmdServerGazeDataResult);
        WriteI32Le(negative + 4, -1);
        MessageFramer f1;
        std::vector<Message> out1;
        std::string error1;
        CHECK(!f1.Push(negative, 8, out1, error1));

        // data_len 超上限
        std::uint8_t huge[8] = {0};
        WriteU16Le(huge, kCmdServerGazeDataResult);
        WriteI32Le(huge + 4, 1 << 20);
        MessageFramer f2;
        std::vector<Message> out2;
        std::string error2;
        CHECK(!f2.Push(huge, 8, out2, error2));

        // 大端字节序（4096 → 0x00001000 → 按小端读成 0x00100000）：len 远超上限被拒
        std::uint8_t be[8] = {0};
        be[0] = 0x00; be[1] = 0x05;          // 大端的 type=5
        be[4] = 0x00; be[5] = 0x00; be[6] = 0x10; be[7] = 0x00; // 大端的 len=4096
        MessageFramer f3;
        std::vector<Message> out3;
        std::string error3;
        CHECK(!f3.Push(be, 8, out3, error3));

        // 违规后缓冲必须清空，避免脏字节拖垮后续帧
        f3.Reset();
        std::vector<Message> out4;
        std::string error4;
        CHECK(f3.Push(be + 1, 1, out4, error4)); // 只喂一个字节不会崩
    }

    // ---- 断线重连退避：0.5s 起步、指数增长、10s 封顶 ----
    CHECK_EQ(ReconnectDelayNs(1), 500000000ull);
    CHECK_EQ(ReconnectDelayNs(2), 1000000000ull);
    CHECK_EQ(ReconnectDelayNs(3), 2000000000ull);
    CHECK_EQ(ReconnectDelayNs(4), 4000000000ull);
    CHECK_EQ(ReconnectDelayNs(5), 8000000000ull);
    CHECK_EQ(ReconnectDelayNs(6), kBackoffCapNs);
    CHECK_EQ(ReconnectDelayNs(50), kBackoffCapNs);
    CHECK_EQ(ReconnectDelayNs(0), kBackoffBaseNs);

    // ---- 清洗与换算：NaN 与非有限值不得进帧 ----
    {
        GazeFrame frame{};
        frame.left.origin_valid = true;
        frame.left.origin_mm[0] = NaN(); // 原点没有帧槽字段，怎么脏都不影响输出
        frame.left.dir_valid = true;
        frame.left.dir[0] = NaN();
        frame.left.dir[1] = 0.3f;
        frame.left.dir[2] = 0.9f;
        frame.left.pupil_valid = true;
        frame.left.pupil_mm = NaN();
        frame.left.blink_valid = true;
        frame.left.open_enabled = true;
        frame.left.open = NaN();
        std::memset(&frame.right, 0, sizeof(frame.right));

        Packetizer packetizer;
        const OutputFrame clean = packetizer.Build(frame);
        CHECK(!clean.left.has_gaze);     // NaN 方向被丢弃
        CHECK(!clean.left.has_pupil);    // NaN 瞳孔被丢弃
        CHECK(!clean.left.has_openness); // NaN open 被丢弃
        CHECK(!clean.left.rejected.empty());
        CHECK(!clean.right.has_openness);
        CHECK(!clean.right.has_gaze);
        CHECK(!clean.right.has_pupil);
    }

    // ---- 无效帧不伪造：全 0 有效位时 AnyValid()==false，WriteFrame 不置属性 ----
    {
        GazeFrame frame{};
        Packetizer packetizer;
        const OutputFrame clean = packetizer.Build(frame);
        CHECK(!clean.AnyValid());

        FgbFrameSlot slot{};
        slot.struct_size = sizeof(FgbFrameSlot);
        slot.abi_version = FGB_MODULE_ABI_V2;
        slot.shape_count = FGB_MAX_SHAPES;
        WriteFrame(clean, &slot);
        CHECK_EQ(slot.attributes, static_cast<std::uint64_t>(0));
    }

    // ---- 方向极化：(-x, y, -z) 映射 + 重归一；朝后的方向按 fail-safe 丢弃 ----
    {
        GazeFrame frame{};
        frame.left.dir_valid = true;
        frame.left.dir[0] = 0.2f;
        frame.left.dir[1] = 0.3f;
        frame.left.dir[2] = 0.9f; // +Z 前（Unity 极化）→ 映射后 -Z 前
        frame.right.dir_valid = true;
        frame.right.dir[0] = 0.0f;
        frame.right.dir[1] = 0.0f;
        frame.right.dir[2] = -1.0f; // 已经是 -Z 前？极化假设不成立 → 丢弃而不是猜

        Packetizer packetizer;
        const OutputFrame clean = packetizer.Build(frame);
        CHECK(clean.left.has_gaze);
        const float length = std::sqrt(clean.left.gaze_dir[0] * clean.left.gaze_dir[0] +
                                       clean.left.gaze_dir[1] * clean.left.gaze_dir[1] +
                                       clean.left.gaze_dir[2] * clean.left.gaze_dir[2]);
        CHECK_NEAR(length, 1.0, 1e-5);
        CHECK(clean.left.gaze_dir[0] < 0.0f);
        CHECK(clean.left.gaze_dir[1] > 0.0f);
        CHECK(clean.left.gaze_dir[2] < 0.0f);
        CHECK_NEAR(clean.left.gaze_dir[0], -0.2f / std::sqrt(0.94f), 1e-5);
        CHECK(!clean.right.has_gaze); // 反向分量必须被拒，绝不翻转「凑」一个方向
        CHECK(!clean.right.rejected.empty());
    }

    // ---- 睁眼度：blink 二值 + v2 open 连续值；14 点滑动平均（首值种子）----
    {
        GazeFrame frame = MakeGazeFrameFromPayload(MakeGazePayload(true));
        Packetizer packetizer;
        const OutputFrame first = packetizer.Build(frame);
        CHECK(first.left.has_openness);
        CHECK_NEAR(first.left.openness, 0.87, 1e-6); // 首值种子：不被 0 拖低
        CHECK(first.right.has_openness);
        CHECK_NEAR(first.right.openness, 0.0, 1e-6); // 右眼 blink=1 → 0

        frame.left.open = 1.0f;
        const OutputFrame second = packetizer.Build(frame);
        // (0.87 * 13 + 1.0) / 14
        CHECK_NEAR(second.left.openness, (0.87f * 13.0f + 1.0f) / 14.0f, 1e-4);
    }

    // ---- 瞳孔：毫米直传（ABI 字段同为毫米），越界丢弃 ----
    {
        GazeFrame frame{};
        frame.left.pupil_valid = true;
        frame.left.pupil_mm = 3.7f;
        frame.right.pupil_valid = true;
        frame.right.pupil_mm = 250.0f; // 明显是脏数据
        Packetizer packetizer;
        const OutputFrame clean = packetizer.Build(frame);
        CHECK(clean.left.has_pupil);
        CHECK_NEAR(clean.left.pupil_mm, 3.7, 1e-6);
        CHECK(!clean.right.has_pupil);
    }

    // ---- WriteFrame：只置真正有数据的属性位 ----
    {
        OutputFrame frame{};
        frame.left.has_openness = true;
        frame.left.openness = 0.5f;
        frame.right.has_gaze = true;
        frame.right.gaze_dir[0] = 0.0f;
        frame.right.gaze_dir[1] = 0.0f;
        frame.right.gaze_dir[2] = -1.0f;

        FgbFrameSlot slot{};
        slot.struct_size = sizeof(FgbFrameSlot);
        slot.abi_version = FGB_MODULE_ABI_V2;
        slot.shape_count = FGB_MAX_SHAPES;
        WriteFrame(frame, &slot);
        CHECK_EQ(slot.attributes,
                 static_cast<std::uint64_t>(FGB_ATTR_LEFT_OPENNESS | FGB_ATTR_RIGHT_GAZE));
        CHECK_NEAR(slot.left_eye.openness, 0.5, 1e-6);
        CHECK_NEAR(slot.right_eye.gaze_dir[2], -1.0, 1e-6);
        CHECK(!AnyNaN(slot.left_eye.gaze_dir));
        CHECK(slot.right_eye.openness_confidence == 0.0f); // 右眼没给 openness
    }

    // ---- v2 回包按字节过一遍解析→清洗→写帧 的流水线 ----
    {
        GazeFrame frame = MakeGazeFrameFromPayload(MakeGazePayload(true));
        Packetizer packetizer;
        const OutputFrame clean = packetizer.Build(frame);
        CHECK(clean.AnyValid());
        CHECK(clean.left.has_gaze);
        CHECK(clean.left.has_pupil);
        CHECK(clean.left.has_openness);

        FgbFrameSlot slot{};
        slot.struct_size = sizeof(FgbFrameSlot);
        slot.abi_version = FGB_MODULE_ABI_V2;
        slot.shape_count = FGB_MAX_SHAPES;
        WriteFrame(clean, &slot);
        CHECK((slot.attributes & FGB_ATTR_LEFT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_PUPIL) != 0);
        CHECK_EQ(slot.attributes & FGB_ATTR_EXPRESSION, static_cast<std::uint64_t>(0));
        CHECK_NEAR(slot.left_eye.pupil_diameter_mm, 3.25, 1e-6);
    }

    return CheckSummary("psvr2-protocol");
}
