# qp 的构建入口：一次跑完两个编译器的完整门禁
#
# 为什么需要这个脚本（而不是直接敲 cmake）：
#   1. **编译器矩阵是硬门禁**（enforcement.md §2.1）。只在一个编译器上通过
#      不算完成——P1 阶段 MSVC 就抓到了 GCC 完全接受的非类型模板参数缺陷。
#   2. MSVC 在中文 Windows 上会往 stderr 打大量本地化的 `/showIncludes` 提示
#      （"注意: 包含文件: ..."）。直接看屏幕会淹掉真正的错误，必须先落日志。
#   3. Ninja + MSVC 需要 vcvars 环境；手敲容易漏，漏了会静默换成别的工具链。
#
# 用法：
#   pwsh scripts/build.ps1                 # 两个编译器都跑
#   pwsh scripts/build.ps1 -Only gcc       # 只跑 GCC
#   pwsh scripts/build.ps1 -Only msvc
#   pwsh scripts/build.ps1 -NoTest         # 只构建不测试
[CmdletBinding()]
param(
    [ValidateSet("all", "gcc", "msvc")]
    [string]$Only = "all",
    [switch]$NoTest,
    [switch]$Fresh,
    [string]$BuildType = "Debug"
)

$ErrorActionPreference = "Continue"
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $repoRoot

$script:Failures = @()

function Write-Step($text) { Write-Host "`n=== $text ===" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "  [OK]   $text" -ForegroundColor Green }
function Write-Bad($text)  { Write-Host "  [FAIL] $text" -ForegroundColor Red; $script:Failures += $text }

function Invoke-Toolchain {
    param(
        [string]$Name,
        [string]$BuildDir,
        [string]$Generator,
        [string]$Compiler,
        [scriptblock]$EnvSetup
    )

    Write-Step "$Name  ($BuildDir)"

    if ($Fresh -and (Test-Path $BuildDir)) {
        Remove-Item -Recurse -Force $BuildDir
    }

    # 环境准备（MSVC 需要 vcvars）与构建都落日志：MSVC 的本地化提示会淹掉错误
    $cfgLog  = Join-Path $repoRoot "$BuildDir.configure.log"
    $bldLog  = Join-Path $repoRoot "$BuildDir.build.log"
    $tstLog  = Join-Path $repoRoot "$BuildDir.test.log"

    & $EnvSetup

    $cfgArgs = @("-S", ".", "-B", $BuildDir, "-G", $Generator, "-DCMAKE_BUILD_TYPE=$BuildType")
    if ($Compiler) { $cfgArgs += "-DCMAKE_CXX_COMPILER=$Compiler" }

    & cmake @cfgArgs *> $cfgLog
    if ($LASTEXITCODE -ne 0) {
        Write-Bad "$Name 配置失败，见 $cfgLog"
        Select-String -Path $cfgLog -Pattern "error|Error|CMake Error" | Select-Object -First 8 |
            ForEach-Object { Write-Host "    $($_.Line)" }
        return
    }

    & cmake --build $BuildDir *> $bldLog
    if ($LASTEXITCODE -ne 0) {
        Write-Bad "$Name 构建失败，见 $bldLog"
        Select-String -Path $bldLog -Pattern "error [A-Z]?\d|error:|FAILED" | Select-Object -First 12 |
            ForEach-Object { Write-Host "    $($_.Line)" }
        return
    }
    Write-Ok "$Name 构建通过"

    if ($NoTest) { return }

    & ctest --test-dir $BuildDir *> $tstLog
    $summary = Select-String -Path $tstLog -Pattern "tests passed" | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) {
        Write-Bad "$Name 测试失败，见 $tstLog"
        Select-String -Path $tstLog -Pattern "Failed|FAILED" | Select-Object -First 12 |
            ForEach-Object { Write-Host "    $($_.Line)" }
    } else {
        Write-Ok "$Name 测试通过 — $($summary.Line.Trim())"
    }
}

# ── GCC（MinGW-w64）──────────────────────────────────────────────────────────
if ($Only -in @("all", "gcc")) {
    Invoke-Toolchain -Name "GCC (MinGW-w64)" -BuildDir "build" -Generator "Ninja" `
        -Compiler "g++" -EnvSetup { }
}

# ── MSVC ─────────────────────────────────────────────────────────────────────
if ($Only -in @("all", "msvc")) {
    $vcvars = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $vcvars) {
        Write-Bad "找不到 vcvars64.bat：MSVC 未安装？"
    } else {
        Invoke-Toolchain -Name "MSVC (cl)" -BuildDir "build-msvc" -Generator "Ninja" `
            -Compiler "cl" -EnvSetup {
                # vcvars 只能在 cmd 里生效，因此用 cmd /c 包一层后再继续
                cmd /c "`"$($vcvars.FullName)`" >nul 2>&1 && set" | ForEach-Object {
                    if ($_ -match '^([^=]+)=(.*)$') {
                        Set-Item -Path "env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
                    }
                }
            }
    }
}

Pop-Location

Write-Host ""
if ($script:Failures.Count -gt 0) {
    Write-Host "构建失败：" -ForegroundColor Red
    $script:Failures | ForEach-Object { Write-Host "  - $_" }
    exit 1
}
Write-Host "全部通过。" -ForegroundColor Green
exit 0
