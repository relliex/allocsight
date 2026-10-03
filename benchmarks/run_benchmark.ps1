<#
.SYNOPSIS
    Reproducible 11-Tool Head-to-Head Disk Scanning Benchmark for AllocSight.
.DESCRIPTION
    Generates a deterministic 100,000-file (1,100-directory, 512 B/file) NTFS test corpus
    (48.8 MB logical size / 390.6 MB physical 4 KB cluster allocation) and measures wall-clock
    execution latency across AllocSight and 10 common CLI/scripting disk scanners used by AI agents.
#>
param(
    [string]$CorpusDir = "$env:TEMP\allocsight_bench_corpus",
    [int]$Runs = 5,
    [switch]$KeepCorpus
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$AllocExe = Join-Path $RepoRoot "allocsight.exe"

if (-not (Test-Path $AllocExe)) {
    Write-Error "allocsight.exe not found at $AllocExe. Run .\build.ps1 first."
}

Write-Host "[1/3] Preparing deterministic 100,000-file NTFS corpus at $CorpusDir ..." -ForegroundColor Cyan
$needCreate = $true
if (Test-Path $CorpusDir) {
    $existing = [System.IO.Directory]::GetFiles($CorpusDir, "*", [System.IO.SearchOption]::AllDirectories).Length
    if ($existing -eq 100000) { $needCreate = $false }
}

if ($needCreate) {
    if (Test-Path $CorpusDir) { Remove-Item -LiteralPath $CorpusDir -Recurse -Force }
    [System.IO.Directory]::CreateDirectory($CorpusDir) | Out-Null
    $payload = New-Object byte[] 512
    for ($i = 0; $i -lt 100; $i++) {
        $d1 = Join-Path $CorpusDir ("pkg_{0:D3}" -f $i)
        [System.IO.Directory]::CreateDirectory($d1) | Out-Null
        for ($j = 0; $j -lt 10; $j++) {
            $d2 = Join-Path $d1 ("mod_{0:D2}" -f $j)
            [System.IO.Directory]::CreateDirectory($d2) | Out-Null
            for ($k = 0; $k -lt 100; $k++) {
                $f = Join-Path $d2 ("item_{0:D3}.bin" -f $k)
                [System.IO.File]::WriteAllBytes($f, $payload)
            }
        }
    }
}

$pyScandir = Join-Path $env:TEMP "bench_scandir.py"
Set-Content -Path $pyScandir -Encoding UTF8 -Value @"
import os, sys
def scan(p):
    total = 0
    stack = [p]
    while stack:
        curr = stack.pop()
        try:
            with os.scandir(curr) as it:
                for entry in it:
                    if entry.is_dir(follow_symlinks=False):
                        stack.append(entry.path)
                    else:
                        total += entry.stat(follow_symlinks=False).st_size
        except PermissionError:
            pass
    return total
print(scan(sys.argv[1]))
"@

$pyWalk = Join-Path $env:TEMP "bench_walk.py"
Set-Content -Path $pyWalk -Encoding UTF8 -Value @"
import os, sys
total = 0
for root, dirs, files in os.walk(sys.argv[1]):
    for f in files:
        try:
            total += os.path.getsize(os.path.join(root, f))
        except OSError:
            pass
print(total)
"@

$nodeStat = Join-Path $env:TEMP "bench_node_sync.js"
Set-Content -Path $nodeStat -Encoding UTF8 -Value @"
const fs = require('fs');
const path = require('path');
function scan(dir) {
    let total = 0;
    const stack = [dir];
    while (stack.length > 0) {
        const curr = stack.pop();
        const entries = fs.readdirSync(curr, { withFileTypes: true });
        for (const e of entries) {
            const full = path.join(curr, e.name);
            if (e.isDirectory()) stack.push(full);
            else total += fs.statSync(full).size;
        }
    }
    return total;
}
console.log(scan(process.argv[2]));
"@

function Measure-Scanner([string]$Name, [int]$Iterations, [scriptblock]$Action) {
    Write-Host "  -> Benchmarking $Name ..."
    & $Action | Out-Null # Warmup
    $times = @()
    for ($r = 0; $r -lt $Iterations; $r++) {
        $sw = [System.Diagnostics.Stopwatch]::StartNew()
        & $Action | Out-Null
        $sw.Stop()
        $times += $sw.Elapsed.TotalMilliseconds
    }
    $sorted = $times | Sort-Object
    $med = [Math]::Round($sorted[[int][Math]::Floor($Iterations / 2)], 1)
    $min = [Math]::Round($sorted[0], 1)
    $mean = [Math]::Round(($times | Measure-Object -Average).Average, 1)
    $fps = [Math]::Round(100000.0 / ($med / 1000.0), 0)
    [PSCustomObject]@{
        Tool        = $Name
        Min_ms      = $min
        Median_ms   = $med
        Mean_ms     = $mean
        FilesPerSec = $fps
    }
}

Write-Host "[2/3] Running Warm-Cache Benchmarks ($Runs runs per tool) ..." -ForegroundColor Cyan
$results = @()
$results += Measure-Scanner "AllocSight v1.1.0 (tree -d 1 -j -q)" $Runs { & $AllocExe tree $CorpusDir -d 1 -j -q }
$results += Measure-Scanner "Robocopy (/L /MT:16 /S /BYTES)" $Runs { robocopy.exe $CorpusDir NULL /L /S /NJH /BYTES /MT:16 /NFL /NDL /NP }
$results += Measure-Scanner "PowerShell 7 (.NET EnumerateFiles)" $Runs {
    $sum = 0L
    foreach ($f in [System.IO.DirectoryInfo]::new($CorpusDir).EnumerateFiles('*', [System.IO.SearchOption]::AllDirectories)) {
        $sum += $f.Length
    }
}
if (Get-Command dua.exe -ErrorAction SilentlyContinue) {
    $results += Measure-Scanner "dua v2.45.0 (Rust jwalk parallel)" $Runs { dua.exe $CorpusDir }
}
$results += Measure-Scanner "CMD (dir /s /a /-c)" $Runs { cmd.exe /c "dir `"$CorpusDir`" /s /a /-c >nul" }
if (Get-Command python -ErrorAction SilentlyContinue) {
    $results += Measure-Scanner "Python 3.13 (os.scandir + DirEntry.stat)" $Runs { python $pyScandir $CorpusDir }
}
$results += Measure-Scanner "PowerShell 7 (Get-ChildItem -Recurse)" $Runs { Get-ChildItem -LiteralPath $CorpusDir -Recurse -File -Force | Measure-Object -Property Length -Sum }
if (Get-Command dust.exe -ErrorAction SilentlyContinue) {
    $results += Measure-Scanner "dust v1.2.6 (Rust rayon parallel)" $Runs { dust.exe -d 1 -n 20 $CorpusDir }
}
if (Get-Command node -ErrorAction SilentlyContinue) {
    $results += Measure-Scanner "Node.js v22 (fs.readdirSync + statSync)" ([Math]::Min($Runs, 3)) { node $nodeStat $CorpusDir }
}
if (Get-Command python -ErrorAction SilentlyContinue) {
    $results += Measure-Scanner "Python 3.13 (os.walk + os.path.getsize)" ([Math]::Min($Runs, 3)) { python $pyWalk $CorpusDir }
}
if (Get-Command du64.exe -ErrorAction SilentlyContinue) {
    $results += Measure-Scanner "Sysinternals du64 v1.62 (-l 1 -q)" 1 { du64.exe -accepteula -nobanner -l 1 -q $CorpusDir }
}

Write-Host "`n[3/3] Benchmark Summary:" -ForegroundColor Green
$results | Format-Table -AutoSize

Remove-Item -LiteralPath $pyScandir, $pyWalk, $nodeStat -Force -ErrorAction SilentlyContinue
if (-not $KeepCorpus) {
    Remove-Item -LiteralPath $CorpusDir -Recurse -Force -ErrorAction SilentlyContinue
}
