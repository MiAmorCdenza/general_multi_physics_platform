# Verify "a header changed, so the build system must recompile its consumers".
#
# Why this test is needed (a real failure measured in this project):
#
#   Ninja relies on `msvc_deps_prefix` matching the `/showIncludes` output prefix
#   to build header dependencies. That prefix is **localized** (English "Note: including
#   file:", or its Chinese translation). If the configure and build phases use different
#   locales, the prefix recorded in rules.ninja no longer matches the actual output, so:
#
#       **dependency tracking fails silently**: a header changes, ninja says "no work to do",
#       the link uses a stale binary, tests run the old code, and every conclusion is false.
#
#   It neither errors nor warns; it surfaces only as "changed a header, saw no change".
#   This project burned several debugging rounds on it.
#
# This script turns "is dependency tracking working?" into an **executable check**:
#   1. append one harmless comment line to a core header
#   2. run a dry-run, count the targets that need recompiling
#   3. restore the file
#   4. if the recompile count is 0 -> tracking is broken -> fail
#
# Usage:
#   pwsh scripts/check_build_deps.ps1              # check the default build directory
#   pwsh scripts/check_build_deps.ps1 -BuildDir build-msvc
#
# Exit code: 0 = dependency tracking works; 1 = broken, or no dry-run output.
[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [string]$ProbeHeader = "core/units/include/qp/units/dim.hpp"
)

$ErrorActionPreference = "Continue"
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $repoRoot
try {
    if (-not (Test-Path (Join-Path $BuildDir "build.ninja"))) {
        Write-Host "[skip] $BuildDir 不是 Ninja 构建目录"
        exit 0
    }
    if (-not (Test-Path $ProbeHeader)) {
        Write-Host "[fail] 探针头文件不存在：$ProbeHeader"
        exit 1
    }

    $probePath = (Resolve-Path $ProbeHeader).Path
    $original = [System.IO.File]::ReadAllText($probePath)
    $marker = "// qp build-dependency probe`n"

    try {
        # Append one comment line: harmless, but it changes the timestamp and the bytes
        [System.IO.File]::WriteAllText($probePath, $original + $marker,
                                       [System.Text.UTF8Encoding]::new($false))

        $dry = & ninja -C $BuildDir -n 2>&1 | Out-String
        $count = ([regex]::Matches($dry, "Building CXX")).Count

        if ($count -eq 0) {
            Write-Host "[fail] 改了 $ProbeHeader 之后，$BuildDir 无需重编译任何目标。"
            Write-Host "       这说明**依赖跟踪已失效**：构建会链接到过期的二进制，"
            Write-Host "       测试跑的是旧代码，结论不可信。"
            Write-Host "       常见原因：Ninja 的 msvc_deps_prefix 与实际 /showIncludes"
            Write-Host "       输出不一致（控制台代码页不是 UTF-8）。见 scripts/build.ps1"
            Write-Host "       里的 chcp 65001，以及 standards/enforcement.md §2.2。"
            Write-Host "       注意 VSLANG 不能替代它：语言由 MUI 语言包决定。"
            exit 1
        }

        Write-Host "[ok]   $BuildDir 依赖跟踪有效：改动 1 个头文件触发 $count 个目标重编译"
        exit 0
    }
    finally {
        [System.IO.File]::WriteAllText($probePath, $original,
                                       [System.Text.UTF8Encoding]::new($false))
    }
}
finally {
    Pop-Location
}
