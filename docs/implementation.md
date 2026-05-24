# 波点音乐 SMTC Bridge 实现原理

## 概述

本工程为波点音乐 Windows 客户端实现 SMTC（System Media Transport Controls）集成。通过**同名代理 DLL** 替换原始 `libmpv-2.dll`，在波点进程内部拦截 mpv 播放引擎的 API 调用，采集播放状态并从应用日志解析元数据，最终发布到 Windows SMTC。

## 架构

```
bodian_pc.exe → libmpv-2.dll (代理)
                     ├─ DllMain: 加载 libmpv_real.dll
                     ├─ mpv_create/mpv_initialize → 记录活跃 handle → 启动 Bridge
                     ├─ mpv_command/mpv_set_property → 转发后触发状态快照
                     ├─ mpv_wait_event → 转发后触发节流快照
                     ├─ mpv_destroy → 清理 handle → 标记断开
                     │
                     └─ 进程内 Bridge 线程
                          ├─ 每 500ms 循环
                          │    ├─ 扫描波点日志 (bdlog/*.log) 解析元数据
                          │    ├─ 确保 SMTC 绑定到主窗口
                          │    └─ 根据播放状态差异推送 SMTC 更新
                          └─ SMTC 事件回调
                               ├─ Play/Pause → SetProperty("pause")
                               ├─ Next/Previous → keybd_event 模拟媒体键
                               └─ Seek → mpv_command("seek")
```

## 模块拆解

### 1. 代理导出生成 (`tools/generate_proxy_def.py`)

从原始 `libmpv-2.dll` 解析 PE 导出表，输出三份代码：

- **`.def` 文件**：列出 DLL 所有导出符号（名称+序号），使链接器生成与原始 DLL 完全兼容的导出表。
- **`.asm` 文件（转发 thunk）**：对**不拦截**的导出生成汇编跳转桩（`jmp QWORD PTR [p_xxx]`），通过函数指针表间接跳转到真实实现。
- **`.cpp` 表文件**：声明函数指针变量并定义 `InitializeForwardedExports`，在运行时从 `libmpv_real.dll` 解析并填充这些指针。

**拦截集**（需代理自定义行为的导出）：

`mpv_create`, `mpv_create_client`, `mpv_create_weak_client`, `mpv_initialize`, `mpv_destroy`, `mpv_terminate_destroy`, `mpv_command`, `mpv_command_async`, `mpv_command_string`, `mpv_set_property`, `mpv_set_property_string`, `mpv_get_property`, `mpv_get_property_string`, `mpv_wait_event`, `mpv_wakeup`, `mpv_free`。

这些函数在 `mpv_proxy.cpp` 中有对应的 `__declspec(dllexport)` 实现。

### 2. 代理 DLL (`src/proxy/mpv_proxy.cpp`)

**DLL 生命周期管理：**

| 事件 | 行为 |
|---|---|
| `DLL_PROCESS_ATTACH` | 调用 `LoadRealModule()`，加载同目录下的 `libmpv_real.dll`，填充转发函数表 |
| `DLL_PROCESS_DETACH` | 设置 `g_process_exiting`，调用 `StopSmtcBridge()` |

**核心数据结构：**

- `g_active_handle`：当前活跃的 `mpv_handle*`，在 `mpv_create` 系列和 `mpv_initialize` 成功时更新，`mpv_destroy` 时清空。
- `g_last_snapshot_ms`：上次状态快照时间戳，用于节流。

**关键拦截逻辑：**

- **`mpv_create` / `mpv_create_client` / `mpv_create_weak_client`**：调用真实函数后记录 `g_active_handle`。
- **`mpv_initialize`**：成功后调用 `EnsureBridge()`（启动 Bridge 线程）+ `EnsureSampler()`（启动后台采样线程），随后触发 `NotifyEvent`。
- **`mpv_command` / `mpv_command_async` / `mpv_command_string` / `mpv_set_property` / `mpv_set_property_string`**：转发后调用 `NotifyEvent` 触发快照。
- **`mpv_wait_event`**：转发后调用 `SendStateSnapshotThrottled`，利用 `compare_exchange_weak` 实现 5 秒节流，避免高频事件导致过量快照。
- **`mpv_destroy` / `mpv_terminate_destroy`**：清空 handle 后通过 `NotifyEvent(nullptr)` 标记断开。

**状态采样：**

- `SendStateSnapshot`：从 mpv handle 读取 `pause`、`idle-active`、`eof-reached`、`time-pos`、`duration`、`speed`、`media-title`、`path`、`filename` 等属性，构造 `PlaybackState` 后调用 `SubmitPlaybackState`。
- `SamplerWorker`：独立后台线程，每 5 秒无条件采样一次（兜底保活）。
- `SendSyntheticMediaKey`：模拟 `keybd_event` 发送 `VK_MEDIA_NEXT_TRACK` / `VK_MEDIA_PREV_TRACK`，带 500ms 防抖。这是因波点客户端的 Dart 侧媒体键监听只响应真实键盘事件。

### 3. SMTC Bridge (`src/bridge/bridge_core.cpp`)

**生命周期：**

```
StartSmtcBridge(callbacks)
  └─ g_running = true, 启动 BridgeWorker 线程

BridgeWorker (STA 线程)
  └─ 每 500ms 循环：
       ├─ EnsureSmtcBoundToMainWindow()
       ├─ 每 2s 解析一次日志元数据
       └─ 状态更新由 SubmitPlaybackState / UpdateSmtc 推动

StopSmtcBridge()
  └─ g_running = false → 线程退出 → ResetSmtcLocked()
```

**SMTC 绑定窗口策略：**

`EnsureSmtcBoundToMainWindow` 通过 `EnumWindows` 遍历当前进程所有顶层窗口，筛选条件：
- 属当前进程、`WS_VISIBLE`、无 owner、非 `WS_EX_TOOLWINDOW`
- 面积（`width × height`）最大

成功后调用 `ISystemMediaTransportControlsInterop::GetForWindow` 绑定到该窗口。

**SMTC 事件处理：**

| SMTC 按钮 | 回调行为 |
|---|---|
| Play | `SetProperty("pause", false)` |
| Pause | `SetProperty("pause", true)` |
| Next | `SendSyntheticMediaKey(VK_MEDIA_NEXT_TRACK)` |
| Previous | `SendSyntheticMediaKey(VK_MEDIA_PREV_TRACK)` |
| Seek (PositionChangeRequested) | `mpv_command("seek", seconds, "absolute", "exact")` |

**元数据解析：**

- `NewestLogFile`：在 `%LOCALAPPDATA%\cn.wenyu.bodian\bodian_pc\bdlog\` 下找最新 `.log` 文件。
- `ReadTail`：读取文件末尾 2MB 内容。
- `ParseMetadataFromLogs`：逆向搜索 `"albumPic:"` 定位当前歌曲日志行，用 `ExtractBetween` 提取 `name`、`album`、`albumPic`、`artist`、`duration` 字段。`name` 提取失败时降级尝试 `songName`。
- `MergeMetadata`：将日志解析的元数据合并到 `g_state`，仅在字段非空且不同于缓存值时触发 `UpdateSmtc()`。

**封面处理：**

- 封面 URL 优先从 `.webp` 替换为 `.jpg`（路径末尾 5 字符替换），以提高 SMTC 解码兼容性。
- `DownloadUrl` 通过 `URLOpenBlockingStreamW` 下载封面数据。
- `CoverThumbnailReference` 使用 WinRT `BitmapDecoder` 解码后经 `BitmapEncoder` 重编码为 JPEG 内存流，最后通过 `RandomAccessStreamReference::CreateFromStream` 设置到 SMTC。

**发布节流策略：**

采用 `SmtcPublishCache` 追踪已发布状态，三类更新各自独立判断：

| 更新类型 | 触发条件 | 说明 |
|---|---|---|
| `PlaybackStatus` | `playing` 状态变化 | 立即更新 |
| `DisplayUpdater`（元数据） | `display_key`（title+artist+album+album_pic+filename）变化 | 避免进度刷新导致元数据重复提交 |
| `TimelineProperties` | 切歌、播放/暂停切换、时长偏差 > 1s、位置偏差 > 3s、或距上次更新超过 5s | 播放中每 5s 兜底，关键事件立即更新 |

异常处理：SMTC 操作抛异常时调用 `ResetSmtcLocked()` 清理状态，下次循环重新绑定。

### 4. 构建与部署 (`scripts/Build.ps1`)

```powershell
.\scripts\Build.ps1
```

流程：
1. 检测 `scripts\libmpv-2.dll` 是否存在，否则自动搜索波点安装目录或提示用户指定路径，复制原始 DLL。
2. 调用 CMake 配置 x64 构建。
3. 构建时 `generate_proxy_def.py` 从原始 DLL 解析导出并生成代理代码。
4. 编译 `mpv_proxy` 目标，输出 `build\bin\RelWithDebInfo\libmpv-2.dll`。
5. 打包到 `dist\` 目录（含代理 `libmpv-2.dll` + 重命名的原始 `libmpv_real.dll`）。

### 5. 数据流全景

```
用户操作 / 自动播放
       │
       ▼
波点客户端 → mpv C API → libmpv-2.dll (代理)
                                │
                    ┌───────────┼───────────┐
                    ▼           ▼           ▼
           转发到真实     拦截关键 API     Bridge 线程
         libmpv_real.dll  采集状态         │
                    │        │         解析日志 → 元数据
                    │        ▼             │
                    │   PlaybackState      │
                    │        │             │
                    │        ▼             │
                    │   SubmitPlaybackState │
                    │        │             │
                    │        ▼             ▼
                    │   UpdateSmtc() ──→ MergeMetadata
                    │        │
                    │        ▼
                    │   SMTC 发布
                    │   (Status / Display / Timeline)
                    │
                    ▼
             波点窗口响应媒体键
             (SMTC → 模拟键盘 → 波点 Dart 层)
```

## 关键技术要点

1. **DLL 代理模式**：利用 PE 导出表兼容性，以同名 DLL 劫持加载链，无需注入或 Hook。原始 DLL 仅重命名，不做任何修改。
2. **进程内无额外进程**：Bridge 作为 DLL 内线程运行，通过 WinRT STA 与 SMTC 交互。
3. **日志解析而非 IPC**：波点日志是获取歌曲元数据（标题、艺术家、专辑、封面）的唯一可靠来源，因为 mpv 属性中只暴露 `media-title` 和文件路径。日志解析通过逆向搜索定位当前曲目行。
4. **媒体键模拟**：波点前端使用 Flutter/Dart 监听系统媒体键，SMTC 直接控制无效，因此退而模拟键盘事件。
5. **封面重编码**：SMTC 对封面格式有限制，`.webp` 解码兼容性差，流程为 `下载 → BitmapDecoder → JPEG BitmapEncoder → 内存流 → RandomAccessStreamReference`。
