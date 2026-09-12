<#
.SYNOPSIS
Packages a distributable Windows directory: the shell, Qt's DLLs, and the licence
files LGPL-3.0 requires.

.DESCRIPTION
Why this is a script rather than a paragraph of instructions: **Apache-2.0 permits
redistribution, LGPL-3.0 conditions it.** Shipping the executable without Qt's
DLLs gives the user a program that cannot start; shipping it without the licence
texts puts the distributor in breach of a licence they accepted by linking against
Qt. Both are one command away from being right and neither is visible in a build
log, so the step is scripted and its result is checked.

What the package must contain, and where each requirement comes from:

  qp_shell.exe          the application
  Qt6*.dll              dynamic linking, so a recipient can replace Qt's libraries
                        (LGPL-3.0 section 4(a)). Static linking would require
                        shipping object code instead, which is why the build
                        deliberately links Qt dynamically.
  LICENSE               the project's own licence (Apache-2.0, section 4(a))
  NOTICE                the attributions that must travel with a binary
                        (Apache-2.0 section 4(d))
  LICENSE.LGPLv3        the full text of Qt's licence (LGPL-3.0 section 4(a))
  THIRD_PARTY_NOTICES.md  what else is in here and on what terms

The LGPLv3 text is **not** kept in the repository. There is no Qt code in the
repository, so committing the text would suggest otherwise; it belongs to the
distribution. The script finds a copy and fails loudly if it cannot, rather than
producing a package that looks complete and is not.

.PARAMETER BuildDir
The Qt-enabled build tree to package. Must have been configured with
-DQP_BUILD_VIEWS=ON.

.PARAMETER OutDir
Where to assemble the package. Created if absent, emptied if present.

.PARAMETER QtBin
Qt's bin directory. Defaults to the one recorded in the build tree's CMake cache.

.PARAMETER SkipVerify
Skip the launch check. Not recommended: launching the packaged copy with Qt's own
directory removed from PATH is the only thing that proves the DLLs were actually
copied, and a package that only works on the machine that built it is the exact
failure this check exists to catch.

.EXAMPLE
pwsh scripts/package.ps1 -BuildDir build-views -OutDir dist/qp-0.1.0-win64
#>
[CmdletBinding()]
param(
    [string]$BuildDir = "build-views",
    [string]$OutDir = "dist/qp-0.1.0-win64",
    [string]$QtBin = "",
    [switch]$SkipVerify
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Push-Location $repoRoot

function Fail($message) {
    Write-Host "[fail] $message" -ForegroundColor Red
    Pop-Location
    exit 1
}

function Step($message) { Write-Host "`n=== $message ===" -ForegroundColor Cyan }
function Ok($message)   { Write-Host "  [ok]   $message" -ForegroundColor Green }

try {
    # -- 1. Locate what we are packaging ------------------------------------
    Step "检查构建树"

    if (-not (Test-Path $BuildDir)) { Fail "构建目录不存在：$BuildDir" }

    $exe = Get-ChildItem -Path $BuildDir -Recurse -Filter "qp_shell.exe" -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if (-not $exe) {
        Fail "在 $BuildDir 下找不到 qp_shell.exe。视图层需要 -DQP_BUILD_VIEWS=ON 才会构建。"
    }
    Ok "可执行文件：$($exe.FullName)"

    # -- 2. Find windeployqt and Qt's bin -----------------------------------
    Step "定位 Qt"

    if (-not $QtBin) {
        # Read it out of the build tree rather than guessing: the kit that was
        # configured is the kit the binary was linked against, and deploying a
        # different one produces a package that fails at load time.
        $cache = Join-Path $BuildDir "CMakeCache.txt"
        if (Test-Path $cache) {
            $hit = Select-String -Path $cache -Pattern "Qt6Core_DIR:PATH=(.+)" -ErrorAction SilentlyContinue |
                   Select-Object -First 1
            if ($hit) {
                # .../lib/cmake/Qt6Core -> .../bin
                $libDir = Split-Path -Parent (Split-Path -Parent $hit.Matches[0].Groups[1].Value)
                $QtBin = Join-Path (Split-Path -Parent $libDir) "bin"
            }
        }
    }
    if (-not $QtBin -or -not (Test-Path $QtBin)) {
        Fail "找不到 Qt 的 bin 目录。用 -QtBin 显式指定，或确认构建树是用 Qt 配置的。"
    }

    $deploy = Join-Path $QtBin "windeployqt.exe"
    if (-not (Test-Path $deploy)) {
        Fail "找不到 windeployqt.exe（$QtBin）。它随 Qt 的 qttools 一起安装。"
    }
    Ok "Qt bin：$QtBin"

    # -- 3. Assemble the package -------------------------------------------
    Step "组装 $OutDir"

    # A previous package's executable may still be running -- most likely because
    # the verification step below launched it and something kept it alive -- and a
    # running process holds its DLLs open, so the cleanup fails with "access to the
    # path is denied" on whichever file Windows happens to report first. Naming the
    # cause here saves reading that message as a permissions problem.
    Get-Process -Name "qp_shell" -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "  [note] 结束仍在运行的旧包进程 (pid $($_.Id))"
        $_.Kill()
        $_.WaitForExit(5000)
    }
    if (Test-Path $OutDir) {
        try {
            Remove-Item -Recurse -Force $OutDir -ErrorAction Stop
        } catch {
            Fail "无法清空 $OutDir ：$($_.Exception.Message)。若有程序仍在使用该目录，请先关闭。"
        }
    }
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

    Copy-Item $exe.FullName (Join-Path $OutDir "qp_shell.exe")
    Ok "已复制 qp_shell.exe"

    # windeployqt copies the Qt DLLs AND the platform plugins. The plugins matter:
    # without platforms/qwindows.dll the executable starts and immediately aborts
    # with "could not find or load the Qt platform plugin windows", which reads as
    # a broken build rather than a missing file.
    #
    # --no-translations keeps the package small; a lab machine does not need Qt's
    # own translated strings, and the project ships no translations of its own yet.
    $deployArgs = @(
        (Join-Path $OutDir "qp_shell.exe"),
        "--release",
        "--no-translations",
        "--no-system-d3d-compiler",
        "--no-opengl-sw",
        # The package must contain exactly what NOTICE declares, and the first run
        # of this script disagreed with it: windeployqt works from imports, so a
        # Widgets application picked up Qt6Network and the tls / networkinformation
        # plugins even though nothing here opens a socket. A licence file that
        # describes a different artifact from the one a recipient received is the
        # failure this list prevents.
        #
        # Reconciled in this direction rather than by widening NOTICE: shipping a
        # networking stack in a classroom physics tool with no network feature would
        # be both larger and a bigger licence surface for no benefit.
        "--skip-plugin-types", "networkinformation,tls",
        "--no-network",
        # The C++ runtime travels **inside** the package rather than being assumed.
        # `dumpbin /dependents` shows MSVCP140.dll, VCRUNTIME140.dll and
        # VCRUNTIME140_1.dll: without them the program fails to start at all on a
        # machine that has never had a Visual C++ Redistributable installed, and the
        # error names a missing DLL rather than the redistributable that supplies it.
        #
        # Most Windows machines happen to have it, because other software installs
        # it -- which is exactly what makes relying on it a bad bet for a package
        # handed to a colleague. App-local deployment is Microsoft's supported
        # pattern and makes the directory self-contained.
        "--compiler-runtime",
        "--dir", (Resolve-Path $OutDir).Path
    )

    # $ErrorActionPreference = "Stop" plus a native command that writes to stderr is
    # a trap: PowerShell 5.1 promotes that stderr into a terminating error, so a
    # command that SUCCEEDS with a warning aborts the script. windeployqt warns
    # about missing DirectX shader compiler DLLs on a machine without the Windows
    # SDK, and that warning killed this run after it had copied exactly one DLL --
    # leaving behind a directory that looked like a package and was not one.
    #
    # The preference is therefore lowered around the call and the exit code is what
    # decides. Output is captured and printed only on failure, which is when it is
    # worth reading.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $deployLog = & $deploy @deployArgs 2>&1
    $deployCode = $LASTEXITCODE
    $ErrorActionPreference = $previous

    if ($deployCode -ne 0) {
        $deployLog | Select-Object -Last 15 | ForEach-Object { Write-Host "    $_" }
        Fail "windeployqt failed (exit code $deployCode)"
    }

    # -- C++ runtime, app-local ------------------------------------------------
    #
    # Copied from the MSVC redistributable directory rather than obtained by running
    # an installer on the target machine. `--compiler-runtime` further up deploys
    # `vc_redist.x64.exe`, which the recipient must run with administrator rights and
    # often reboot for; app-local deployment is Microsoft's supported alternative and
    # needs nothing at all.
    #
    # It also drags in `dxcompiler.dll` and `dxil.dll` (15 MB) for DirectX shader
    # compilation, which a Qt **Widgets** application never does -- those belong to
    # Qt Quick. Both are removed below, along with the installer.
    Remove-Item (Join-Path $OutDir "vc_redist.x64.exe") -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $OutDir "dxcompiler.dll") -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $OutDir "dxil.dll") -ErrorAction SilentlyContinue

    if (-not $env:VCToolsRedistDir) {
        Fail @"
找不到 VCToolsRedistDir（VC++ 可再发行文件目录）。
请在 "x64 Native Tools Command Prompt" 里运行本脚本；否则请自行把
MSVCP140.dll / VCRUNTIME140.dll / VCRUNTIME140_1.dll 放入包中。
"@
    }

    # The CRT directory name tracks the toolset version (Microsoft.VC143.CRT today),
    # so the first match is taken rather than one name being hard-coded: a toolset
    # upgrade must not break packaging.
    $redistRoot = Get-ChildItem $env:VCToolsRedistDir -Recurse -Directory `
        -Filter "Microsoft.VC*.CRT" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*\x64\*" } | Select-Object -First 1 -ExpandProperty FullName
    if (-not $redistRoot) {
        Fail "在 $env:VCToolsRedistDir 下找不到 x64 的 Microsoft.VC*.CRT 目录"
    }

    foreach ($runtime in @("MSVCP140.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll")) {
        $source = Join-Path $redistRoot $runtime
        if (-not (Test-Path $source)) {
            Fail "缺少 $runtime（在 $redistRoot 下）：程序在没有装过 VC++ 可再发行包的机器上无法启动"
        }
        Copy-Item $source $OutDir
    }
    Ok "C++ 运行库就位（app-local：目标机器无需安装、无需管理员权限、无需重启）"
    $dllCount = (Get-ChildItem $OutDir -Filter "Qt6*.dll" | Measure-Object).Count
    if ($dllCount -eq 0) { Fail "windeployqt 没有复制任何 Qt DLL，包不完整" }
    Ok "已部署 $dllCount 个 Qt DLL"

    $platformPlugin = Join-Path $OutDir "platforms/qwindows.dll"
    if (-not (Test-Path $platformPlugin)) {
        Fail "缺少 platforms/qwindows.dll：程序会启动后立刻因找不到平台插件而中止"
    }
    Ok "平台插件就位"

    # -- 4. Licence files --------------------------------------------------
    Step "许可文件"

    # The run instructions are part of the package, not a message in a chat window:
    # whoever receives the folder will not have this conversation. `docs/` is not
    # copied wholesale -- most of it is design material for developers.
    $runbook = Join-Path $repoRoot "packaging/运行说明.md"
    if (Test-Path $runbook) {
        Copy-Item $runbook $OutDir
        Ok "运行说明已复制"
    } else {
        Write-Host "  [warn] 缺少 packaging/运行说明.md，包内将没有运行说明" -ForegroundColor Yellow
    }

    foreach ($name in @("LICENSE", "NOTICE", "THIRD_PARTY_NOTICES.md")) {
        if (-not (Test-Path $name)) { Fail "缺少 $name，无法随二进制分发" }
        Copy-Item $name $OutDir
    }
    Ok "LICENSE / NOTICE / THIRD_PARTY_NOTICES.md 已复制"

    # LGPLv3's full text must accompany the binary. It is deliberately not in the
    # repository -- there is no Qt code here -- so it is located from an installed
    # copy. A package assembled without it is in breach, and the check is cheap.
    $lgplSources = @(
        (Join-Path $QtBin "..\LICENSES\LGPL-3.0-only.txt"),
        (Join-Path $QtBin "..\LICENSES\LGPLv3.txt"),
        "C:\Program Files\CMake\share\cmake-4.2\Licenses\LGPLv3.txt"
    )
    $lgpl = $lgplSources | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $lgpl) {
        Fail @"
找不到 LGPL-3.0 全文。分发物必须附带它（LGPL-3.0 第 4(a) 条）。
已查找：
$($lgplSources -join "`n")
请从 https://www.gnu.org/licenses/lgpl-3.0.txt 取得后放到上述任一路径，或先手工放入包中。
"@
    }
    Copy-Item $lgpl (Join-Path $OutDir "LICENSE.LGPLv3")
    Ok "LGPL-3.0 全文已复制（来自 $lgpl）"

    # -- 5. Verify the package actually runs --------------------------------
    #
    # This is the step that makes the word "distributable" mean something. Running
    # with Qt's own bin removed from PATH proves the DLLs came along; a package
    # that only works on the build machine would otherwise pass every check above.
    if ($SkipVerify) {
        Write-Host "`n[skip] 已跳过启动验证（-SkipVerify）：此包**未经验证**可运行" -ForegroundColor Yellow
    } else {
        Step "验证打包后的程序能独立启动"

        # Qt's own bin directory is removed from PATH so the packaged copy cannot
        # borrow Qt from the machine that built it. This is the check that makes
        # the word "distributable" mean something: a package that only works on the
        # build machine passes every other step in this script.
        $cleanPath = ($env:PATH -split ';' | Where-Object { $_ -and ($_ -notlike "*$QtBin*") }) -join ';'
        $packagedExe = Join-Path (Resolve-Path $OutDir).Path "qp_shell.exe"

        # `cmd /c` rather than `Start-Process -Environment`: the latter is PowerShell
        # 7 only, and on the 5.1 that ships with Windows it fails at parameter
        # binding -- before the check runs, so the script dies instead of verifying.
        $proc = Start-Process -FilePath "cmd.exe" `
            -ArgumentList "/c", "set `"PATH=$cleanPath`" && `"$packagedExe`"" `
            -PassThru -WindowStyle Hidden

        Start-Sleep -Seconds 6
        if ($proc.HasExited) {
            Fail "打包后的程序在 6 秒内退出（退出码 $($proc.ExitCode)）。Qt 的 DLL 很可能没全部复制。"
        }
        $proc.Kill()
        $proc.WaitForExit(5000) | Out-Null
        Ok "程序在去掉 Qt 目录的 PATH 下启动并保持运行"
    }

    # -- 6. Summary ---------------------------------------------------------
    Step "完成"
    $size = [int](((Get-ChildItem $OutDir -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB)
    Write-Host "  目录：$OutDir"
    Write-Host "  大小：约 $size MB"
    Write-Host "  内容："
    Get-ChildItem $OutDir | Sort-Object Name | ForEach-Object {
        $kind = if ($_.PSIsContainer) { "dir " } else { "file" }
        Write-Host "    [$kind] $($_.Name)"
    }
    Write-Host ""
    Write-Host "分发前的剩余检查（人工）："
    Write-Host "  - 在一台**没装 Qt** 的机器上解压并运行（本脚本验证的是 PATH 隔离，不是全新机器）"
    Write-Host "  - 确认 THIRD_PARTY_NOTICES.md 里没有列出实际并未使用的组件"
}
finally {
    Pop-Location
}
