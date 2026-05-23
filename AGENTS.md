# AGENTS 文档

## 项目范围

本工程是为无源码的波点音乐 Windows 客户端实现 SMTC（System Media Transport Controls）集成的原型。当前方案由两部分组成：

- `mpv_proxy`：生成同名 `libmpv-2.dll`，转发原始 `libmpv_real.dll` 的导出，并采集 mpv 播放状态。
- `bodian_smtc_bridge`：通过命名管道接收代理状态，结合波点日志解析歌曲元数据，并发布到 Windows SMTC。

默认只操作仓库根目录和 `bodian-test` 复制目录。未经明确确认，不得修改 `C:\Program Files (x86)\bodian`。

## 安全边界

- 原始安装目录仅作为读取源，用于生成代理导出表和复制测试目录。
- 部署脚本只准备 `E:\VMShare\board\test\bodian-test\bodian`。
- 禁止直接替换原安装目录下的 `libmpv-2.dll`，除非用户明确要求并再次确认。
- 遇到运行时崩溃、无法启动、SMTC 无会话等问题，优先在复制测试目录复现和修复。
- 高风险操作（删除目录、覆盖原安装、停止用户正在使用的进程）必须先说明目标路径和影响，再取得确认。

## 构建与部署

构建命令：

```powershell
.\scripts\Build.ps1
```

准备复制测试目录：

```powershell
.\scripts\Prepare-BodianTest.ps1
```

测试启动顺序：

```powershell
.\bodian-test\bodian\bodian_smtc_bridge.exe
.\bodian-test\bodian\bodian_pc.exe
```

回退测试目录时，删除测试目录中的代理 `libmpv-2.dll`，再把 `libmpv_real.dll` 改回 `libmpv-2.dll`。

## 关键实现约束

- 代理 DLL 必须保持原 `libmpv-2.dll` 的导出兼容性，包括名称和序号。
- 代理侧常规状态快照频率为 5 秒。
- Bridge 侧 `UpdateTimelineProperties` 也需要做最终节流：播放中约每 5 秒更新一次，暂停、跳转、切歌、时长变化时立即更新。
- SMTC 元数据更新和 timeline 更新应分开判断，避免进度刷新带来元数据重复提交。
- 封面优先走本地缓存文件流：下载到 `%LOCALAPPDATA%\BodianSmtcBridge\album-art`，再通过 `StorageFile` 和 `RandomAccessStreamReference::CreateFromFile` 传给 SMTC。
- 波点日志里的 `.webp` 封面优先改取同路径 `.jpg`，以提高 SMTC 解码成功率。
- 封面缓存清理策略：每小时最多清理一次，删除 3 天未使用的 `.jpg/.png`，最多保留最近使用的 50 个文件。

## 代码规范

- 代码注释和文档使用中文。
- 文件使用 UTF-8 无 BOM。
- 改动保持最小范围，优先延续现有目录结构和 C++/WinRT 实现方式。
- 不引入新依赖，除非当前问题无法通过 Win32、标准库或现有 Windows SDK API 解决。
- 高频路径避免写日志；关键入口、异常和分支决策可以补充简短日志或注释。
- 可恢复错误就近处理；不可恢复错误应清晰失败，禁止空 `catch` 静默吞错。

## 验证要求

涉及 C++ 代码、CMake 或部署脚本的改动后，至少运行：

```powershell
.\scripts\Build.ps1
```

需要更新测试目录时继续运行：

```powershell
.\scripts\Prepare-BodianTest.ps1
```

涉及代理导出或 DLL 加载时，补充最小 smoke test：从 `bodian-test\bodian` 加载 `libmpv-2.dll`，调用转发导出 `mpv_client_api_version()`，期望返回有效版本值。

涉及 Bridge 行为时，至少启动 `bodian_smtc_bridge.exe` 数秒，确认进程可存活。涉及封面缓存时，确认 `%LOCALAPPDATA%\BodianSmtcBridge\album-art` 生成非空图片文件。

## 参考资料

- Microsoft SMTC 手动控制文档：<https://learn.microsoft.com/windows/apps/develop/media-playback/system-media-transport-controls>
- `SystemMediaTransportControlsDisplayUpdater.Thumbnail`：<https://learn.microsoft.com/uwp/api/windows.media.systemmediatransportcontrolsdisplayupdater.thumbnail>
- `RandomAccessStreamReference.CreateFromFile`：<https://learn.microsoft.com/uwp/api/windows.storage.streams.randomaccessstreamreference.createfromfile>
- mpv 客户端 API 文档：<https://mpv.io/manual/master/#client-api>
