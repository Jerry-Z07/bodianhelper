param(
    [string]$Configuration = "RelWithDebInfo",
    [string]$BuildDir = "build"
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildPath = Join-Path $root $BuildDir
$scriptsDir = $PSScriptRoot
$originalDll = Join-Path $scriptsDir "libmpv-2.dll"

function FindBodianInstallDir() {
    $candidates = @(
        "${env:ProgramFiles(x86)}\bodian",
        "${env:ProgramFiles}\bodian"
    )
    foreach ($dir in $candidates) {
        if (Test-Path (Join-Path $dir "libmpv-2.dll")) {
            return $dir
        }
    }
    return $null
}

if (-not (Test-Path -LiteralPath $originalDll)) {
    $bodianDir = FindBodianInstallDir
    if (-not $bodianDir) {
        do {
            $input = Read-Host "未自动检测到波点音乐安装目录，请输入路径"
            $input = $input.Trim()
        } while ([string]::IsNullOrEmpty($input))
        $bodianDir = $input
    }

    $sourceDll = Join-Path $bodianDir "libmpv-2.dll"
    if (-not (Test-Path -LiteralPath $sourceDll)) {
        throw "指定目录中未找到 libmpv-2.dll: $bodianDir"
    }

    Write-Host "已复制原始 DLL: $sourceDll → $originalDll"
    Copy-Item -LiteralPath $sourceDll -Destination $originalDll
}

cmake -S $root -B $buildPath -A x64 "-DBODIAN_ORIGINAL_DIR=$scriptsDir"
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码: $LASTEXITCODE"
}

cmake --build $buildPath --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "构建失败，退出码: $LASTEXITCODE"
}

$outputDll = Join-Path $buildPath "bin\$Configuration\libmpv-2.dll"

$distDir = Join-Path $root "dist"
if (Test-Path -LiteralPath $distDir) {
    Remove-Item -Recurse -Force $distDir
}
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
Copy-Item -LiteralPath $outputDll -Destination (Join-Path $distDir "libmpv-2.dll")
Copy-Item -LiteralPath $originalDll -Destination (Join-Path $distDir "libmpv_real.dll")

Write-Host "构建完成"
Write-Host "交付目录: $distDir"
Write-Host "其中包含:"
Write-Host "  libmpv-2.dll   (代理 DLL)"
Write-Host "  libmpv_real.dll (原始 DLL，已重命名)"
Write-Host ""
Write-Host "使用方式: 备份波点安装目录中的 libmpv-2.dll，将 dist\* （即该文件夹下所有的dll）复制到安装目录"