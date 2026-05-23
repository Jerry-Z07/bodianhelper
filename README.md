# 波点音乐 SMTC Bridge 原型

该工程为无源码波点音乐实现 SMTC 的原型：

- `mpv_proxy`：同名代理 `libmpv-2.dll`，转发原 DLL 并采集 mpv 状态。
- `bodian_smtc_bridge`：接收代理状态，结合波点日志补齐歌曲信息，并发布 Windows SMTC。
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
.\bodian-test\bodian\bodian_smtc_bridge.exe
.\bodian-test\bodian\bodian_pc.exe
```

## 回退测试目录

在测试目录中删除代理 `libmpv-2.dll`，再把 `libmpv_real.dll` 改回 `libmpv-2.dll`。
