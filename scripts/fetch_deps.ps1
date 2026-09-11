# Fetches third-party dependencies into external/ (not tracked; see .gitignore)
#
# Why not use the default CMake FetchContent behavior:
#   FetchContent goes online during the **configure phase**. Offline development, CI
#   caches, and "configure succeeds only if the network does" are unacceptable; so
#   fetching is **explicit and one-shot**, and configure only probes locally.
#
# Usage:
#   pwsh scripts/fetch_deps.ps1            # fetch the missing dependencies
#   pwsh scripts/fetch_deps.ps1 -Force     # force a re-fetch
#
# Versions are **pinned** once chosen: a Catch2 upgrade edits this file and says why in the PR.
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

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
        git clone --quiet --depth 1 --branch $dep.Tag $dep.Url $target
        if ($LASTEXITCODE -ne 0) {
            throw "拉取 $($dep.Name) 失败（退出码 $LASTEXITCODE）"
        }

        if (-not (Test-Path $probe)) {
            throw "$($dep.Name) 拉取后缺少 $($dep.Check)，副本不完整"
        }

        $tag = (git -C $target describe --tags 2>$null)
        Write-Host "[ok]    $($dep.Name) -> $tag  ($($dep.Purpose))"
    }

    Write-Host ""
    Write-Host "依赖就绪。接下来："
    Write-Host "  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug"
    Write-Host "  cmake --build build"
    Write-Host "  ctest --test-dir build --output-on-failure"
}
finally {
    Pop-Location
}
