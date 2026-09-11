# 拉取第三方依赖到 external/（不入库，见 .gitignore）
#
# 为什么不用 CMake FetchContent 的默认行为：
#   FetchContent 在**配置阶段**联网。离线开发、CI 缓存、以及"配置成功与否
#   取决于网络"都是不可接受的。因此依赖拉取是**显式的、一次性的**步骤，
#   配置阶段只做本地探测。
#
# 用法：
#   pwsh scripts/fetch_deps.ps1            # 拉取缺失的依赖
#   pwsh scripts/fetch_deps.ps1 -Force     # 强制重拉
#
# 版本一经选定即**钉死**：升级 Catch2 必须改本文件并在 PR 说明中写明理由。
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

# ── 依赖清单（唯一真相源）────────────────────────────────────────────────────
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
