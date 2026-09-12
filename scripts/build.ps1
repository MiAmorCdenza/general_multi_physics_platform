# Build entry point for qp: run the complete gate for both compilers in one go
#
# Why this script is needed (rather than typing cmake directly):
#   1. **The compiler matrix is a hard gate** (enforcement.md section 2.1). Passing on only one compiler
#      does not count -- in phase P1 MSVC caught a non-type template parameter defect GCC accepted fully.
#   2. On a Chinese Windows, MSVC writes many localized `/showIncludes` notices to stderr
#      ("Note: including file: ..."). Reading the screen buries the real errors; log to a file first.
#   3. Ninja + MSVC needs a vcvars environment; hand-typing it is easy to forget, and a forgotten one silently switches toolchain.
#
# Usage:
#   pwsh scripts/build.ps1                 # both compilers
#   pwsh scripts/build.ps1 -Only gcc       # GCC only
#   pwsh scripts/build.ps1 -Only msvc
#   pwsh scripts/build.ps1 -NoTest         # build only, no tests
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
        [scriptblock]$EnvSetup,
        [string[]]$ExtraArgs = @()
    )

    Write-Step "$Name  ($BuildDir)"

    if ($Fresh -and (Test-Path $BuildDir)) {
        Remove-Item -Recurse -Force $BuildDir
    }

    # Both environment setup (MSVC needs vcvars) and the build go to a log: MSVC's localized notices bury errors
    $cfgLog  = Join-Path $repoRoot "$BuildDir.configure.log"
    $bldLog  = Join-Path $repoRoot "$BuildDir.build.log"
    $tstLog  = Join-Path $repoRoot "$BuildDir.test.log"

    & $EnvSetup

    # -- Pin the console code page to UTF-8 before configuring ----------------
    #
    # This is not cosmetic. Ninja learns a translation unit's header dependencies
    # by matching cl.exe's `/showIncludes` output against `msvc_deps_prefix` in
    # rules.ninja. CMake detects that prefix during compiler detection with
    # `execute_process(ENCODING AUTO)`, which decodes the child's output using
    # the console code page. cl.exe prints the (localized) prefix as UTF-8.
    #
    # If the console code page is a legacy one (437, 936, ...), CMake records
    # mojibake instead of the prefix: the UTF-8 bytes E6 B3 A8 that begin the
    # localized Chinese prefix are decoded one byte at a time and
    # re-emitted as C2 B5 E2 94 82 C2 BF. Ninja then compares the wrong bytes
    # and therefore sees no dependency at all.
    #
    # The failure is silent: every build succeeds, Ninja simply never learns a
    # dependency, editing a header reports "no work to do", and the test suite
    # runs against stale binaries. This project lost several debugging rounds to
    # exactly that.
    #
    # Setting the code page here fixes it at the source: CMake now decodes what
    # cl.exe actually wrote. scripts/check_build_deps.ps1 is the regression test
    # and must stay green for the MSVC build directory.
    & chcp 65001 | Out-Null

    $cfgArgs = @("-S", ".", "-B", $BuildDir, "-G", $Generator, "-DCMAKE_BUILD_TYPE=$BuildType")
    if ($Compiler) { $cfgArgs += "-DCMAKE_CXX_COMPILER=$Compiler" }
    if ($ExtraArgs.Count -gt 0) { $cfgArgs += $ExtraArgs }

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

# -- GCC (MinGW-w64) ----------------------------------------------------------
if ($Only -in @("all", "gcc")) {
    # Plugins too, for the same reason as MSVC: content that no matrix entry compiles
    # is content that rots, and both compilers have to accept it. Qt stays off -- this
    # is the build that proves core/ and plugins/ do not need it.
    Invoke-Toolchain -Name "GCC (MinGW-w64)" -BuildDir "build" -Generator "Ninja" `
        -Compiler "g++" -ExtraArgs @("-DQP_BUILD_PLUGINS=ON") -EnvSetup { }
}

# -- MSVC ---------------------------------------------------------------------
if ($Only -in @("all", "msvc")) {
    $vcvars = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $vcvars) {
        Write-Bad "找不到 vcvars64.bat：MSVC 未安装？"
    } else {
        # -- Build the Qt half too, whenever a Qt kit exists ---------------------
        #
        # QP_BUILD_VIEWS defaults to OFF, and this script never passed it. So
        # "both compilers green" meant core/ only: views/qt/ (the canvas, the
        # property panel, the editor window) was compiled by no gate, no matrix
        # entry and no test run -- it could stop compiling and every signal in this
        # repository would stay green. That is the worst kind of gap, because the
        # GUI is the part a user actually touches, and it was verified only by
        # someone remembering to configure by hand.
        #
        # views/model/ is deliberately *not* behind the flag and is already covered
        # by both compilers; this is about the Qt-dependent half.
        #
        # Detection mirrors the CMake side (QP_QT_ROOT, then C:/Qt). If no kit is
        # found, the build proceeds without views and says so, rather than failing:
        # core/ is contractually buildable with no Qt present, and a missing Qt
        # must not turn into a red build for someone working on core/.
        $qtFound = @()
        foreach ($root in @((Join-Path $repoRoot "external\Qt"), "C:\Qt")) {
            if (Test-Path $root) {
                $qtFound += Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -match '^\d+\.\d+' } |
                    ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
                    Where-Object { $_.Name -like "msvc*_64" }
            }
        }

        # Plugin content is in the matrix unconditionally. It is toolchain-neutral, and
        # leaving it out would recreate exactly the gap views/ had: a directory of real
        # code that no entry point in this repository ever compiled.
        $msvcArgs = @("-DQP_BUILD_PLUGINS=ON")
        if ($qtFound.Count -gt 0) {
            $msvcArgs += "-DQP_BUILD_VIEWS=ON"
            Write-Host "  Qt kit: $($qtFound[0].FullName) -- 同时构建并测试 Qt 视图层" -ForegroundColor DarkGray
        } else {
            Write-Host "  未找到 Qt 套件；跳过视图层（core/ 不依赖 Qt）" -ForegroundColor DarkGray
        }

        Invoke-Toolchain -Name "MSVC (cl)" -BuildDir "build-msvc" -Generator "Ninja" `
            -Compiler "cl" -ExtraArgs $msvcArgs -EnvSetup {
                # vcvars only takes effect inside cmd, so wrap it in cmd /c before continuing
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
