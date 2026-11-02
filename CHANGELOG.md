# 更新日志

遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与语义化版本。

## [未发布]

## [0.1.0] - 未发布
### 新增
- PSVR2 Toolkit IPC（TCP 127.0.0.1:3364）→ 逐眼注视方向、睁眼度与瞳孔直径。
- 握手协商 IPC 版本（v1=66 字节 / v2=76 字节回包），按 8ms 节奏轮询（约 120Hz）。
- 断线指数退避重连（0.5s 起、10s 封顶）+ 限频告警，Toolkit 重启后自动恢复并重新握手。
- NaN/Inf 清洗、越界瞳孔丢弃、极化可疑方向 fail-safe：无效字段一律不置属性位。
- `config.ini` 支持覆盖 host/port。
- 字节级协议单测 + 内嵌合成 TCP 服务端的真实 ABI 冒烟（含断线重连用例）。

### 已知限制
- 未在真机（PSVR2 Toolkit DriverEx）验证，状态为未验证。
- `gazeDirNorm` 的 z 轴极化未经驱动端一手确认，采用「Unity +Z 前」假设并带 fail-safe，
  见 docs/protocol.md。
