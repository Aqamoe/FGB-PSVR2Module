// 走真实 ABI 的端到端冒烟：起一个进程内的合成 PSVR2 Toolkit TCP 服务端（绑 3364），
// 加载 DLL → query → initialize → 握手/轮询/回帧 → update → 检查提交帧；
// 再强制踢掉服务端连接，验证模块能在退避后自动重连并恢复出帧。
//
// 它会绑定 TCP 3364 端口，本机运行前先关闭 PSVR2 Toolkit 的 DriverEx；
// GitHub Windows runner 上该端口空闲，因此 ci.yml 里 run_smoke 常开。
// 因此不挂进 ctest，只由 `cmake --build ... --target smoke-test` 触发。

#include "psvr2_protocol.hpp"

#include "fgb/kit/testing/check.hpp"
#include "fgb/kit/testing/host_mock.hpp"
#include "fgb_module_api.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>

namespace {

using namespace psvr2;

constexpr std::uint16_t kPort = 3364;

// 合成服务端：先 phase1 数值，被 Kick 后切 phase2 并期待模块重连重新握手。
class FakeToolkit {
public:
    bool Start(std::string& error) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            error = "服务端 socket 失败";
            return false;
        }
        BOOL reuse = TRUE;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kPort);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = "无法绑定 TCP 3364，WSA " + std::to_string(::WSAGetLastError()) +
                    "（DriverEx 是否在运行？）";
            return false;
        }
        ::listen(listener_, 2);
        const DWORD timeout = 50;
        ::setsockopt(listener_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                     sizeof(timeout));
        stop_.store(false);
        worker_ = std::thread{&FakeToolkit::Run, this};
        return true;
    }

    void KickClient() { kick_.store(true); }

    void Stop() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (client_ != INVALID_SOCKET) {
            ::closesocket(client_);
            client_ = INVALID_SOCKET;
        }
        if (listener_ != INVALID_SOCKET) {
            ::closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
    }

    std::atomic<int> handshakes{0};
    std::atomic<int> gaze_requests{0};
    std::atomic<int> phase{1}; // 1=初始数值，2=重连后的数值

private:
    void Run() {
        client_ = INVALID_SOCKET;
        while (!stop_.load()) {
            if (client_ == INVALID_SOCKET) {
                // Windows 上 SO_RCVTIMEO 不作用于 accept，用 select 做 50ms 轮询以便退出。
                fd_set readable;
                FD_ZERO(&readable);
                FD_SET(listener_, &readable);
                timeval timeout{};
                timeout.tv_usec = 50000;
                const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
                if (ready <= 0) {
                    continue;
                }
                SOCKET incoming = ::accept(listener_, nullptr, nullptr);
                if (incoming == INVALID_SOCKET) {
                    continue;
                }
                const DWORD receive_timeout = 20;
                ::setsockopt(incoming, SOL_SOCKET, SO_RCVTIMEO,
                             reinterpret_cast<const char*>(&receive_timeout),
                             sizeof(receive_timeout));
                pending_.clear(); // 上一条连接可能留下半截请求
                client_ = incoming;
                continue;
            }
            if (kick_.exchange(false)) {
                ::closesocket(client_);
                client_ = INVALID_SOCKET; // 模拟 Toolkit 侧断链，模块应自动重连
                continue;
            }
            std::uint8_t buffer[64];
            const int received = ::recv(client_, reinterpret_cast<char*>(buffer),
                                        static_cast<int>(sizeof(buffer)), 0);
            if (received > 0) {
                ServeBytes(buffer, static_cast<std::size_t>(received));
                continue;
            }
            if (received == SOCKET_ERROR && ::WSAGetLastError() == WSAETIMEDOUT) {
                continue;
            }
            ::closesocket(client_);
            client_ = INVALID_SOCKET; // 对端断开（模块 Stop 或重连前的关闭）
        }
        if (client_ != INVALID_SOCKET) {
            ::closesocket(client_);
            client_ = INVALID_SOCKET;
        }
    }

    // 请求也是「帧头+负载」；按 8 字节头 + data_len 消费。
    void ServeBytes(std::uint8_t* data, std::size_t size) {
        pending_.insert(pending_.end(), data, data + size);
        for (;;) {
            if (pending_.size() < kHeaderBytes) {
                return;
            }
            Header header{};
            DecodeHeader(pending_.data(), header);
            if (header.data_len < 0 ||
                pending_.size() < kHeaderBytes + static_cast<std::size_t>(header.data_len)) {
                return;
            }
            pending_.erase(pending_.begin(),
                           pending_.begin() + static_cast<std::ptrdiff_t>(
                               kHeaderBytes + (header.data_len > 0 ? header.data_len : 0)));
            if (header.type == kCmdClientRequestHandshake) {
                ++handshakes;
                SendHandshakeResult();
            } else if (header.type == kCmdClientRequestGazeData) {
                ++gaze_requests;
                SendGazeResult();
            }
        }
    }

    void SendHandshakeResult() {
        std::uint8_t payload[kHandshakeResultBytes] = {0};
        payload[0] = kHandshakeSuccess;
        WriteU16Le(payload + 2, kIpcVersion);
        SendFrame(kCmdServerHandshakeResult, payload, kHandshakeResultBytes);
    }

    void SendGazeResult() {
        // 拼 76 字节 v2 回包：左眼看向 (x>0,y>0,+Z 前)，右眼 phase 可切换眨眼。
        std::vector<std::uint8_t> payload(kGazeResultBytesV2, 0);
        const int phase_value = phase.load();
        const float left_dir[3] = {0.1f, 0.05f, 0.99302570f}; // 单位向量（+Z 前极化）
        const float right_dir[3] = {-0.2f, 0.1f, 0.97363600f};
        const float origin[3] = {8.0f, 1.5f, -42.0f};
        const float left_open = 0.8f;
        const float right_open = phase_value == 1 ? 0.6f : 0.1f;
        FillEye(payload, 0, left_dir, origin, 3.1f, left_open);
        FillEye(payload, kEyeBytesV2, right_dir, origin, 3.2f, right_open);
        SendFrame(kCmdServerGazeDataResult, payload.data(), payload.size());
    }

    static void FillEye(std::vector<std::uint8_t>& payload, std::size_t at, const float (&dir)[3],
                        const float (&origin)[3], float pupil, float open) {
        payload[at + 0] = 1; // isGazeOriginValid
        WriteFloatLe(payload.data() + at + 1, origin[0]);
        WriteFloatLe(payload.data() + at + 5, origin[1]);
        WriteFloatLe(payload.data() + at + 9, origin[2]);
        payload[at + 13] = 1; // isGazeDirValid
        WriteFloatLe(payload.data() + at + 14, dir[0]);
        WriteFloatLe(payload.data() + at + 18, dir[1]);
        WriteFloatLe(payload.data() + at + 22, dir[2]);
        payload[at + 26] = 1; // isPupilDiaValid
        WriteFloatLe(payload.data() + at + 27, pupil);
        payload[at + 31] = 1; // isBlinkValid
        payload[at + 32] = 0; // blink
        payload[at + 33] = 1; // isOpenEnabled
        WriteFloatLe(payload.data() + at + 34, open);
    }

    void SendFrame(std::uint16_t type, const std::uint8_t* payload, std::size_t len) {
        if (client_ == INVALID_SOCKET) {
            return;
        }
        std::vector<std::uint8_t> frame(kHeaderBytes + len, 0);
        WriteU16Le(frame.data(), type);
        WriteI32Le(frame.data() + 4, static_cast<std::int32_t>(len));
        if (payload != nullptr && len > 0) {
            std::memcpy(frame.data() + kHeaderBytes, payload, len);
        }
        ::send(client_, reinterpret_cast<const char*>(frame.data()),
               static_cast<int>(frame.size()), 0);
    }

    SOCKET listener_ = INVALID_SOCKET;
    SOCKET client_ = INVALID_SOCKET;
    std::atomic<bool> stop_{true};
    std::atomic<bool> kick_{false};
    std::thread worker_;
    std::vector<std::uint8_t> pending_;
};

template <typename Predicate>
bool WaitFor(Predicate predicate, int timeout_ms) {
    for (int elapsed = 0; elapsed < timeout_ms; elapsed += 10) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法：%s <psvr2.dll 路径>\n", argv[0]);
        return 2;
    }

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("WSAStartup 失败\n");
        return 1;
    }

    FakeToolkit toolkit;
    std::string net_error;
    if (!toolkit.Start(net_error)) {
        std::printf("合成服务端启动失败：%s\n", net_error.c_str());
        ::WSACleanup();
        return 1;
    }

    HMODULE library = ::LoadLibraryExA(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CHECK(library != nullptr);
    if (library == nullptr) {
        std::printf("LoadLibrary 失败，错误码 %lu\n", ::GetLastError());
        toolkit.Stop();
        ::WSACleanup();
        return CheckSummary("psvr2-abi-smoke");
    }

    const auto query = reinterpret_cast<decltype(&fgb_module_query_v2)>(
        ::GetProcAddress(library, "fgb_module_query_v2"));
    CHECK(query != nullptr);

    fgb::kit::testing::HostMock host;
    FgbInitParams params = host.MakeParams();

    const FgbModule* module = query(FGB_MODULE_ABI_V2);
    CHECK(module != nullptr);
    if (module == nullptr) {
        ::FreeLibrary(library);
        toolkit.Stop();
        ::WSACleanup();
        return CheckSummary("psvr2-abi-smoke");
    }
    CHECK(module->id == std::string{"com.fgbbridge.psvr2"});
    CHECK_EQ(module->abi_version, FGB_MODULE_ABI_V2);
    CHECK_EQ(module->capabilities,
             static_cast<std::uint32_t>(FGB_CAP_EYES | FGB_CAP_GAZE));
    CHECK(query(FGB_MODULE_ABI_V2 + 1) == nullptr); // ABI 不匹配必须拒绝

    void* self = nullptr;
    CHECK_EQ(module->initialize(&self, host.services(), &params), 1);
    if (self == nullptr) {
        std::printf("initialize 失败：%s\n", host.LastStatusDetail().c_str());
        ::FreeLibrary(library);
        toolkit.Stop();
        ::WSACleanup();
        return CheckSummary("psvr2-abi-smoke");
    }

    // ---- 阶段 1：握手 + 轮询 + 出帧 ----
    const bool connected = WaitFor([&toolkit]() { return toolkit.handshakes.load() > 0; }, 5000);
    CHECK(connected);
    const bool got_frames = WaitFor(
        [&]() {
            module->update(self);
            return !host.committed().empty();
        },
        5000);
    CHECK(got_frames);

    if (!host.committed().empty()) {
        const FgbFrameSlot& slot = host.committed().back();
        CHECK((slot.attributes & FGB_ATTR_LEFT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_RIGHT_GAZE) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_OPENNESS) != 0);
        CHECK((slot.attributes & FGB_ATTR_LEFT_PUPIL) != 0);
        CHECK((slot.attributes & FGB_ATTR_EXPRESSION) == 0); // 眼追源不该写表情
        // (-x, y, -z)：左眼 x=0.1 → gaze_dir[0]≈-0.1，z≈-0.993
        CHECK_NEAR(slot.left_eye.gaze_dir[0], -0.1f, 2e-3);
        CHECK_NEAR(slot.left_eye.gaze_dir[2], -0.993f, 2e-3);
        CHECK(slot.left_eye.gaze_dir[2] < 0.0f);
        CHECK_NEAR(slot.left_eye.pupil_diameter_mm, 3.1, 1e-3); // 毫米直传
    }
    // 轮询是持续的（8ms 节奏），不是单次请求：等计数涨起来再断言，避免采样时机误报。
    CHECK(WaitFor([&toolkit]() { return toolkit.gaze_requests.load() >= 3; }, 2000));

    // ---- 阶段 2：踢掉连接 → 退避重连 → 重新握手 → 新数值出帧 ----
    toolkit.phase.store(2);
    toolkit.KickClient();
    const bool reconnected =
        WaitFor([&toolkit]() { return toolkit.handshakes.load() >= 2; }, 8000);
    CHECK(reconnected); // 首次退避 0.5s，8 秒足够

    // 右眼 open 从 0.6 切到 0.1；滑动平均 14 点收敛，等到最近一帧明显低于 0.25。
    const bool settled = WaitFor(
        [&]() {
            module->update(self);
            return !host.committed().empty() &&
                   (host.committed().back().attributes & FGB_ATTR_RIGHT_OPENNESS) != 0 &&
                   host.committed().back().right_eye.openness < 0.25f;
        },
        8000);
    CHECK(settled);

    module->teardown(self);
    toolkit.Stop();
    ::FreeLibrary(library);
    ::WSACleanup();
    return CheckSummary("psvr2-abi-smoke");
}
