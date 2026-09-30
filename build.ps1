# SPDX-License-Identifier: MIT
# Build script for AllocSight (MinGW-w64 / GCC C++17)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcFile = Join-Path $ScriptDir "src\allocsight.cpp"
$OutExe  = Join-Path $ScriptDir "allocsight.exe"

$Gpp = "g++"
if (Test-Path "C:\mingw64\bin\g++.exe") {
    $Gpp = "C:\mingw64\bin\g++.exe"
}

Write-Host "[AllocSight] Compiling $SrcFile -> $OutExe ..." -ForegroundColor Cyan
& $Gpp -O3 -s -std=c++17 -municode -static $SrcFile -o $OutExe
Write-Host "[AllocSight] Build succeeded! Binary size: $((Get-Item $OutExe).Length / 1KB) KB" -ForegroundColor Green
