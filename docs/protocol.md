# PSVR2 Toolkit IPC 协议要点

一手核对来源：上游 VRCFT 模块 **PSVR2Toolkit.VRCFT**（Bnuuy Solutions，MIT 许可）的
`IpcProtocol.cs` / `IpcClient.cs` / `Psvr2TrackingModule.cs`。本模块是该线格式的独立
C++ 实现，未复制上游源码。C# 结构体的实际序列化布局（含填充）用
`Marshal.SizeOf/OffsetOf` 在本机逐一验证。

对端服务器是 **PlayStation VR2 Toolkit 的 DriverEx**（驱动侧 C++ 实现），其源码不在
上游仓库内，凡依赖服务端行为才能确认的条目单独标注。

## 连接

| 项 | 值 | 状态 |
|---|---|---|
| 传输 | TCP | 已确认（`IpcClient.cs` 用 `TcpClient`） |
| 地址/端口 | **127.0.0.1:3364**（`IPC_SERVER_PORT = 3364`） | 已确认 |
| 客户端 IPC 版本 | `k_unIpcVersion = 2` | 已确认 |
| 轮询节奏 | 客户端线程每 8ms 发一次 gaze 请求（约 120Hz） | 已确认（`m_gazePumpPeriodMs = 8`） |

## 帧头（与常见摘要的差异点）

每条消息 = 帧头 + 负载。帧头是 **8 字节**，不是「u16 + i32」紧凑的 6 字节：

```text
偏移 0  u16 type        小端
偏移 2  2 字节填充      C# 默认 Sequential 对齐产生；上游发送时不初始化，接收侧忽略
偏移 4  i32 data_len    小端
```

- 8 字节帧头与填充：已确认（`Marshal.SizeOf<CommandHeader>() == 8`、
  `OffsetOf("dataLen") == 4`，实测）。
- 字节序为小端：已确认（Windows x64 上 `Marshal` 按本机序拷贝内存）。

## 消息类型

`ECommandType : ushort` 未显式赋值，取值即声明顺序：

| 值 | 名称 | 方向 | 状态 |
|---|---|---|---|
| 0 | ClientPing | C→S，空载 | 已确认 |
| 1 | ServerPong | S→C，空载 | 已确认（存在性） |
| 2 | ClientRequestHandshake | C→S，8 字节负载 | 已确认 |
| 3 | ServerHandshakeResult | S→C，4 字节负载 | 已确认 |
| 4 | ClientRequestGazeData | C→S，空载 | 已确认 |
| 5 | ServerGazeDataResult | S→C，66 或 76 字节 | 已确认 |
| 6..12 | ClientTriggerEffect*（振动器） | C→S | 已确认，本模块不实现 |

## 握手

请求负载（8 字节，`Marshal` 默认对齐，已实测）：

```text
偏移 0  u16 ipc_version   客户端声明版本（2）
偏移 2  2 字节填充
偏移 4  u32 process_id
```

响应负载（4 字节，已实测）：

```text
偏移 0  u8 result         0=Failed 1=Success 2=Outdated
偏移 1  1 字节填充
偏移 2  u16 ipc_version   服务端版本
```

客户端规则（已确认，`HandleIpcCommand`）：响应长度必须恰为 4；`ipc_version`
被保存，**仅当恰等于 1 时**眼追按 66 字节解析，其余按 76 字节。握手失败/过旧时
上游只记日志、不断开也不重发——本模块同样继续轮询，由服务端回不回数据决定。

握手是否要求「先发 Ping」：未确认（上游连接后直接握手，未见必须 Ping 的路径）。

## 眼追轮询与回包

请求：`type=4, data_len=0`，无序号、无关联 id。响应与请求**不配对**：
上游收到任何一条 `ServerGazeDataResult` 都覆盖 `m_lastGazeState`，消费端取
「最新一帧」。请求/响应靠 TCP 连接本身与固定长度隐式对齐（已确认客户端行为）。

回包负载 `CommandDataServerGazeDataResult(2)`：左右眼各一个 Pack=1 结构，
**无填充、小端**（尺寸 33/38 已实测）：

```text
GazeEyeResult（v1，33 字节）           GazeEyeResult2（v2，38 字节 = v1 + 尾部 5 字节）
偏移 0   u8 isGazeOriginValid          偏移 33 u8 isOpenEnabled
偏移 1   f32 gazeOriginMm.x (毫米)      偏移 34 f32 open (0..1)
偏移 5   f32 gazeOriginMm.y
偏移 9   f32 gazeOriginMm.z            右眼起始：v1=33，v2=38（OffsetOf 实测）
偏移 13  u8 isGazeDirValid
偏移 14  f32 gazeDirNorm.x
偏移 18  f32 gazeDirNorm.y
偏移 22  f32 gazeDirNorm.z
偏移 26  u8 isPupilDiaValid
偏移 27  f32 pupilDiaMm (毫米)
偏移 31  u8 isBlinkValid
偏移 32  u8 blink (0/1)
```

布尔按 `UnmanagedType.I1`（1 字节，非 0 即真）解析——已确认自 C# 声明。
总长 v1=66、v2=76：已确认（实测 `Marshal.SizeOf`）。服务端是否还会下发更老的
66 字节包取决于 DriverEx 版本：未确认。

## 单位与极化（映射到 FGB ABI 的依据）

| 字段 | 上游证据 | 本模块处理 | 状态 |
|---|---|---|---|
| `pupilDiaMm` | VRCFT 直写 `PupilDiameter_MM`（毫米），FGB ABI 的 `pupil_diameter_mm` 同为毫米 | **不换算**，仅校验 (0,20] 区间 | 单位已确认；区间是防御性选择 |
| `gazeOriginMm` | 上游 VRCFT 模块**未使用**该字段 | 解析保留但 FGB 帧槽无对应字段，不输出 | 已确认（未使用） |
| `gazeDirNorm` x/y | 上游用 `(x, y)` 后 `FlipXCoordinates()`（即 `-x, y`）→ x 轴与 Unity/VRCFT 空间相反 | 映射 x 取反 | x 极化已确认 |
| `gazeDirNorm` z | C# 客户端不消费 z，服务端坐标系（Unity +Z 前 还是 OpenXR -Z 前）无法从客户端源码确认 | 按「Unity +Z 前」映射为 `(-x, y, -z)`；映射后 z 仍不朝前（≥ -0.05）判为无效并告警，**不伪造方向** | **未确认**，带 fail-safe |
| blink→openness | 上游 `isOpenEnabled ? open : (blink ? 0 : 1)`，再过 15 参数 LowPassFilter（内部窗长 14） | 同一公式；同窗长滑动平均，但首值种子（上游实现从 0 起步会在启动期把睁眼度压到 0） | 已确认（公式），滤波为等效实现 |

「无效帧不置属性位」：各 `is*Valid` 为假、值非有限（NaN/Inf）、长度为零的方向、
越界瞳孔，一律丢弃对应属性位，绝不写 0 冒充「闭眼/看正前方」。

## 断线重连语义

- 上游客户端**没有**自动重连：`Start()` 只在模块 Initialize 时调一次，接收线程
  退出后 `m_running` 仍为真，`RequestEyeTrackingData` 会**无限返回最后一帧陈旧数据**
  （已确认，这被认为是上游缺陷而非特性）。
- 本模块的重连策略是自定的改进项（未确认对应服务端行为）：连接失败按
  0.5s→1s→2s→4s→8s→10s 封顶指数退避；首败告警一次、之后每 30 秒最多一条；
  recv 得到 0（服务端关闭）或出错即断开重连；重连成功后**重新走完整握手**。
- DriverEx 对同一 PID 重复连接、或多客户端并发轮询的行为：未确认。
- 追踪丢失/未标定时服务端是继续回包（valid 位为假）还是停发：未确认，
  本模块两种情况都能安全处理（属性位不置 / 断流提示）。

## 版本与兼容

- 客户端 v2 ↔ 服务端 v1：回包按 66 字节解析，v2 独有的 `open` 不存在，
  睁眼度退化为 blink 二值（上游同款逻辑）。
- 服务端回 `Outdated`：本模块告警并继续（与上游一致），没有 v1 回退路径——
  真机若遇到需要补实现。
