# make-windows.ps1 -- build a portable ZIP and an installer .exe.
#
# ASCII only, deliberately. Windows PowerShell 5.1 reads a .ps1 without a byte
# order mark in the system ANSI codepage, not UTF-8. An em dash inside a
# double-quoted string then decodes to a curly closing quote, which PowerShell
# accepts as a string delimiter -- the string ends early and every line after
# it fails to parse. A BOM would also fix it, but BOMs get stripped by editors
# and diff tools and the failure is a wall of misleading syntax errors, so the
# file simply has no characters that can be misread.
#
#   .\scripts\make-windows.ps1
#   .\scripts\make-windows.ps1 -Build build-msvc
#
# Before the first run, on the Windows machine:
#
#   1. Visual Studio 2022 with "Desktop development with C++".
#   2. Qt 6 for MSVC, from the online installer. Note the path -- something
#      like C:\Qt\6.8.1\msvc2022_64.
#   3. SQLite and zlib. Windows has neither, so vcpkg:
#          git clone https://github.com/microsoft/vcpkg C:\vcpkg
#          C:\vcpkg\bootstrap-vcpkg.bat
#          C:\vcpkg\vcpkg install sqlite3:x64-windows zlib:x64-windows
#   4. Inno Setup 6, for the installer half. Skip it and you still get the ZIP.
#
#   cmake -B build-msvc -A x64 `
#     -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
#     -DCMAKE_PREFIX_PATH=C:\Qt\6.8.1\msvc2022_64
#   cmake --build build-msvc --config Release
#   ctest --test-dir build-msvc -C Release --output-on-failure
#
# **Run the tests.** The money arithmetic uses a different 128-bit
# implementation on MSVC than it does anywhere else -- see src/core/Wide.h --
# and while the two agree on 1,2 million values and on the whole suite when
# forced down the same path on gcc, this is the first time MSVC's own compiler
# is the one running it. If anything in this project is going to differ on
# Windows, that is where it will show, and the suite is what will show it.

param(
    [string]$Build = "build-msvc",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $Root $Build

# MSVC generators put the binary under a per-config directory; Ninja does not.
$Exe = Join-Path $BuildDir "$Config\Fakturo.exe"
if (-not (Test-Path $Exe)) { $Exe = Join-Path $BuildDir "Fakturo.exe" }
if (-not (Test-Path $Exe)) {
    Write-Error "No Fakturo.exe under $BuildDir. Build first:`n  cmake --build $Build --config $Config"
}

$Version = (Get-Item $Exe).VersionInfo.ProductVersion
if (-not $Version) { $Version = "0.0.0" }
Write-Host "Fakturo $Version"

# ---------------------------------------------------------------- staging
$Stage = Join-Path ([System.IO.Path]::GetTempPath()) ("fakturo-" + [guid]::NewGuid())
$App = Join-Path $Stage "Fakturo"
New-Item -ItemType Directory -Path $App -Force | Out-Null
Copy-Item $Exe $App

Write-Host "  bundling Qt (windeployqt)"
# Three ways to find it, in order of how much they can be trusted.
#
# The build directory knows exactly which Qt was compiled against, and asking
# it is the only method that cannot pick the wrong one -- PATH and qmake will
# happily hand back a different Qt, or a MinGW one, and the mismatch does not
# show until the packaged application fails to start on another machine. It
# also means the PATH does not have to be set at all, which is what actually
# went wrong: the PATH lives for one console session and packaging usually
# happens in a later one.
$WinDeployPath = $null
$Cache = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path $Cache) {
    # Qt6_DIR points at <prefix>/lib/cmake/Qt6, so the prefix is three up.
    $line = Select-String -Path $Cache -Pattern '^Qt6_DIR:PATH=(.+)$' |
            Select-Object -First 1
    if ($line) {
        $qtDir = $line.Matches[0].Groups[1].Value
        $prefix = Split-Path (Split-Path (Split-Path $qtDir))
        $candidate = Join-Path $prefix "bin\windeployqt.exe"
        if (Test-Path $candidate) { $WinDeployPath = $candidate }
    }
}
if (-not $WinDeployPath) {
    $found = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
    if (-not $found) {
        $qmake = Get-Command qmake.exe -ErrorAction SilentlyContinue
        if ($qmake) {
            $candidate = Join-Path (Split-Path $qmake.Source) "windeployqt.exe"
            if (Test-Path $candidate) { $WinDeployPath = $candidate }
        }
    } else {
        $WinDeployPath = $found.Source
    }
}
if (-not $WinDeployPath) {
    Write-Error ("windeployqt not found, and $Cache did not name a Qt either.`n" +
                 "Configure the build first, or put Qt's bin directory on PATH:`n" +
                 "  set PATH=C:\Qt6\6.5.3\msvc2019_64\bin;%PATH%")
}
Write-Host "    $WinDeployPath"

# Fakturo uses none of these. Left in, they add tens of megabytes and pull the
# whole QML runtime in behind them.
& $WinDeployPath --release --no-quick-import --no-translations `
    --no-system-d3d-compiler --no-opengl-sw `
    (Join-Path $App "Fakturo.exe")
if ($LASTEXITCODE -ne 0) { Write-Error "windeployqt failed" }

# ------------------------------------------------- is it actually portable?
# The equivalent of the dmg script's otool check. windeployqt copies what it
# can find and says little about what it could not, so a folder that runs here
# -- where the Qt and vcpkg directories are on PATH -- can fail on a machine
# where they are not. Anything the .exe still needs and the folder does not
# have is what would go missing.
Write-Host "  checking the folder is self-contained"
$missing = @()
foreach ($dll in @("Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll",
                   "Qt6Concurrent.dll", "platforms\qwindows.dll")) {
    if (-not (Test-Path (Join-Path $App $dll))) { $missing += $dll }
}
# vcpkg's dependencies are not Qt's, so windeployqt does not know about them.
foreach ($dll in @("sqlite3.dll", "zlib1.dll")) {
    if (-not (Test-Path (Join-Path $App $dll))) {
        $found = Get-ChildItem -Path (Join-Path $BuildDir "..") -Filter $dll -Recurse `
                    -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { Copy-Item $found.FullName $App }
        else { $missing += "$dll (from vcpkg)" }
    }
}
if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "  Missing from the folder:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "      $_" -ForegroundColor Yellow }
    Write-Host "  It may run on THIS machine, where these are on PATH, and fail" -ForegroundColor Yellow
    Write-Host "  on any other. Copy them in before shipping it." -ForegroundColor Yellow
    Write-Host ""
}

# ------------------------------------------------------------- portable ZIP
$Zip = Join-Path $BuildDir "Fakturo-$Version-windows-portable.zip"
if (Test-Path $Zip) { Remove-Item $Zip }
Write-Host "  packing the portable ZIP"
Compress-Archive -Path $App -DestinationPath $Zip

# --------------------------------------------------------------- installer
$Iss = Join-Path $Root "resources\fakturo.iss"
# Three places, because where Inno Setup lands depends on how it was
# installed. 6.7 through winget installs per-user into LOCALAPPDATA and never
# touches Program Files at all, so a script looking only there reports it
# missing and quietly drops the installer half of the build.
$ISCC = @(
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if ($ISCC) {
    Write-Host "  building the installer (Inno Setup)"
    & $ISCC "/DAppVersion=$Version" "/DStageDir=$App" "/DOutDir=$BuildDir" $Iss | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Error "Inno Setup failed" }
} else {
    Write-Host "  Inno Setup not found -- ZIP only." -ForegroundColor Yellow
    Write-Host "  Install it from https://jrsoftware.org/isdl.php to get the .exe"
}

Remove-Item -Recurse -Force $Stage
Write-Host ""
Write-Host "Done:"
Get-ChildItem $BuildDir -Filter "Fakturo-$Version-*" | ForEach-Object {
    "{0,10:N1} MB  {1}" -f ($_.Length / 1MB), $_.Name
}
Write-Host ""
Write-Host "Unsigned, so SmartScreen will warn on another machine: 'More info'"
Write-Host "then 'Run anyway'. Silencing that needs a code-signing certificate."
