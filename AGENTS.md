# AGENTS 文档

## 项目范围

本工程是为无源码的波点音乐 Windows 客户端实现 SMTC（System Media Transport Controls）集成的原型。当前方案由两部分组成：

- `mpv_proxy`：生成同名 `libmpv-2.dll`，转发原始 `libmpv_real.dll` 的导出，并采集 mpv 播放状态。
- 进程内 Bridge：由代理 DLL 在波点进程内启动，结合波点日志解析歌曲元数据，并发布到 Windows SMTC。

操作范围仅限于仓库目录。用户自行将原始 `libmpv-2.dll` 复制到 `scripts/` 目录供构建使用（首次运行 Build.ps1 时自动检测并复制）。

## 安全边界

- 原始 DLL 仅放置在 `scripts/` 目录作为构建输入，仓库不追踪该文件。
- 构建产物是代理 `libmpv-2.dll`，由用户自行备份原文件后替换。
- 修改代理来源（如 CMakeList.txt 中 BODIAN_ORIGINAL_DIR 默认值）必须先确认。
- 遇到运行时崩溃、无法启动、SMTC 无会话等问题，用户应自行在测试环境中复现。

## 构建

```powershell
.\scripts\Build.ps1
```

构建产物位于 `build\bin\RelWithDebInfo\libmpv-2.dll`，同时打包到 `dist\` 目录（含 `libmpv_real.dll`）。

## 关键实现约束

- 代理 DLL 必须保持原 `libmpv-2.dll` 的导出兼容性，包括名称和序号。
- 代理侧常规状态快照频率为 5 秒。
- Bridge 侧 `UpdateTimelineProperties` 也需要做最终节流：播放中约每 5 秒更新一次，暂停、跳转、切歌、时长变化时立即更新。
- Bridge 在 `mpv_initialize` 成功后启动，随波点进程退出而关闭。
- SMTC 绑定当前进程内可见、无 owner、非工具窗口、面积最大的顶层窗口。
- SMTC 元数据更新和 timeline 更新应分开判断，避免进度刷新带来元数据重复提交。
- 封面需要时直接获取并转换为 JPEG 内存流，再通过 `RandomAccessStreamReference::CreateFromStream` 传给 SMTC。
- 波点日志里的 `.webp` 封面优先改取同路径 `.jpg`，以提高 SMTC 解码成功率。

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

涉及代理导出或 DLL 加载时，确认构建产物 `build\bin\RelWithDebInfo\libmpv-2.dll` 可被依赖链加载（即 `libmpv_real.dll` 位于同一目录）。

## 参考资料

- Microsoft SMTC 手动控制文档：<https://learn.microsoft.com/windows/apps/develop/media-playback/system-media-transport-controls>
- `SystemMediaTransportControlsDisplayUpdater.Thumbnail`：<https://learn.microsoft.com/uwp/api/windows.media.systemmediatransportcontrolsdisplayupdater.thumbnail>
- `RandomAccessStreamReference.CreateFromStream`：<https://learn.microsoft.com/uwp/api/windows.storage.streams.randomaccessstreamreference.createfromstream>
- WinRT 图像解码与编码：<https://learn.microsoft.com/windows/apps/develop/media-authoring-processing/imaging>
- mpv 客户端 API 文档：<https://mpv.io/manual/master/#client-api>
