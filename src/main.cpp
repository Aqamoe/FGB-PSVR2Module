// PlayStation VR2 眼追 → FaceGaze Bridge 原生模块。
//
// 通过本地 TCP（默认 127.0.0.1:3364）连接 PlayStation VR2 Toolkit 的 DriverEx IPC
// 服务：握手协商 IPC 版本后按约 120Hz 轮询眼追数据，输出逐眼注视方向、睁眼度与
// 瞳孔直径。不需要 VRCFT，不注入任何进程，也不需要管理员权限。
//
// 线格式一手核对自上游 C# 客户端（见 docs/protocol.md），本文件只负责连接与生命周期：
// 后台线程独占套接字做「连接→握手→轮询→收包→分帧解析」，宿主的 update() 里只取
// 最新一帧并写帧槽，日志事件由线程排队、tick 里排空——保证不阻塞引擎线程，
// 也不会因为连不上而每个 tick 重连刷日志（指数退避 + 限频告警）。

#include "psvr2_protocol.hpp"

#include "fgb/kit/config.hpp"
#include "fgb/kit/module.hpp" // 连带 host.hpp / udp.hpp：WinsockRuntime、NowNs

#include <windows.h> // GetCurrentProcessId（必须在 winsock2.h 之后）

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr char kDefaultAddress[] = "127.0.0.1";
constexpr std::uint16_t kDefaultPort = 3364;
// 上游 IpcClient 每 8ms 发一次 ClientRequestGazeData（约 120Hz），沿用同一节奏。
constexpr std::uint64_t kRequestPeriodNs = 8ull * 1000000ull;
constexpr std::uint64_t kNoDataTimeoutNs = 5ull * 1000000000ull;
constexpr std::uint64_t kWarnIntervalNs = 10ull * 1000000000ull;
constexpr std::size_t kEventLimit = 32;

// 后台线程 → 宿主线程的日志事件（线程不能直接调宿主回调）。
struct LogEvent {
    int level = FGB_LOG_INFO;
    std::string text;
};

// 拥有套接字的管道：断线自动按 ReconnectDelayNs 的指数退避重连。
class GazePipe {
public:
    struct Snapshot {
        psvr2::GazeFrame frame{};
        std::uint64_t sequence = 0;
        std::uint64_t received_ns = 0;
    };

    bool Start(const std::string& address, std::uint16_t port, std::string& error) {
        if (!fgb::kit::WinsockRuntime::Acquire(error)) {
            return false;
        }
        winsock_acquired_ = true;
        address_ = address;
        port_ = port;
        stop_.store(false);
        worker_ = std::thread{&GazePipe::Run, this};
        return true;
    }

    void Stop() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (winsock_acquired_) {
            winsock_acquired_ = false;
            fgb::kit::WinsockRuntime::Release();
        }
    }

    // 排空事件队列，宿主线程逐条写日志。
    void DrainEvents(fgb::kit::Host& host) {
        std::deque<LogEvent> pending;
        {
            std::lock_guard lock{state_mutex_};
            pending.swap(events_);
        }
        for (const LogEvent& event : pending) {
            host.Log(event.level, event.text);
        }
    }

    // 取「比上次更新」的一帧眼追数据。
    bool TakeLatest(Snapshot& out, std::uint64_t& seen_sequence) {
        std::lock_guard lock{state_mutex_};
        if (sequence_ == seen_sequence) {
            return false;
        }
        out = latest_;
        seen_sequence = sequence_;
        return true;
    }

    std::uint64_t LastFrameNs() {
        std::lock_guard lock{state_mutex_};
        return latest_.received_ns;
    }

private:
    void PushEvent(int level, std::string text) {
        std::lock_guard lock{state_mutex_};
        if (events_.size() >= kEventLimit) {
            events_.pop_front(); // 事件刷屏时丢最旧的
        }
        events_.push_back(LogEvent{level, std::move(text)});
    }

    void Run();
    bool TryConnect(std::string& error);
    void CloseSocket();
    void Dispatch(const psvr2::Message& message);

    SOCKET socket_ = INVALID_SOCKET;
    std::string address_ = kDefaultAddress;
    std::uint16_t port_ = kDefaultPort;
    bool winsock_acquired_ = false;
    std::atomic<bool> stop_{true};
    std::thread worker_;
    std::mutex state_mutex_;
    Snapshot latest_{};
    std::uint64_t sequence_ = 0;
    std::deque<LogEvent> events_;

    // 以下只在后台线程里访问。
    psvr2::MessageFramer framer_;
    std::uint16_t server_ipc_version_ = psvr2::kIpcVersion;
    std::uint32_t connect_failures_ = 0;
    std::uint64_t next_attempt_ns_ = 0;
    std::uint64_t next_fail_event_ns_ = 0;
};

bool GazePipe::TryConnect(std::string& error) {
    const SOCKET handle = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle == INVALID_SOCKET) {
        error = "socket() 失败，WSA " + std::to_string(::WSAGetLastError());
        return false;
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(port_);
    if (::inet_pton(AF_INET, address_.c_str(), &target.sin_addr) != 1) {
        error = "目标地址非法：" + address_;
        ::closesocket(handle);
        return false;
    }
    // 回环地址上的 connect 要么立即成功要么立即被拒，不会长时间卡住线程。
    if (::connect(handle, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == SOCKET_ERROR) {
        error = "无法连接 " + address_ + ":" + std::to_string(port_) + "，WSA " +
                std::to_string(::WSAGetLastError());
        ::closesocket(handle);
        return false;
    }
    DWORD timeout = 5; // recv 5ms 超时，让线程能周期性检查 stop_ 与请求节奏
    ::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
    BOOL nodelay = TRUE;
    ::setsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                 sizeof(nodelay));

    socket_ = handle;
    framer_.Reset();
    server_ipc_version_ = psvr2::kIpcVersion; // 握手回来之前沿用上游的默认值

    const std::vector<std::uint8_t> handshake =
        psvr2::BuildHandshakeRequest(static_cast<std::uint32_t>(::GetCurrentProcessId()));
    const int sent = ::send(socket_, reinterpret_cast<const char*>(handshake.data()),
                            static_cast<int>(handshake.size()), 0);
    if (sent == SOCKET_ERROR) {
        error = "发送握手请求失败，WSA " + std::to_string(::WSAGetLastError());
        CloseSocket();
        return false;
    }
    return true;
}

void GazePipe::CloseSocket() {
    if (socket_ != INVALID_SOCKET) {
        ::closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}

void GazePipe::Dispatch(const psvr2::Message& message) {
    if (message.type == psvr2::kCmdServerHandshakeResult) {
        psvr2::HandshakeResult result{};
        std::string error;
        if (!psvr2::ParseHandshakeResult(message.payload.data(), message.payload.size(), result,
                                         error)) {
            PushEvent(FGB_LOG_WARN, "拒收握手结果：" + error);
            return;
        }
        // 上游规则：版本恰为 1 用旧布局，其余都按 v2 的 76 字节解析。
        server_ipc_version_ = result.ipc_version == 1 ? 1 : 2;
        if (result.code == psvr2::kHandshakeSuccess) {
            PushEvent(FGB_LOG_INFO, "PSVR2 Toolkit 握手成功，IPC 版本 " +
                                        std::to_string(server_ipc_version_));
        } else if (result.code == psvr2::kHandshakeOutdated) {
            PushEvent(FGB_LOG_ERROR, "握手被拒：本客户端 IPC v" +
                                         std::to_string(psvr2::kIpcVersion) +
                                         " 过旧，服务端要求 v" +
                                         std::to_string(result.ipc_version) +
                                         "，请升级 PlayStation VR2 Toolkit");
        } else {
            PushEvent(FGB_LOG_ERROR,
                      "握手被拒：服务端返回 Failed，检查 Toolkit 里是否启用了眼动");
        }
        return;
    }
    if (message.type == psvr2::kCmdServerGazeDataResult) {
        psvr2::GazeFrame frame{};
        std::string error;
        if (!psvr2::ParseGazeResult(message.payload.data(), message.payload.size(),
                                    server_ipc_version_, frame, error)) {
            PushEvent(FGB_LOG_WARN, "拒收眼追回包：" + error);
            return;
        }
        std::lock_guard lock{state_mutex_};
        latest_.frame = frame;
        latest_.received_ns = fgb::kit::NowNs();
        ++sequence_;
        return;
    }
    // ServerPong 与未实现的消息类型（振动器指令的回包等）忽略，与上游一致。
}

void GazePipe::Run() {
    std::vector<std::uint8_t> recv_buffer(4096);
    std::uint64_t next_request_ns = 0;

    while (!stop_.load()) {
        const std::uint64_t now = fgb::kit::NowNs();

        if (socket_ == INVALID_SOCKET) {
            if (now < next_attempt_ns_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            std::string error;
            if (TryConnect(error)) {
                connect_failures_ = 0;
                next_fail_event_ns_ = 0;
                next_request_ns = fgb::kit::NowNs();
                PushEvent(FGB_LOG_INFO, "已连接 PSVR2 Toolkit（" + address_ + ":" +
                                            std::to_string(port_) + "），开始轮询眼追数据");
            } else {
                ++connect_failures_;
                const std::uint64_t fail_now = fgb::kit::NowNs();
                // 首次失败立刻告警；之后每 30 秒最多一条，避免退避期间刷日志。
                if (fail_now >= next_fail_event_ns_) {
                    PushEvent(FGB_LOG_WARN,
                              error + "（第 " + std::to_string(connect_failures_) +
                                  " 次）。需要 PSVR2 驱动与 Toolkit 的 DriverEx 在运行；"
                                  "重连间隔已退避到 " +
                                  std::to_string(psvr2::ReconnectDelayNs(connect_failures_) /
                                                 1000000ull) +
                                  "ms");
                    next_fail_event_ns_ = fail_now + 30ull * 1000000000ull;
                }
                next_attempt_ns_ = fail_now + psvr2::ReconnectDelayNs(connect_failures_);
                continue;
            }
        }

        // 连接存活：按 8ms 节奏发轮询请求，recv 超时 5ms，两头都不阻塞退出检查。
        const std::uint64_t pump_now = fgb::kit::NowNs();
        if (pump_now >= next_request_ns) {
            const std::vector<std::uint8_t> request = psvr2::BuildGazeDataRequest();
            const int sent = ::send(socket_, reinterpret_cast<const char*>(request.data()),
                                    static_cast<int>(request.size()), 0);
            if (sent == SOCKET_ERROR) {
                PushEvent(FGB_LOG_WARN, "发送轮询请求失败，WSA " +
                                            std::to_string(::WSAGetLastError()) + "，准备重连");
                CloseSocket();
                connect_failures_ = 0;
                next_attempt_ns_ = fgb::kit::NowNs() + psvr2::kBackoffBaseNs;
                continue;
            }
            next_request_ns = pump_now + kRequestPeriodNs;
        }

        const int received =
            ::recv(socket_, reinterpret_cast<char*>(recv_buffer.data()),
                   static_cast<int>(recv_buffer.size()), 0);
        if (received > 0) {
            std::vector<psvr2::Message> messages;
            std::string error;
            if (!framer_.Push(recv_buffer.data(), static_cast<std::size_t>(received), messages,
                              error)) {
                PushEvent(FGB_LOG_WARN, error + "；断开并重连");
                CloseSocket();
                connect_failures_ = 0;
                next_attempt_ns_ = fgb::kit::NowNs() + psvr2::kBackoffBaseNs;
                continue;
            }
            for (const psvr2::Message& message : messages) {
                Dispatch(message);
            }
            continue;
        }
        if (received == 0) {
            PushEvent(FGB_LOG_WARN, "PSVR2 Toolkit 关闭了连接，稍后按退避自动重连");
            CloseSocket();
            connect_failures_ = 0;
            next_attempt_ns_ = fgb::kit::NowNs() + psvr2::kBackoffBaseNs;
            continue;
        }
        const int code = ::WSAGetLastError();
        if (code != WSAETIMEDOUT) {
            PushEvent(FGB_LOG_WARN, "接收失败，WSA " + std::to_string(code) + "，稍后自动重连");
            CloseSocket();
            connect_failures_ = 0;
            next_attempt_ns_ = fgb::kit::NowNs() + psvr2::kBackoffBaseNs;
        }
    }
    CloseSocket();
}

class PSVR2Source : public fgb::kit::IModuleSource {
public:
    bool Start(fgb::kit::Host& host, const FgbInitParams& params, std::string& error) override {
        fgb::kit::ConfigIni config;
        std::string config_error;
        const std::string directory = params.module_dir != nullptr ? params.module_dir : "";
        config.AllowKeys({"host", "port"});
        if (config.Load(directory, "config.ini", config_error)) {
            address_ = config.Get("host", kDefaultAddress);
            port_ = static_cast<std::uint16_t>(config.GetInt("port", port_));
            if (!config_error.empty()) {
                host.Warn("config.ini：" + config_error);
            }
            if (!config.UnknownKeys().empty()) {
                host.Warn("config.ini 有未识别的键：" + config.UnknownKeys()[0]);
            }
        }

        if (!pipe_.Start(address_, port_, error)) {
            return false;
        }
        host.Info("PSVR2 眼追将连接 TCP " + address_ + ":" + std::to_string(port_) +
                  "，需 PlayStation VR2 Toolkit 的 DriverEx 在运行并启用眼动");
        host.StatusStarting("连接 PSVR2 Toolkit 中");
        return true;
    }

    bool Read(fgb::kit::Host& host, FgbFrameSlot& slot, std::uint64_t now_ns) override {
        pipe_.DrainEvents(host);

        GazePipe::Snapshot snapshot{};
        const bool fresh = pipe_.TakeLatest(snapshot, seen_sequence_);

        // 断流监控：连接断开或驱动停止推帧时给出一次可操作的提示，恢复后自动回到活跃。
        if (!staleness_.Check(host, pipe_.LastFrameNs(), now_ns) || !fresh) {
            return false;
        }

        const psvr2::OutputFrame frame = packetizer_.Build(snapshot.frame);
        const std::string rejected = frame.left.rejected + frame.right.rejected;
        if (!rejected.empty()) {
            host.WarnThrottled(warn_ns_, now_ns, kWarnIntervalNs,
                               "眼追数据有字段被丢弃（保持原属性不置位）：" + rejected);
        }
        if (!frame.AnyValid()) {
            // 整帧都无效（例如追踪丢失时全 0 有效位）：不提交，避免伪造一帧「看向正前」。
            host.WarnThrottled(empty_warn_ns_, now_ns, kWarnIntervalNs,
                               "PSVR2 眼追连续返回无效帧，可能未戴好头显或未标定");
            return false;
        }
        psvr2::WriteFrame(frame, &slot);
        return true;
    }

    void Stop(fgb::kit::Host& host) override {
        pipe_.Stop();
        host.StatusStopped("PSVR2 眼追已停止");
    }

private:
    GazePipe pipe_;
    psvr2::Packetizer packetizer_;
    fgb::kit::Staleness staleness_{kNoDataTimeoutNs, "PSVR2 眼追"};
    std::string address_ = kDefaultAddress;
    std::uint16_t port_ = kDefaultPort;
    std::uint64_t seen_sequence_ = 0;
    std::uint64_t warn_ns_ = 0;
    std::uint64_t empty_warn_ns_ = 0;
};

} // namespace

FGB_KIT_EXPORT(PSVR2Source, "com.fgbbridge.psvr2", "PlayStation VR2",
               FGB_CAP_EYES | FGB_CAP_GAZE)
