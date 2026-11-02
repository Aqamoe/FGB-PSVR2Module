# FGB PSVR2Module

[![build](https://github.com/Aqamoe/FGB-PSVR2Module/actions/workflows/ci.yml/badge.svg)](https://github.com/Aqamoe/FGB-PSVR2Module/actions/workflows/ci.yml)

**状态：0.1.0，未验证（untested）**。协议解析、清洗与 ABI 生命周期有单元测试和合成 TCP
服务端端到端冒烟（含断线重连）；真机（PSVR2 Toolkit DriverEx）验收待补，
见 [docs/verification.md](docs/verification.md)。

[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge) 的 PlayStation VR2 眼追原生模块。
通过本地 **TCP 127.0.0.1:3364** 连接 **PlayStation VR2 Toolkit** 的 DriverEx IPC 服务，
按约 120Hz 轮询眼追数据，输出逐眼注视方向、睁眼度与瞳孔直径。
不需要 VRCFT，不注入任何进程，也不需要管理员权限。

Native PlayStation VR2 eye-tracking module for FaceGaze Bridge. Talks the local TCP
IPC of PlayStation VR2 Toolkit's DriverEx (127.0.0.1:3364), polls gaze data at
~120Hz and outputs per-eye gaze direction, eye openness and pupil diameter.
No VRCFT, no process injection, no admin rights.

## 功能 / Features

- **注视方向**：`gazeDirNorm` 重归一后映射为 ABI 的 HMD 局部单位向量（-Z 前），
  极化依据与未确认项见 [docs/protocol.md](docs/protocol.md)。
- **睁眼度**：v2 的连续 `open` 优先，退回 `blink` 二值；14 点滑动平均抑制闪烁抖动。
- **瞳孔直径**：毫米直传（上游与 ABI 单位一致），越界脏数据丢弃。
- **断线可恢复**：连接失败指数退避（0.5s 起、10s 封顶），告警限频不刷日志；
  Toolkit 重启后自动重连并重新握手；5 秒无数据时在模块卡片提示原因。
- **不伪造数据**：任何 `is*Valid` 为假、NaN/Inf 或极化可疑的字段都不置属性位。

## 不提供 / Not provided

| 数据 | 原因 |
|---|---|
| 表情 | 上游眼追回包不含表情数据（PSVR2 Toolkit 不输出 ARKit 系数） |
| 眼注视原点 | 回包里的 `gazeOriginMm` 上游 VRCFT 也未使用；FGB ABI 帧槽没有原点字段 |
| 2D 注视点 | 无标定依据，不置 `POINT_VIEWPORT`；方向走 gaze 通道 |
| 头姿 | IPC 里没有头部姿态数据 |
| 控制器振动 | 同一 IPC 的 trigger 指令与眼追无关，不在本模块职责内 |

## 环境要求

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 / 11 x64 |
| 宿主 | FaceGaze Bridge（原生模块 ABI v2） |
| 驱动 | PSVR2 驱动 + PlayStation VR2 Toolkit（DriverEx 在运行且启用眼动） |
| 其它 | 关闭 VRCFaceTracking：它的 PSVR2Toolkit 模块会连同一个 IPC 端口 |

## 安装

1. 从 Releases 下载 `psvr2-0.1.0.zip`。
2. FaceGaze Bridge →「模块库 → 原生模块 → 安装原生模块 (.zip)」，或「模块」页「从 .zip 安装」。
3. 「模块」页把 **PlayStation VR2** 设为启动，角色通常给 `eyes` 与 `gaze`。
4. 重启引擎；戴好头显、Toolkit 完成标定后模块卡片应显示活跃。

## 配置

模块目录下可选 `config.ini`：

```ini
host = 127.0.0.1
port = 3364
```

Toolkit 的 IPC 服务只监听本机，一般不需要改。未知键会被记录一条警告，不会静默失效。

## 从源码构建

需要 Visual Studio 2022（x64）、CMake ≥ 3.24 与 Node 20（脚本用）。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

SDK 与 Kit 由 `sdk.lock.json` / `kit.lock.json` 在配置阶段拉取并校验 SHA256，仓库里不含 SDK 头文件副本。
本地想指向已解压的 Kit：

```powershell
cmake -S . -B build -A x64 -DFGB_KIT_DIR="../FGB-Module-Kit"
```

打包与测试：

```powershell
cmake --build build --config Release --target package-modules   # build/Release/psvr2-<版本>.zip
ctest --test-dir build -C Release --output-on-failure           # 协议单测
cmake --build build --config Release --target smoke-test        # 真实 ABI + 合成 TCP 服务端冒烟（需 3364 空闲）
```

## 目录结构

```
src/main.cpp                ABI 生命周期、TCP 客户端（退避重连、后台线程）
src/psvr2_protocol.*        帧头/分帧/解析/清洗/映射（无 winsock 依赖，可单测）
module/                     fgb-native.json 与图标
tests/                      协议单测、真实 ABI 冒烟（内嵌合成 Toolkit）
docs/protocol.md            线格式一手核对结论，逐项标「已确认/未确认」
docs/verification.md        真机验收清单
cmake/                      依赖锁解析（SDK / Kit 都是拉取，不复制）
```

## 排错

| 现象 | 处理 |
|---|---|
| 卡片显示「无法连接 127.0.0.1:3364」 | 确认 Toolkit 的 DriverEx 在运行并启用眼动；重连会自动进行，退避间隔见日志 |
| 握手被拒 Failed/Outdated | Toolkit 版本过旧或未启用眼动；按日志升级 Toolkit |
| 一直提示「还没收到任何数据包」 | 头显未戴好/未标定，眼追没出数据；先在 Toolkit 里确认眼追状态 |
| 日志出现「注视方向映射后不朝 -Z」 | z 轴极化未确认（见 docs/protocol.md）；把日志和 Toolkit 版本提到 issue，勿强行放行 |
| 表情通道没有数据 | 属正常：本模块只有眼追，没有表情来源 |

## 致谢 / Attribution

- 线格式与字段语义一手核对自 [PSVR2Toolkit.VRCFT](https://github.com/BnuuySolutions/PSVR2Toolkit.VRCFT)
  （MIT 许可）；本仓库是该线格式的独立 C++ 实现，未包含其源码。
- ABI 头与宿主：[FaceGaze Bridge](https://github.com/Aqamoe/FaceGaze-Bridge)；共享工具链：[FGB-Module-Kit](https://github.com/Aqamoe/FGB-Module-Kit)。
- PlayStation VR2、PlayStation 是 Sony Interactive Entertainment 的商标；本项目与其无关联。

## 许可

Apache License 2.0，见 [LICENSE](LICENSE) 与 [NOTICE](NOTICE)。
