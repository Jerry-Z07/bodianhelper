param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$BuildDir = "build",
    [string]$SourceDir = "C:\Program Files (x86)\bodian",
    [string]$TestRoot = "E:\VMShare\board\test\bodian-test"
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildBin = Join-Path $root "$BuildDir\bin\$Configuration"
$testBodian = Join-Path $TestRoot "bodian"

if (-not (Test-Path -LiteralPath (Join-Path $buildBin "libmpv-2.dll"))) {
    throw "未找到代理 DLL，请先运行 scripts\Build.ps1"
}

if (-not (Test-Path -LiteralPath $testBodian)) {
    New-Item -ItemType Directory -Force -Path $TestRoot | Out-Null
    robocopy $SourceDir $testBodian /MIR /R:2 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) {
        throw "复制波点测试目录失败，robocopy 退出码: $LASTEXITCODE"
    }
}

$realMpv = Join-Path $testBodian "libmpv_real.dll"
$originalMpv = Join-Path $testBodian "libmpv-2.dll"

if (-not (Test-Path -LiteralPath $realMpv)) {
    if (-not (Test-Path -LiteralPath $originalMpv)) {
        throw "测试目录缺少 libmpv-2.dll: $originalMpv"
    }
    Rename-Item -LiteralPath $originalMpv -NewName "libmpv_real.dll"
}

Copy-Item -LiteralPath (Join-Path $buildBin "libmpv-2.dll") -Destination $originalMpv -Force

Write-Host "测试目录已准备: $testBodian"
Write-Host "启动:"
Write-Host "  $testBodian\bodian_pc.exe"
