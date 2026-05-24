#!/usr/bin/env pwsh
# bootstrap.ps1 — 新手引导
# 检查前置依赖，引导完成构建。

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")

function Step([string]$label) {
    Write-Host ""
    Write-Host "=== $label ===" -ForegroundColor Cyan
}

function CheckCommand([string]$name, [string]$reason) {
    $path = Get-Command $name -ErrorAction SilentlyContinue
    if (-not $path) {
        Write-Host "[x] $name — 未找到" -ForegroundColor Red
        Write-Host "    $reason" -ForegroundColor Yellow
        return $false
    }
    Write-Host "[v] $name ($($path.Source))" -ForegroundColor Green
    return $true
}

Write-Host ""
Write-Host "波点音乐 SMTC Bridge — 环境检查" -ForegroundColor Magenta
Write-Host ("=" * 50) -ForegroundColor Magenta

Step "1. 前置依赖检查"
$cmakeOk = CheckCommand "cmake" "从 https://cmake.org/download 下载安装"
$pythonOk = CheckCommand "python" "从 https://www.python.org/downloads 下载安装"
$pwshVer = $PSVersionTable.PSVersion
Write-Host "[v] PowerShell $($pwshVer.Major).$($pwshVer.Minor)" -ForegroundColor Green

Step "2. Visual Studio 检查"
try {
    $vsOutput = cmake --help 2>$null | Select-String "Visual Studio"
    $hasVs = $vsOutput -ne $null
    if ($hasVs) {
        Write-Host "[v] 检测到 Visual Studio 生成器" -ForegroundColor Green
    } else {
        Write-Host "[?] 未检测到 Visual Studio 生成器" -ForegroundColor Yellow
        Write-Host "    确保安装了含 C++ 桌面开发工作负载的 Visual Studio" -ForegroundColor Yellow
    }
} catch {
    Write-Host "[?] 无法检测 VS 生成器" -ForegroundColor Yellow
}

Step "3. 原始 DLL 检查"
$originalDll = Join-Path $PSScriptRoot "libmpv-2.dll"
if (Test-Path $originalDll) {
    Write-Host "[v] scripts\libmpv-2.dll 已就绪" -ForegroundColor Green
    $dllReady = $true
} else {
    Write-Host "[ ] scripts\libmpv-2.dll 未找到" -ForegroundColor Yellow
    Write-Host "    请从波点音乐安装目录复制原始 libmpv-2.dll 到 scripts\ 目录" -ForegroundColor Yellow
    $dllReady = $false
}

Step "4. 汇总"
$allOk = $cmakeOk -and $pythonOk

if (-not $allOk) {
    Write-Host "缺少必要依赖，请按上述提示安装" -ForegroundColor Red
    exit 1
}

if ($dllReady) {
    Write-Host "一切就绪，运行构建:" -ForegroundColor Green
    Write-Host "  .\scripts\Build.ps1" -ForegroundColor Gray
    Write-Host ""
    Write-Host "构建产物输出到 dist\ 目录:" -ForegroundColor Gray
    Write-Host "  libmpv-2.dll   (代理 DLL)" -ForegroundColor Gray
    Write-Host "  libmpv_real.dll (原始 DLL，已重命名)" -ForegroundColor Gray
    Write-Host "使用方式: 将 dist\* 复制到波点安装目录替换原 libmpv-2.dll" -ForegroundColor Gray
} else {
    Write-Host "依赖检查通过，放入原始 DLL 后运行构建:" -ForegroundColor Green
    Write-Host "  1. 从波点安装目录复制 libmpv-2.dll 到 scripts\" -ForegroundColor White
    Write-Host "  2. .\scripts\Build.ps1" -ForegroundColor Gray
    Write-Host "  3. 将 dist\* 复制到波点安装目录替换原 libmpv-2.dll" -ForegroundColor Gray
}