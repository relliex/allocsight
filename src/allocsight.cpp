// SPDX-License-Identifier: MIT
// AllocSight - High-Concurrency Disk Allocation & AI Space Cleanup Engine
// Copyright (c) 2026 AllocSight Contributors

#include "alloc_types.hpp"
#include "alloc_scanner.hpp"
#include "alloc_filter.hpp"
#include "alloc_views.hpp"
#include <chrono>
#include <fstream>

using namespace allocsight;

static int64_t parseSizeOption(const std::string& raw) {
    std::string low = asciiLower(trimWhitespace(raw));
    if (low.empty()) return 0;
    size_t i = 0;
    while (i < low.size() && ((low[i] >= '0' && low[i] <= '9') || low[i] == '.')) ++i;
    if (i == 0) return 0;
    double val = std::stod(low.substr(0, i));
    std::string unit = trimWhitespace(low.substr(i));
    int64_t mult = 1;
    if (unit == "k" || unit == "kb" || unit == "kib") mult = 1024LL;
    else if (unit == "m" || unit == "mb" || unit == "mib") mult = 1024LL * 1024LL;
    else if (unit == "g" || unit == "gb" || unit == "gib") mult = 1024LL * 1024LL * 1024LL;
    else if (unit == "t" || unit == "tb" || unit == "tib") mult = 1024LL * 1024LL * 1024LL * 1024LL;
    return static_cast<int64_t>(val * static_cast<double>(mult));
}

static void printBannerHelp() {
    std::cout <<
R"(================================================================================
 AllocSight v1.1.0 - High-Concurrency Disk Allocation & AI Space Cleanup CLI
 License: MIT (Zero-Dependency Native Win32 C++17 Implementation)
================================================================================

USAGE:
  allocsight drives                         List all logical volumes & capacity
  allocsight tree       <path> [options]    Hierarchical cluster-allocation tree
  allocsight top        <path> [options]    Top-N largest files & directories
  allocsight categories <path> [options]    Breakdown by AI/dev categories & exts
  allocsight analyze    <path> [options]    Smart cleanup & redundancy diagnostic
  allocsight report     <path> [options]    Generate clickable Markdown report

OPTIONS:
  -f, --filter "<expr>"    Filter expression (e.g. "*.whl;>100mb;dir:node_modules")
  -d, --depth <N>          Max directory tree depth to display (default: 2)
  -n, --top <N>            Max items per level or list (default: 20)
  -m, --min-size <size>    Minimum allocated size to display (e.g. 50mb, 1gb)
  -t, --threads <N>        Worker threads for parallel Win32 scan (default: auto)
  -o, --output <file>      Write output (e.g. Markdown report or JSON) to file
      --files              Include only regular files in top/tree
      --folders            Include only directories in top/tree
      --ads                Inspect NTFS Alternate Data Streams (FindFirstStreamW)
      --hardlinks          Deduplicate NTFS hard links (prevents WinSxS double counting)
      --logical            Rank/display by logical size instead of cluster size
  -j, --json               Emit structured JSON (with file:/// URIs) for AI agents
  -q, --quiet              Suppress stderr scan telemetry summary

FILTER SYNTAX EXAMPLES:
  - File globs   : *.gguf;*.safetensors  !*.log (prefix '!' or '-' to exclude)
  - Folder globs : dir:node_modules;dir:.venv  !dir:windows (or trailing '/')
  - Size bounds  : >100mb  <4kb  allocated>1gb  logical>500mb
  - Time / Age   : >6months  <7days  created>30days  modified>1year
  - Categories   : category:AI Models    category:Archives    category:Virtual
  - Attributes   : attr:hidden+system    attr:compressed      attr:sparse+ads
================================================================================
)";
}

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    if (argc <= 1) {
        printBannerHelp();
        return 0;
    }

    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    std::string cmd = asciiLower(wideToUtf8(args[0]));
    if (cmd == "help" || cmd == "-h" || cmd == "--help" || cmd == "/?") {
        printBannerHelp();
        return 0;
    }
    if (cmd == "version" || cmd == "-v" || cmd == "--version") {
        std::cout << "AllocSight v1.1.0 (MIT License)\n";
        return 0;
    }

    cmd = "tree";
    size_t cmdArgIdx = static_cast<size_t>(-1);

    for (size_t i = 0; i < args.size(); ++i) {
        std::string s = asciiLower(wideToUtf8(args[i]));
        if (s == "drives" || s == "tree" || s == "top" ||
            s == "categories" || s == "analyze" || s == "report" || s == "scan") {
            cmd = s;
            cmdArgIdx = i;
            break;
        }
    }

    ScanConfig scanCfg{};
    RenderOptions renderOpts{};
    std::string filterExpr;
    std::wstring outputPath;
    std::vector<std::wstring> positional;

    for (size_t idx = 0; idx < args.size(); ++idx) {
        if (idx == cmdArgIdx) continue;
        std::string a = wideToUtf8(args[idx]);
        if ((a == "-f" || a == "--filter") && idx + 1 < args.size()) {
            filterExpr = wideToUtf8(args[++idx]);
        } else if ((a == "-d" || a == "--depth") && idx + 1 < args.size()) {
            renderOpts.maxDepth = std::stoi(wideToUtf8(args[++idx]));
        } else if ((a == "-n" || a == "--top") && idx + 1 < args.size()) {
            renderOpts.topLimit = std::stoi(wideToUtf8(args[++idx]));
        } else if ((a == "-m" || a == "--min-size") && idx + 1 < args.size()) {
            renderOpts.minBytes = parseSizeOption(wideToUtf8(args[++idx]));
        } else if ((a == "-t" || a == "--threads") && idx + 1 < args.size()) {
            scanCfg.workerThreads = std::stoi(wideToUtf8(args[++idx]));
        } else if ((a == "-o" || a == "--output") && idx + 1 < args.size()) {
            outputPath = args[++idx];
        } else if (a == "--files") {
            renderOpts.filesOnly = true;
        } else if (a == "--folders" || a == "--dirs") {
            renderOpts.dirsOnly = true;
        } else if (a == "--ads") {
            scanCfg.includeAltStreams = true;
        } else if (a == "--hardlinks") {
            scanCfg.detectHardLinks = true;
        } else if (a == "--logical") {
            renderOpts.useLogical = true;
        } else if (a == "-j" || a == "--json") {
            renderOpts.jsonOutput = true;
        } else if (a == "-q" || a == "--quiet") {
            scanCfg.quietMode = true;
        } else {
            positional.push_back(args[idx]);
        }
    }

    std::ofstream fileOut;
    std::ostream* outStream = &std::cout;
    if (!outputPath.empty()) {
        fileOut.open(outputPath.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!fileOut) {
            std::cerr << "[ERROR] Cannot open output file: " << wideToUtf8(outputPath) << "\n";
            return 1;
        }
        outStream = &fileOut;
    }

    if (cmd == "drives") {
        ReportEngine::renderDrives(renderOpts.jsonOutput, *outStream);
        return 0;
    }

    if (positional.empty()) {
        std::cerr << "[ERROR] Missing target directory or volume path.\n";
        printBannerHelp();
        return 1;
    }

    auto tStart = std::chrono::steady_clock::now();
    ScanTelemetry telemetry{};
    VolumeMetrics volInfo{};
    auto root = ParallelScanner::scan(positional[0], scanCfg, telemetry, &volInfo);
    auto tEnd = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(tEnd - tStart).count();

    if (!renderOpts.jsonOutput && !scanCfg.quietMode) {
        std::cerr << "[AllocSight scanned in " << std::fixed << std::setprecision(2) << secs << "s | "
                  << telemetry.dirsCount.load() << " dirs, "
                  << telemetry.filesCount.load() << " files";
        if (telemetry.hardLinksCount.load() > 0) {
            std::cerr << ", " << telemetry.hardLinksCount.load() << " hardlinks deduped ("
                      << humanBytes(telemetry.hardLinksBytesSaved.load()) << " saved)";
        }
        std::cerr << " | Threads=" << telemetry.threadsUsed << ", "
                  << "Cluster=" << volInfo.clusterBytes << "B, "
                  << "BackupPriv=" << (telemetry.backupPrivilegeEnabled ? "ON" : "OFF") << "]\n";
    }

    if (!filterExpr.empty()) {
        QueryFilter qf(filterExpr);
        for (const auto& w : qf.warnings()) {
            std::cerr << "[Filter Warning] " << w << "\n";
        }
        if (!qf.isEmpty()) {
            root = qf.applyToTree(root.get());
        }
    }

    if (!root) {
        std::cerr << "[ERROR] Target produced an empty or inaccessible tree.\n";
        return 1;
    }

    if (cmd == "top") {
        ReportEngine::renderTop(root.get(), renderOpts, *outStream);
    } else if (cmd == "categories") {
        QueryFilter qf(filterExpr);
        ReportEngine::renderCategories(root.get(), qf, renderOpts, *outStream);
    } else if (cmd == "analyze") {
        ReportEngine::renderAnalyze(root.get(), renderOpts, *outStream);
    } else if (cmd == "report") {
        ReportEngine::renderMarkdownReport(root.get(), renderOpts, *outStream);
        if (!outputPath.empty() && !scanCfg.quietMode) {
            std::cerr << "[Report written to " << wideToUtf8(outputPath) << "]\n";
        }
    } else {
        ReportEngine::renderTree(root.get(), renderOpts, *outStream);
    }

    return 0;
}
