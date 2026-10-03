![AllocSight Banner](assets/banner.png)

[![License: MIT](https://img.shields.io/badge/License-MIT-0284c7.svg?style=flat-square)](../../LICENSE)
[![Standard: C++17](https://img.shields.io/badge/C%2B%2B-17-4f46e5.svg?style=flat-square)](../../src/allocsight.cpp)
[![Platform: Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0f766e.svg?style=flat-square)](../../CMakeLists.txt)
[![Dependencies: Zero](https://img.shields.io/badge/Dependencies-None-059669.svg?style=flat-square)](../../build.ps1)

[**English**](../../README.md) | [**简体中文**](README.zh-CN.md) | [**Español**](README.es.md) | [**Français**](README.fr.md) | [**Русский**](README.ru.md) | [**العربية**](README.ar.md)

---

## 目录

- [项目概述](#项目概述)
- [核心技术特性](#核心技术特性)
- [系统架构与执行管线](#系统架构与执行管线)
- [性能基准测试](#性能基准测试)
- [命令行接口参考](#命令行接口参考)
- [查询过滤器语法规范](#查询过滤器语法规范)
- [空间回收启发式诊断规则](#空间回收启发式诊断规则)
- [AI Agent 与 JSON 遥测集成](#ai-agent-与-json-遥测集成)
- [源码编译指南](#源码编译指南)
- [开源许可](#开源许可)

---

## 项目概述

**AllocSight** 是一款面向 Windows 平台的高并发、零第三方依赖的磁盘簇空间分析与自动化清理诊断命令行引擎。项目采用原生 C++17 标准直接构建于 Win32 文件管理内核接口之上，专为软件工程师、系统管理员以及自动化 AI Agent 打造，在彻底剥离图形渲染开销的同时提供亚秒级到秒级的全盘存储遥测能力。

不同于仅统计文件逻辑字节长度（`nFileSizeLow` / `nFileSizeHigh`）的传统目录统计工具，AllocSight 直接按目标卷的真实 **NTFS 物理簇分配（Physical Cluster Allocation）** 进行空间核算，精确处理文件系统簇对齐、NTFS LZNT1/XPRESS 透明压缩、稀疏文件（Sparse Files）、云端零占用占位符以及 NTFS 备用数据流（ADS）。

![AllocSight Terminal Output](assets/terminal-demo.png)

---

## 核心技术特性

1. **高并发内核级目录枚举**
   通过最小化锁竞争的多线程工作队列（`ParallelScanner`）调度 8 至 16 个工作线程。每个工作线程调用 `FindFirstFileExW`，并启用 `FindExInfoBasic`（跳过 8.3 短文件名查询）与 `FIND_FIRST_EX_LARGE_FETCH`（开启内核大页目录缓冲批量读取）。
2. **精确的物理磁盘簇核算**
   - 根据 `GetDiskFreeSpaceW` 获取的目标卷簇大小（Cluster Size）精确计算物理对齐字节数。
   - 对带有 `FILE_ATTRIBUTE_COMPRESSED` 或 `FILE_ATTRIBUTE_SPARSE_FILE` 属性的文件，通过 `GetCompressedFileSizeW` 读取真实磁盘落盘字节。
   - 自动识别未落盘的云同步占位文件（`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`、`FILE_ATTRIBUTE_RECALL_ON_OPEN`、`FILE_ATTRIBUTE_OFFLINE`）并将其物理簇占用记为 `0`，避免 OneDrive 等云盘虚增占用统计。
   - 支持通过公开的 `FindFirstStreamW` / `FindNextStreamW` 接口（`--ads`）审计隐藏在文件或目录背后的 NTFS 备用数据流（`:$DATA`）。
3. **重解析点（Reparse Point）死循环防护**
   基于 `<winnt.h>` 标准宏（`IsReparseTagNameSurrogate`、`IO_REPARSE_TAG_SYMLINK`、`IO_REPARSE_TAG_MOUNT_POINT`）校验重解析标签，严格阻断由符号链接与目录交接点（Junctions）引发的无限递归死循环。
4. **双层启发式空间清理诊断引擎**
   自动将可回收空间划分为两个明确的安全等级：
   - **`[SAFE]`（安全直清）**：包管理器缓存（`uv`、`pip`、`conda/pkgs`、`npm/_cacache`、`pnpm-cache`）、着色器与图形缓存（`DXCache`、`ShaderCache`）、崩溃转储以及残留的临时安装目录。
   - **`[REVIEW]`（建议审阅）**：同级目录下已存在同名解压文件夹的冗余压缩包、带有副本后缀（`- 副本` / `- Copy`）的备份目录、可随时重建的构建与依赖产物（`target`、`node_modules`、`.venv`）以及超过 180 天未修改的陈旧大文件。
5. **支持一键跳转的 Markdown 与 JSON 报告**
   支持一键导出结构化 Markdown 报告（`allocsight report`）与机器可读的 JSON 数据流（`-j`）。所有目录与文件路径均附带符合 RFC 8089 标准的百分号编码 `file:///` URI，支持在现代 IDE 与 Markdown 阅读器中直接点击打开对应文件夹。

---

## 系统架构与执行管线

![AllocSight System Architecture](assets/architecture.png)

代码库采用模块化头文件架构，位于 [`src/`](../../src/) 目录下：

| 模块文件 | 核心职责说明 |
| :--- | :--- |
| [`src/alloc_types.hpp`](../../src/alloc_types.hpp) | 定义 `FsNode` 内存文件树节点、`VolumeMetrics` 磁盘簇几何对齐计算、`SeBackupPrivilege` 备份特权令牌提权、UTF-8/UTF-16 转换及 RFC 8089 `file:///` 链接编码。 |
| [`src/alloc_scanner.hpp`](../../src/alloc_scanner.hpp) | 实现 `ParallelScanner` 多线程目录工作队列、Win32 `FindFirstFileExW` 大页枚举、重解析点死循环拦截以及 `NtfsStreamInspector`（`FindFirstStreamW`）。 |
| [`src/alloc_filter.hpp`](../../src/alloc_filter.hpp) | 实现 `QueryFilter` 非递归双指针回溯通配符匹配器（`wildcardMatch`）、`MetricPredicate` 与 `AttributePredicate` 规则向量求值及子树裁剪重聚合。 |
| [`src/alloc_views.hpp`](../../src/alloc_views.hpp) | 实现 `ReportEngine` 六大视图渲染器（`drives`、`tree`、`top`、`categories`、`analyze`、`report`）及启发式空间清理分类规则。 |
| [`src/allocsight.cpp`](../../src/allocsight.cpp) | 宽字符命令行入口（`wmain`）、参数解析器以及扫描性能遥测摘要输出。 |

---

## 性能基准测试：与主流 AI 扫盘方案的横向对比

当主流 AI Agent（如 Claude Code、Cursor、Codex、Antigravity 等）在 Windows 平台上执行磁盘空间诊断与清理任务时，通常只能临时调用系统 Shell 管道（`PowerShell Get-ChildItem`）、现场编写脚本（`Python os.walk` / `Node.js fs`），或调用通用的 CLI 工具。

为了提供 100% 真实、严禁作弊、完全可复现的对比数据，我们在配置 4 KB 簇大小的 NTFS 固态硬盘上生成了包含 **100,000 个文件、1,100 个目录** 的确定性基准测试语料库（每个文件 512 字节：**逻辑数据 48.8 MB，真实物理簇占用 390.6 MB**）。在完全相同的热缓存环境下对 11 种常见工具进行了基准实测（中位数取 5 次运行，可通过 [`benchmarks/run_benchmark.ps1`](../../benchmarks/run_benchmark.ps1) 完整复现）：

![AllocSight Benchmark Comparison](assets/benchmark.png)

| 扫盘方案 / 工具类型 | 扫描实测耗时 | 文件吞吐率 | 相对耗时倍率 | 物理簇核算 (390.6 MB) | 结构化 JSON 输出 | 死循环与云占位安全 |
| :--- | :---: | :---: | :---: | :---: | :---: | :---: |
| **dua v2.45.0 (Rust `jwalk` 并发)** | **142.7 ms** | **700,771 个/秒** | **0.72x (快 1.39 倍)** | 仅逻辑大小 (Windows 下 48.8 MB) | 无 (交互终端 / 文本) | 基础符号链接过滤 |
| **AllocSight v1.0.0 (原生 C++17)** | **199.1 ms** | **502,260 个/秒** | **1.00x (基准)** | **100% 精确 (物理+逻辑双核算)** | **内置支持 (`file:///` 协议)** | **内核代号校验 + 云占位 0 字节保护** |
| **Robocopy (`/L /S /BYTES /MT:16`)** | **223.7 ms** | 447,027 个/秒 | 慢 1.12 倍 | 仅逻辑大小 (48.8 MB) | 无 (仅扁平文本汇总) | 内置基础遍历 |
| **PowerShell 7 (`.NET EnumerateFiles`)** | **398.6 ms** | 250,878 个/秒 | 慢 2.00 倍 | 仅逻辑大小 (48.8 MB) | 无 (需编写脚本) | 遇只读/权限拒绝目录直接崩溃 |
| **Python 3.13 (`os.scandir` + 缓存 stat)** | **420.4 ms** | 237,869 个/秒 | 慢 2.11 倍 | 仅逻辑大小 (48.8 MB) | 需编写脚本 | 单线程受 GIL 限制 |
| **CMD (`cmd.exe /c dir /s /a /-c`)** | **1,172.5 ms** | 85,288 个/秒 | 慢 5.89 倍 | 仅逻辑大小 (48.8 MB) | 无 (倾倒数千行文本撑爆上下文) | 无法处理深层循环符号链接 |
| **PowerShell 7 (`Get-ChildItem -Recurse`)** | **2,361.9 ms** | 42,339 个/秒 | 慢 11.86 倍 | 仅逻辑大小 (48.8 MB) | 无 (`FileInfo` 对象开销极大) | 默认深入交接点引发死循环 |
| **dust v1.2.6 (Rust `rayon` 并发)** | **3,313.3 ms** | 30,181 个/秒 | 慢 16.64 倍 | 精确 (390.6 MB) | 支持 (`-j`) | 逐文件打开内核句柄查 ID 致耗时飙升 |
| **Node.js v22 (`fs.readdirSync` + stat)** | **19,791.8 ms** | 5,053 个/秒 | 慢 99.41 倍 | 仅逻辑大小 (48.8 MB) | 需编写脚本 | `Dirent` 无大小，触发 10 万次 stat 系统调用 |
| **Python 3.13 (`os.walk` + `os.path.getsize`)** | **20,361.3 ms** | 4,911 个/秒 | 慢 102.27 倍 | 仅逻辑大小 (48.8 MB) | 需编写脚本 | 触发 10 万次冗余 `GetFileAttributesExW` 调用 |
| **Sysinternals `du64.exe` v1.62** | **43,204.1 ms** | 2,315 个/秒 | 慢 217.00 倍 | 精确 (390.6 MB) | 无 (仅控制台打印) | 单线程逐文件流式探测 |

### 技术权衡与客观根因分析

1. **客观承认：`dua` 在纯粹扫描耗时上的领先（142.7 ms vs 199.1 ms）**：
   在纯粹将逻辑大小无脑累加的场景下，Rust 编写的 `dua` 比 AllocSight 快了约 56 毫秒。这是因为 `dua` 在内核遍历中直接在 CPU 寄存器内做整数相加，完全不分配内存来构建目录树结构，也不进行任何 4 KB 物理簇对齐数学计算，更不进行 AI/开发文件分类。
2. **AllocSight 在准确性与工程深度上的优势**：
   - **双重空间精确核算**：在 4 KB 簇大小的 NTFS 卷上，小文件真实占用整个物理簇。`dua` 仅报出 48.8 MB（严重少报了 87.5% 的真实占用），而 AllocSight 能够同时精准输出 390.6 MB 真实物理占用与 48.8 MB 逻辑大小。
   - **全量内存树与 AI 结构化支持**：AllocSight 在内存中构建完整的 `FsNode` 树形结构，支持子树排序、目录占比计算、启发式冗余诊断（`[SAFE]` / `[REVIEW]`）并直接生成带 `file:///` 超链接的 JSON/Markdown。
3. **原生 Win32 大页目录流批处理 (`FIND_FIRST_EX_LARGE_FETCH`)**：
   AllocSight 批量汲取目录项。Python `os.walk` 与 Node.js `fs` 丢弃了底层目录流已包含的文件大小元数据，导致后续触发了超过 100,000 次冗余的内核属性系统调用，耗时均达到 20 秒左右（慢约 100 倍）。
4. **云同步占位文件零落盘保护 (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`)**：
   未本地下载的 OneDrive / iCloud 云端文件逻辑大小巨大，但实际磁盘占用为 0。AllocSight 自动识别云占位属性并将其实际物理占用计为 0 字节，防止 AI 产生误判决策。

---


## 命令行接口参考

### 命令格式

```text
allocsight <command> [path] [options]
```

### 子命令列表

| 子命令 | 功能描述 |
| :--- | :--- |
| `drives` | 列出本机所有已挂载逻辑卷的文件系统类型、簇大小、总容量、已分配空间与使用率。 |
| `tree <path>` | 按物理磁盘簇占用从大到小输出多级目录比例树视图。 |
| `top <path>` | 跨子树全局排序并列出占用最大的 Top-N 文件或目录。 |
| `categories <path>` | 按 AI/开发者文件分类（模型权重、压缩包、虚拟磁盘、数据库等）及扩展名汇总占用。 |
| `analyze <path>` | 运行启发式空间清理诊断引擎（区分 `[SAFE]` 与 `[REVIEW]`），支持终端或 JSON 输出。 |
| `report <path>` | 生成包含可点击 `file:///` 路径链接的结构化 Markdown 磁盘清理报告。 |

### 参数选项

| 选项标志 | 参数值 | 默认值 | 说明 |
| :--- | :--- | :--- | :--- |
| `-f`, `--filter` | `<expr>` | *(无)* | 在聚合与渲染前应用分号分隔的过滤条件表达式。 |
| `-d`, `--depth` | `<int>` | `2` | `tree` 模式下展开的最大目录层级深度。 |
| `-n`, `--top` | `<int>` | `20` | 每个层级或排行榜输出的最大条目数。 |
| `-m`, `--min-size` | `<size>` | `0` | 最小显示空间阈值（如 `50mb`、`1gb`）。 |
| `-t`, `--threads` | `<int>` | `auto` | 指定并发扫描工作线程数（默认根据 CPU 硬件并发数自动设定为 `8..16`）。 |
| `-o`, `--output` | `<file>` | `stdout` | 将输出结果直接写入指定文件（UTF-8 编码）。 |
| `--files` | *(无)* | `false` | 在 `top` 或 `tree` 视图中仅包含普通文件。 |
| `--folders` | *(无)* | `false` | 在 `top` 或 `tree` 视图中仅包含目录。 |
| `--ads` | *(无)* | `false` | 使用 `FindFirstStreamW` 扫描 NTFS 备用数据流。 |
| `--hardlinks` | *(无)* | `false` | 去重 NTFS 硬链接（防止 WinSxS / 系统目录重复累加物理大小）。 |
| `--logical` | *(无)* | `false` | 按逻辑字节大小而非物理簇分配大小进行排序和展示。 |
| `-j`, `--json` | *(无)* | `false` | 输出结构化 JSON，供程序或 AI Agent 解析调用。 |
| `-q`, `--quiet` | *(无)* | `false` | 静默模式，不在 `stderr` 输出扫描耗时与遥测摘要。 |

### 常用命令示例

```powershell
# 1. 查看所有逻辑磁盘分区概览
allocsight drives

# 2. 扫描 D:\ 前 3 层目录树，仅显示占用 >= 100 MB 的目录或文件
allocsight tree D:\ -d 3 -m 100mb -n 20

# 3. 找出 D:\ 盘中体积最大的前 30 个单文件
allocsight top D:\ --files -n 30

# 4. 诊断 C:\ 盘可回收空间并导出可点击跳转的 Markdown 报告
allocsight report C:\ -n 40 -o cleanup_report.md -q

# 5. 筛选 D:\ 盘中大于 500 MB 且超过 30 天未修改的 AI 模型权重文件
allocsight top D:\ --files -f "category:AI Models; allocated>500mb; modified>30days"
```

---

## 查询过滤器语法规范

过滤表达式（`-f "<expr>"`）由一个或多个以分号（`;`）分隔的子句组成。在任意子句前添加 `!` 或 `-` 表示排除（取反）。

| 过滤类型 | 语法格式 | 示例 | 匹配语义 |
| :--- | :--- | :--- | :--- |
| **文件名通配** | `<pattern>` | `*.gguf;*.safetensors` / `!*.log` | 使用非递归双指针算法匹配文件名通配符（`*`、`?`）。 |
| **目录名通配** | `dir:<pattern>` 或 `<pattern>/` | `dir:node_modules;dir:.venv` / `!dir:windows` | 匹配或排除位于指定名称父目录（任意嵌套深度）下的文件。 |
| **空间上下界** | `[metric]<op><value><unit>` | `>100mb` / `allocated>1gb` / `logical<4kb` | 指标：`allocated`（`alloc`, `cluster`, `disk`）、`logical`（`log`, `size`）。单位：`b`, `kb`, `mb`, `gb`, `tb`。 |
| **时间与文件年龄** | `[field]<op><value><unit>` | `>6months` / `created>30days` / `accessed<7d` | 字段：`modified`（`mtime`, `mod`, `age`）、`created`（`ctime`）、`accessed`（`atime`）。单位：`s`, `m`, `h`, `d`, `w`, `mo`, `y`。 |
| **内置文件分类** | `category:<name>` | `category:AI Models` / `!category:Archives` | 匹配内置的开发者/AI 文件类型分组（模型权重、压缩包、虚拟磁盘、媒体等）。 |
| **系统文件属性** | `attr:<flags>` | `attr:hidden+system` / `attr:sparse-readonly` | 标志：`archive`, `system`, `readonly`, `hidden`, `compressed`, `encrypted`, `offline`, `temporary`, `sparse`, `ads`。 |

---

## 空间回收启发式诊断规则

`analyze` 与 `report` 子命令会对扫描生成的 `FsNode` 树执行以下确定性启发式规则检测：

| 规则标识 | 安全等级 | 判定逻辑说明 |
| :--- | :---: | :--- |
| `recycle_bin` | `SAFE` | 非空的系统回收站目录（`$Recycle.Bin` 或 `RECYCLER`）。 |
| `safe_temp_cache` | `SAFE` | 明确的缓存与临时目录（`temp`, `tmp`, `cache`, `dxcache`, `_cacache`, `pnpm-cache`, `squirreltemp`, `crashpad`, `shadercache`, `gpucache`）且占用 $\ge 10\text{ MB}$。 |
| `orphan_staging` | `SAFE` | 安装中断残留的运行时暂存目录或自动化浏览器临时配置目录，且占用 $\ge 20\text{ MB}$。 |
| `logs_and_temp_files` | `SAFE` | 单个体积 $\ge 10\text{ MB}$ 的 `.log`、`.dmp`、`.tmp`、`.bak`、`.old`、`.crdownload`、`.ushaderprecache` 文件。 |
| `already_extracted_archive` | `REVIEW` | 体积 $\ge 50\text{ MB}$ 的压缩包（`.zip`, `.7z`, `.rar`, `.tar.gz`, `.tgz`），且同一父目录下已存在同名解压文件夹。 |
| `duplicate_copy_folder` | `REVIEW` | 目录名包含副本标记（`- 副本`, `- Copy`, `old_backup`）且占用 $\ge 50\text{ MB}$。 |
| `dev_artifacts` | `REVIEW` | 可通过包管理器或编译器随时重建的依赖与构建产物目录（`node_modules`, `.venv`, `venv`, `__pycache__`, `target`）且占用 $\ge 50\text{ MB}$。 |
| `large_archives_installers` | `REVIEW` | 体积 $\ge 100\text{ MB}$ 的独立镜像、Python Wheel 包、调试符号或安装程序（`.iso`, `.whl`, `.conda`, `.msi`, `.pdb`）。 |
| `wsl_docker_vhdx` | `REVIEW` | WSL2 或 Docker Desktop 虚拟磁盘镜像（`ext4.vhdx` $\ge 2\text{ GB}$）；可通过 `wsl --shutdown` 和 `diskpart compact` 回收稀疏空间。 |
| `package_manager_caches` | `REVIEW` | 构建工具、包管理器与 AI 模型缓存（`.gradle`, `.m2`, `go-build`, `huggingface` $\ge 50\text{ MB}$）。 |
| `ide_system_caches` | `REVIEW` | IDE 工作区索引与符号缓存（`.vs`, `.idea`, `workspaceStorage` $\ge 50\text{ MB}$）。 |
| `bloated_git_pack` | `REVIEW` | 膨胀的 Git 对象包存储（`.git/objects/pack` $\ge 200\text{ MB}$）；可运行 `git gc --prune=now` 回收空间。 |
| `stale_large_files` | `REVIEW` | 体积 $\ge 250\text{ MB}$ 且最后修改时间距今超过 180 天的陈旧文件。 |

---

## AI Agent 与 JSON 遥测集成

在任意子命令后附加 `-j` / `--json` 参数，程序将在标准输出生成严格转义的 UTF-8 JSON 数据，并自动关闭 `stderr` 进度输出。所有候选条目均包含原生 Windows 路径与可点击的 `fileUri` 字段：

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

关于 AI Agent 的详细调用规范，请参阅 [`SKILL.md`](../../SKILL.md)。

---

## 源码编译指南

AllocSight 仅需支持 C++17 的 Windows x64 编译器（`MinGW-w64 GCC/Clang` 或 `MSVC`），仅链接 Windows 标准系统库（`kernel32`、`advapi32`）。

### 方式一：PowerShell 构建脚本（MinGW-w64）

```powershell
.\build.ps1
```

或直接单行编译：

```powershell
g++ -O3 -s -std=c++17 -municode -static src/allocsight.cpp -o allocsight.exe
```

### 方式二：CMake（支持 MSVC 或 MinGW-w64）

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

## 开源许可

本项目基于 [MIT License](../../LICENSE) 许可协议发布。Copyright (c) 2026 AllocSight Contributors.
