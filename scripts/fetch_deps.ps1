# Fetches third-party dependencies into external/ (not tracked; see .gitignore)
#
# Why not use the default CMake FetchContent behavior:
#   FetchContent goes online during the **configure phase**. Offline development, CI
#   caches, and "configure succeeds only if the network does" are unacceptable; so
#   fetching is **explicit and one-shot**, and configure only probes locally.
#
# Usage:
#   pwsh scripts/fetch_deps.ps1                  # fetch the missing dependencies
#   pwsh scripts/fetch_deps.ps1 -Force           # force a re-fetch
#   pwsh scripts/fetch_deps.ps1 -WithQt          # ...and Qt, into external/Qt
#   pwsh scripts/fetch_deps.ps1 -WithQt -QtSource vcpkg
#
# Qt is opt-in because it is a GUI dependency: `core` builds and passes every test
# with Qt absent entirely, and that property is worth keeping visible in the
# default path rather than hidden behind a download nobody needed.
#
# Qt cannot be built with this project's GCC toolchain, and that is not a choice
# made here: **there is no 32-bit Qt 6**. The GUI is a 64-bit MSVC target.
#
# Versions are **pinned** once chosen: a Catch2 upgrade edits this file and says why in the PR.
[CmdletBinding()]
param(
    [switch]$Force,
    [switch]$WithQt,
    [ValidateSet("aqt", "vcpkg")]
    [string]$QtSource = "aqt",
    [string]$QtVersion = "6.8.3",
    [string]$QtKit = "win64_msvc2022_64",
    [string]$QtRoot = "external/Qt"
)

$ErrorActionPreference = "Stop"

# Runs a native command and returns its exit code, tolerating stderr output.
#
# PowerShell 5.1 turns a native command's stderr into a terminating error under
# `$ErrorActionPreference = "Stop"`, so a command that merely *logs* to stderr
# aborts the script. `pip install` reports progress that way, git reports clone
# progress that way, and windeployqt warns that way -- all three have killed a run
# in this repository at some point.
#
# The preference is therefore lowered for the duration of the call and the exit code
# is what decides. Output is captured and printed only on failure, because that is
# when it is worth reading.
function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $Exe @Arguments 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) {
        $output | Select-Object -Last 20 | ForEach-Object { Write-Host "    $_" }
    }
    return $code
}

# -- Dependency manifest (single source of truth) -----------------------------
$Deps = @(
    @{
        Name    = "Catch2"
        Url     = "https://github.com/catchorg/Catch2.git"
        Tag     = "v3.5.2"
        Path    = "external/Catch2"
        Check   = "CMakeLists.txt"
        Purpose = "单元测试与性质测试框架"
    }
)

$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $repoRoot
try {
    foreach ($dep in $Deps) {
        $target = Join-Path $repoRoot $dep.Path
        $probe  = Join-Path $target $dep.Check

        if ((Test-Path $probe) -and (-not $Force)) {
            $tag = (git -C $target describe --tags 2>$null)
            Write-Host "[skip]  $($dep.Name) 已存在 ($tag)"
            continue
        }

        if (Test-Path $target) {
            Write-Host "[clean] $($dep.Name) 移除旧副本"
            Remove-Item -Recurse -Force $target
        }

        Write-Host "[fetch] $($dep.Name) @ $($dep.Tag)  <- $($dep.Url)"
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        $code = Invoke-Native git clone --quiet --depth 1 --branch $dep.Tag $dep.Url $target
        if ($code -ne 0) {
            throw "拉取 $($dep.Name) 失败（退出码 $code）"
        }

        if (-not (Test-Path $probe)) {
            throw "$($dep.Name) 拉取后缺少 $($dep.Check)，副本不完整"
        }

        $tag = (git -C $target describe --tags 2>$null)
        Write-Host "[ok]    $($dep.Name) -> $tag  ($($dep.Purpose))"
    }

    if ($WithQt) {
        Write-Host ""
        Write-Host "=== Qt $QtVersion ($QtSource) ===" -ForegroundColor Cyan
        $qtTarget = Join-Path $repoRoot $QtRoot
        $qtProbe  = Join-Path $qtTarget "$QtVersion/msvc2022_64/lib/cmake/Qt6/Qt6Config.cmake"

        if ((Test-Path $qtProbe) -and (-not $Force)) {
            Write-Host "[skip]  Qt 已存在于 $qtTarget"
        }
        elseif ($QtSource -eq "aqt") {
            # aqtinstall downloads Qt's own prebuilt kit: no Qt account, no
            # interactive installer, and it takes minutes rather than the hours a
            # from-source build needs.
            Write-Host "[fetch] Qt $QtVersion $QtKit -> $QtRoot"
            $code = Invoke-Native python -m pip install --quiet --upgrade aqtinstall
            if ($code -ne 0) { throw "安装 aqtinstall 失败（退出码 $code）" }

            New-Item -ItemType Directory -Force -Path $qtTarget | Out-Null
            $code = Invoke-Native python -m aqt install-qt windows desktop $QtVersion $QtKit `
                --outputdir $qtTarget --modules qtimageformats
            if ($code -ne 0) {
                # Reported rather than swallowed: a partial Qt is the worst outcome,
                # because everything looks installed until a link fails on a module
                # that never arrived. The observed failure mode is a dropped
                # connection part-way through one module's archive.
                throw "aqt install-qt 失败（退出码 $code）。若为网络中断，重跑本脚本即可续传。"
            }
        }
        else {
            Write-Host "[fetch] Qt via vcpkg -> $QtRoot"
            Write-Host "        从源码构建，比 aqt 慢得多也大得多，但改 triplet 或预编译包不匹配时是正确选择。"
            if (-not (Get-Command vcpkg -ErrorAction SilentlyContinue)) {
                throw "找不到 vcpkg。请先安装，或用默认的 -QtSource aqt。"
            }
            New-Item -ItemType Directory -Force -Path $qtTarget | Out-Null
            $code = Invoke-Native vcpkg install qtbase:x64-windows --x-install-root="$qtTarget"
            if ($code -ne 0) { throw "vcpkg install 失败（退出码 $code）" }
        }

        if (-not (Test-Path $qtProbe)) {
            throw "Qt 安装后仍找不到 $qtProbe 。发行包不完整。"
        }
        Write-Host "[ok]    Qt 就位：$qtTarget"

        # A partial download is the failure this check exists for: everything looks
        # installed until a link fails on a module that never arrived. Measured on
        # this project -- qtdeclarative died at 18 MB of 148 MB and left a directory
        # tree that looked complete.
        $core = Join-Path $qtTarget "$QtVersion/msvc2022_64/lib/cmake/Qt6Core/Qt6CoreConfig.cmake"
        $widgets = Join-Path $qtTarget "$QtVersion/msvc2022_64/lib/cmake/Qt6Widgets/Qt6WidgetsConfig.cmake"
        if (-not (Test-Path $core) -or -not (Test-Path $widgets)) {
            throw "Qt 缺少 Qt6Core 或 Qt6Widgets。下载很可能中断了：删除 $qtTarget 后重跑。"
        }
        Write-Host "[ok]    Qt6Core 与 Qt6Widgets 都在"
    }

    Write-Host ""
    Write-Host "依赖就绪。接下来："
    Write-Host "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug"
    Write-Host "  cmake --build build"
    Write-Host "  ctest --test-dir build --output-on-failure"
    if ($WithQt) {
        Write-Host ""
        Write-Host "GUI（需要 MSVC x64；Qt 6 没有 32 位包）："
        Write-Host "  cmake -S . -B build-views -G Ninja -DCMAKE_BUILD_TYPE=Debug ``"
        Write-Host "        -DCMAKE_CXX_COMPILER=cl -DQP_BUILD_VIEWS=ON"
        Write-Host "  cmake --build build-views"
        Write-Host ""
        Write-Host "打可分发的包（含 Qt DLL、C++ 运行库与许可文件）："
        Write-Host "  pwsh scripts/package.ps1 -BuildDir build-views"
    }
}
finally {
    Pop-Location
}
