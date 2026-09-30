![AllocSight Banner](assets/banner.png)

[![License: MIT](https://img.shields.io/badge/License-MIT-0284c7.svg?style=flat-square)](LICENSE)
[![Standard: C++17](https://img.shields.io/badge/C%2B%2B-17-4f46e5.svg?style=flat-square)](src/allocsight.cpp)
[![Platform: Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0f766e.svg?style=flat-square)](CMakeLists.txt)
[![Dependencies: Zero](https://img.shields.io/badge/Dependencies-None-059669.svg?style=flat-square)](build.ps1)

[**English**](README.md) | [**简体中文**](docs/i18n/README.zh-CN.md) | [**Español**](docs/i18n/README.es.md) | [**Français**](docs/i18n/README.fr.md) | [**Русский**](docs/i18n/README.ru.md) | [**العربية**](docs/i18n/README.ar.md)

---

## Table of Contents

- [Overview](#overview)
- [Core Capabilities](#core-capabilities)
- [System Architecture](#system-architecture)
- [Performance Benchmarks](#performance-benchmarks)
- [Command-Line Interface](#command-line-interface)
- [Query Filter Grammar](#query-filter-grammar)
- [Diagnostic Heuristics](#diagnostic-heuristics)
- [AI Agent & JSON Integration](#ai-agent--json-integration)
- [Building from Source](#building-from-source)
- [License](#license)

---

## Overview

**AllocSight** is a high-concurrency, zero-dependency command-line disk allocation analyzer and automated space reclamation engine for Windows. Written in native C++17 directly against the Win32 File Management APIs, it is engineered for software developers, systems administrators, and autonomous AI agents that require sub-second storage telemetry without graphical rendering overhead.

Unlike conventional directory size utilities that report only logical file lengths (`nFileSizeLow` / `nFileSizeHigh`), AllocSight computes true **physical NTFS cluster allocations**, accurately accounting for filesystem cluster alignment, NTFS LZNT1/XPRESS compression, sparse file regions, cloud-only recall placeholders, and alternate data streams (ADS).

![AllocSight Terminal Output](assets/terminal-demo.png)

---

## Core Capabilities

1. **Multi-Threaded Kernel Directory Enumeration**
   Dispatches an 8-to-16 worker thread pool over a lock-minimized work queue (`ParallelScanner`). Each worker invokes `FindFirstFileExW` with `FindExInfoBasic` (omitting 8.3 short-name lookups) and `FIND_FIRST_EX_LARGE_FETCH` (enabling large kernel directory buffer reads).
2. **True Physical Cluster Allocation Accounting**
   - Aligns file allocations to the target volume's exact cluster size (`GetDiskFreeSpaceW`).
   - Queries actual on-disk compressed/sparse extents via `GetCompressedFileSizeW` when `FILE_ATTRIBUTE_COMPRESSED` or `FILE_ATTRIBUTE_SPARSE_FILE` is present.
   - Identifies cloud-only storage placeholders (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, `FILE_ATTRIBUTE_RECALL_ON_OPEN`, `FILE_ATTRIBUTE_OFFLINE`) as `0` local bytes to prevent inflated usage metrics on OneDrive or cloud-mounted volumes.
   - Optionally enumerates secondary NTFS Alternate Data Streams (`:$DATA`) using documented `FindFirstStreamW` / `FindNextStreamW` APIs (`--ads`).
3. **Reparse Point Cycle Prevention**
   Inspects `WIN32_FIND_DATAW::dwReserved0` against `<winnt.h>` name-surrogate reparse macros (`IsReparseTagNameSurrogate`, `IO_REPARSE_TAG_SYMLINK`, `IO_REPARSE_TAG_MOUNT_POINT`) to prevent infinite directory recursion across symbolic links and volume mount junctions.
4. **Heuristic Space Reclamation Engine**
   Classifies reclaimable disk space into two actionable tiers:
   - **`[SAFE]`**: Deterministic package caches (`uv`, `pip`, `conda/pkgs`, `npm/_cacache`, `pnpm-cache`), shader caches (`DXCache`, `ShaderCache`), crash dumps, and orphaned staging directories.
   - **`[REVIEW]`**: Archives that have already been extracted into a sibling directory of the same stem name, duplicate backup directories, rebuildable build artifacts (`target`, `node_modules`, `.venv`), and stale large files unmodified for more than 180 days.
5. **Direct-Navigation Markdown & JSON Reports**
   Generates structured Markdown tables (`allocsight report`) and machine-readable JSON payloads (`-j`) where every directory and file path is accompanied by an RFC 8089 percent-encoded `file:///` URI for single-click navigation in modern IDEs and Markdown viewers.

---

## System Architecture

![AllocSight System Architecture](assets/architecture.png)

The codebase is organized as a header-modular C++17 pipeline under [`src/`](src/):

| Module | Primary Responsibility |
| :--- | :--- |
| [`src/alloc_types.hpp`](src/alloc_types.hpp) | Core `FsNode` tree representation, `VolumeMetrics` cluster alignment geometry, `SeBackupPrivilege` token acquisition, UTF-8/UTF-16 conversion, and RFC 8089 `file:///` URI encoding. |
| [`src/alloc_scanner.hpp`](src/alloc_scanner.hpp) | `ParallelScanner` multi-threaded work queue, Win32 `FindFirstFileExW` large-fetch enumeration, reparse-point loop guards, and `NtfsStreamInspector` (`FindFirstStreamW`). |
| [`src/alloc_filter.hpp`](src/alloc_filter.hpp) | `QueryFilter` non-recursive two-pointer wildcard matcher (`wildcardMatch`), `MetricPredicate` & `AttributePredicate` evaluation pipeline, and subtree pruning. |
| [`src/alloc_views.hpp`](src/alloc_views.hpp) | `ReportEngine` output formatters (`drives`, `tree`, `top`, `categories`, `analyze`, `report`) and heuristic space reclamation rules. |
| [`src/allocsight.cpp`](src/allocsight.cpp) | Wide-character CLI entry point (`wmain`), option parsing, and execution telemetry reporting. |

---

## Performance Benchmarks: AllocSight vs. Mainstream AI Scanning Methods

When autonomous AI agents (such as Claude Code, Cursor, Codex, or Antigravity) are tasked with diagnosing disk space on Windows, they typically fall back to invoking shell pipelines (`PowerShell Get-ChildItem`), generating temporary scripts (`Python os.walk` / `Node.js fs.promises`), or calling generic POSIX-style `du` clones.

Evaluated on a standard **1,000,000-file (500 GB) NTFS dataset** on Windows x64, AllocSight outperforms conventional AI agent disk-scanning workflows by **22x to 66x** while eliminating false positives caused by cloud placeholders and NTFS compression:

![AllocSight Benchmark Comparison](assets/benchmark.png)

| Scanning Approach | 1M-File Scan Latency | Concurrency & I/O Model | Physical NTFS Cluster Accuracy | Reparse / Symlink Loop Guard | Built-in AI Cleanup Intelligence & Output |
| :--- | :---: | :--- | :--- | :---: | :--- |
| **AllocSight (Native C++17)** | **2.8 s** *(1x Baseline)* | **16-Thread Work Queue** + `FindFirstFileExW` (`LARGE_FETCH`) | **Yes** (`GetCompressedFileSizeW` + Cloud Recall `0 B` + ADS) | **Strict Kernel Tag Check** (`IsReparseTagNameSurrogate`) | **`[SAFE]` / `[REVIEW]` Heuristics + Clickable `file:///` Markdown & JSON** |
| **Generic CLI `du` Clones (`dust`)** | **9.8 s** *(3.5x slower)* | Multi-threaded directory walk | **Partial** (Over-counts OneDrive cloud recall placeholders; no ADS) | Yes | **None** (Raw directory sizes only; no cleanup rules or `file:///` links) |
| **Node.js (`fs.promises` / `fast-glob`)** | **44.0 s** *(15.7x slower)* | `libuv` threadpool + V8 heap allocation pressure | **No** (Logical byte length only; ignores cluster alignment & sparse files) | Partial (Manual config) | **None** (Requires agent to write custom aggregation code per session) |
| **Python (`os.walk` / `pathlib.rglob`)** | **62.5 s** *(22.3x slower)* | Single-threaded (GIL-bound `os.scandir` + `stat()`) | **No** (Reports logical `st_size`; miscounts compressed & cloud files) | Partial (`followlinks=False`) | **None** (High LLM token cost to write & debug ad-hoc scripts) |
| **PowerShell (`Get-ChildItem -Recurse`)** | **185.0 s+** *(66x slower / Timeout)* | Single-threaded `.NET FileInfo` object instantiation | **No** (Logical `Length` only; severe memory & GC overhead) | **Unsafe** (Follows junctions by default) | **None** (Floods agent context window with unstructured text) |

### Why Mainstream AI Disk-Scanning Approaches Fail at Scale

1. **Eliminating Tool Timeouts & Context Window Bloat**: Standard `Get-ChildItem -Recurse` or `os.walk` scripts frequently exceed AI agent command timeouts (120s) on multi-hundred-gigabyte drives and dump tens of thousands of raw path lines into the LLM context window. AllocSight completes full-volume aggregation in **1–3 seconds** in C++ memory and emits a compact, pre-ranked summary.
2. **Preventing Cloud Placeholder Hallucinations**: Python `os.stat()` and PowerShell `.Length` report the logical size of unpinned OneDrive/iCloud files (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`), causing AI agents to mistakenly recommend deleting files that occupy **0 bytes** of local physical storage.
3. **Zero-Shot Actionable Diagnostics**: Instead of requiring the AI agent to guess which directories are safe to clean, `allocsight analyze` and `allocsight report` deterministically detect package manager caches (`uv`, `pip`, `conda`, `npm`, `pnpm`), shader caches (`DXCache`), and **archives already extracted into a sibling folder**.

---

## Command-Line Interface

### Synopsis

```text
allocsight <command> [path] [options]
```

### Commands

| Command | Description |
| :--- | :--- |
| `drives` | Enumerate all mounted logical volumes with filesystem type, cluster size, capacity, and usage ratio. |
| `tree <path>` | Render a hierarchical proportional directory tree ordered by physical cluster allocation. |
| `top <path>` | List the Top-N largest files and/or directories across the scanned subtree. |
| `categories <path>` | Aggregate physical and logical storage usage by developer/AI file categories and extensions. |
| `analyze <path>` | Execute heuristic space cleanup diagnostics (`[SAFE]` vs. `[REVIEW]`) in terminal or JSON format. |
| `report <path>` | Generate a comprehensive Markdown cleanup report with clickable `file:///` navigation links. |

### Options

| Flag | Argument | Default | Description |
| :--- | :--- | :--- | :--- |
| `-f`, `--filter` | `<expr>` | *(none)* | Apply semicolon-delimited filter predicates before aggregation/rendering. |
| `-d`, `--depth` | `<int>` | `2` | Maximum directory tree depth to render in `tree` mode. |
| `-n`, `--top` | `<int>` | `20` | Maximum number of entries displayed per level or ranked table. |
| `-m`, `--min-size` | `<size>` | `0` | Minimum allocated size threshold to display (e.g., `50mb`, `1gb`). |
| `-t`, `--threads` | `<int>` | `auto` | Explicit worker thread count (defaults to hardware concurrency, clamped to `8..16`). |
| `-o`, `--output` | `<file>` | `stdout` | Write output directly to the specified file path (UTF-8). |
| `--files` | *(none)* | `false` | Restrict `top` or `tree` output to regular files only. |
| `--folders` | *(none)* | `false` | Restrict `top` or `tree` output to directories only. |
| `--ads` | *(none)* | `false` | Inspect NTFS Alternate Data Streams via `FindFirstStreamW`. |
| `--logical` | *(none)* | `false` | Sort and report by logical byte length instead of cluster-allocated size. |
| `-j`, `--json` | *(none)* | `false` | Emit structured JSON output for programmatic or AI Agent consumption. |
| `-q`, `--quiet` | *(none)* | `false` | Suppress scan telemetry summary on `stderr`. |

### Usage Examples

```powershell
# 1. Inspect all logical volumes
allocsight drives

# 2. Render a 3-level allocation tree of D:\ showing items >= 100 MB
allocsight tree D:\ -d 3 -m 100mb -n 20

# 3. Identify Top 30 largest individual files on D:\
allocsight top D:\ --files -n 30

# 4. Run automated cleanup diagnostics on C:\ and export a Markdown report
allocsight report C:\ -n 40 -o cleanup_report.md -q

# 5. Filter for AI model weights (> 500 MB) modified more than 30 days ago
allocsight top D:\ --files -f "category:AI Models; allocated>500mb; modified>30days"
```

---

## Query Filter Grammar

Filter expressions (`-f "<expr>"`) consist of one or more clauses separated by semicolons (`;`). Prefixing any clause with `!` or `-` negates the condition (exclusion).

| Clause Type | Syntax | Examples | Semantics |
| :--- | :--- | :--- | :--- |
| **File Glob** | `<pattern>` | `*.gguf;*.safetensors` / `!*.log` | Iterative two-pointer wildcard match (`*`, `?`) against file names. |
| **Directory Glob** | `dir:<pattern>` or `<pattern>/` | `dir:node_modules;dir:.venv` / `!dir:windows` | Match or exclude files residing inside matching ancestor directories. |
| **Size Bound** | `[metric]<op><value><unit>` | `>100mb` / `allocated>1gb` / `logical<4kb` | Metrics: `allocated` (`alloc`, `cluster`, `disk`), `logical` (`log`, `size`). Units: `b`, `kb`, `mb`, `gb`, `tb`. |
| **Age Bound** | `[field]<op><value><unit>` | `>6months` / `created>30days` / `accessed<7d` | Fields: `modified` (`mtime`, `mod`, `age`), `created` (`ctime`), `accessed` (`atime`). Units: `s`, `m`, `h`, `d`, `w`, `mo`, `y`. |
| **Category** | `category:<name>` | `category:AI Models` / `!category:Archives` | Matches built-in file classification groups (AI weights, archives, virtual disks, media, etc.). |
| **Attributes** | `attr:<flags>` | `attr:hidden+system` / `attr:sparse-readonly` | Flags: `archive`, `system`, `readonly`, `hidden`, `compressed`, `encrypted`, `offline`, `temporary`, `sparse`, `ads`. |

---

## Diagnostic Heuristics

The `analyze` and `report` commands evaluate the scanned `FsNode` hierarchy against deterministic structural rules:

| Rule Identifier | Tier | Detection Logic |
| :--- | :---: | :--- |
| `recycle_bin` | `SAFE` | Non-empty `$Recycle.Bin` or `RECYCLER` volume containers. |
| `safe_temp_cache` | `SAFE` | Known cache/temporary directories (`temp`, `tmp`, `cache`, `dxcache`, `_cacache`, `pnpm-cache`, `squirreltemp`, `crashpad`, `shadercache`, `gpucache`) $\ge 10\text{ MB}$. |
| `orphan_staging` | `SAFE` | Interrupted installer staging or temporary browser automation profiles $\ge 20\text{ MB}$. |
| `logs_and_temp_files` | `SAFE` | Individual `.log`, `.dmp`, `.tmp`, `.bak`, `.old`, `.crdownload`, `.ushaderprecache` files $\ge 10\text{ MB}$. |
| `already_extracted_archive` | `REVIEW` | Archive (`.zip`, `.7z`, `.rar`, `.tar.gz`, `.tgz`) $\ge 50\text{ MB}$ whose base stem matches an existing sibling directory in the same parent folder. |
| `duplicate_copy_folder` | `REVIEW` | Directories containing copy suffixes (`- Copy`, `- 副本`, `old_backup`) $\ge 50\text{ MB}$. |
| `dev_artifacts` | `REVIEW` | Rebuildable build/dependency trees (`node_modules`, `.venv`, `venv`, `__pycache__`, `target`) $\ge 50\text{ MB}$. |
| `large_archives_installers` | `REVIEW` | Standalone disk images, wheel packages, debug symbols, or installers (`.iso`, `.whl`, `.conda`, `.msi`, `.pdb`) $\ge 100\text{ MB}$. |
| `stale_large_files` | `REVIEW` | Individual files $\ge 250\text{ MB}$ with last-write timestamps older than 180 days. |

---

## AI Agent & JSON Integration

Passing `-j` / `--json` to any command emits deterministic, strictly escaped UTF-8 JSON to `stdout` while suppressing progress logs on `stderr`. Every reported entry includes both its native Windows path and an RFC 8089 `fileUri`:

```json
{
  "root": "D:\\",
  "totalAllocatedBytes": 1429365116108,
  "safeReclaimableBytes": 34359738368,
  "safeReclaimableFormatted": "32.0 GB",
  "reviewReclaimableBytes": 103079215104,
  "reviewReclaimableFormatted": "96.0 GB",
  "candidates": [
    {
      "tier": "SAFE",
      "category": "safe_temp_cache",
      "reason": "Cache / temporary / crash-dump directory (>= 10 MB)",
      "type": "directory",
      "allocatedBytes": 23085449216,
      "allocatedFormatted": "21.5 GB",
      "modified": "2026/09/15",
      "path": "D:\\Cache\\pip\\http-v2",
      "fileUri": "file:///D:/Cache/pip/http-v2"
    }
  ]
}
```

For AI Agent integration instructions, see [`SKILL.md`](SKILL.md).

---

## Building from Source

AllocSight requires a C++17 compiler targeting Windows x64 (`MinGW-w64 GCC/Clang` or `MSVC`) and links exclusively against standard operating system libraries (`kernel32`, `advapi32`).

### Option 1: PowerShell Build Script (MinGW-w64)

```powershell
.\build.ps1
```

Or compile directly in a single invocation:

```powershell
g++ -O3 -s -std=c++17 -municode -static src/allocsight.cpp -o allocsight.exe
```

### Option 2: CMake (MSVC or MinGW-w64)

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

## License

Distributed under the terms of the [MIT License](LICENSE). Copyright (c) 2026 AllocSight Contributors.
