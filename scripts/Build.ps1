param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$BuildDir = "build",
    [string]$BodianOriginalDir = "C:\Program Files (x86)\bodian"
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildPath = Join-Path $root $BuildDir
$vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "未找到 vcvars64.bat: $vcvars"
}

if (-not (Test-Path -LiteralPath (Join-Path $BodianOriginalDir "libmpv-2.dll"))) {
    throw "未找到原始 libmpv-2.dll: $BodianOriginalDir"
}

$cmakeConfigure = "cmake -S `"$root`" -B `"$buildPath`" -G `"Visual Studio 18 2026`" -A x64 -DBODIAN_ORIGINAL_DIR=`"$BodianOriginalDir`""
$cmakeBuild = "cmake --build `"$buildPath`" --config $Configuration"

cmd /c "`"$vcvars`" && $cmakeConfigure && $cmakeBuild"
if ($LASTEXITCODE -ne 0) {
    throw "构建失败，退出码: $LASTEXITCODE"
}

Write-Host "构建完成: $(Join-Path $buildPath 'bin')"
