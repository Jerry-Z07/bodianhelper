# 波点音乐 SMTC Bridge

为 [波点音乐](https://www.bodian.cn) Windows 客户端实现 SMTC（System Media Transport Controls）集成。

通过同名代理 DLL 替换原 `libmpv-2.dll`，在波点进程内采集 mpv 播放状态，解析应用日志获取歌曲元数据，发布到 Windows SMTC。

## 原理

```
bodian_pc.exe → libmpv-2.dll (代理)
                     ├─ 转发原始导出到 libmpv_real.dll
                     ├─ 采集 mpv 播放状态
                     └─ 启动进程内 Bridge → SMTC
```

## 前置依赖

- Visual Studio（包含 C++ 桌面开发工作负载）
- CMake 3.25+
- Python 3.x

## 使用方式

```powershell
.\scripts\Build.ps1
```

首次运行时自动检测波点安装目录，复制原始 `libmpv-2.dll` 后构建。也可手动将原 DLL 放入 `scripts\` 目录跳过检测。

构建完成后，交付目录 `dist\` 包含：
- `libmpv-2.dll` — 代理 DLL
- `libmpv_real.dll` — 原始 DLL（已重命名）

备份波点安装目录的原 `libmpv-2.dll`，将 `dist\*` 全部复制到安装目录即可。

## 回退

用备份的原始 `libmpv-2.dll` 恢复即可。

## 参考资料

- [SMTC 手动控制](https://learn.microsoft.com/windows/apps/develop/media-playback/system-media-transport-controls)
- [mpv 客户端 API](https://mpv.io/manual/master/#client-api)
