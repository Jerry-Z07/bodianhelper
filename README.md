# 波点音乐 SMTC Bridge 原型

该工程为无源码波点音乐实现 SMTC 的原型：

- `mpv_proxy`：同名代理 `libmpv-2.dll`，转发原 DLL，采集 mpv 状态，并在波点进程内发布 Windows SMTC。
- 进程内 Bridge：随播放引擎初始化启动，异步绑定波点主窗口，结合波点日志补齐歌曲信息。
- 部署脚本只准备复制出来的测试目录，不修改原安装目录。

## 构建

```powershell
.\scripts\Build.ps1
```

## 准备测试目录

```powershell
.\scripts\Prepare-BodianTest.ps1
```

## 启动

```powershell
.\bodian-test\bodian\bodian_pc.exe
```

## 回退测试目录

在测试目录中删除代理 `libmpv-2.dll`，再把 `libmpv_real.dll` 改回 `libmpv-2.dll`。
