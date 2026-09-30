// SPDX-License-Identifier: MIT
// AllocSight - High-Concurrency Disk Allocation & AI Space Cleanup Engine
// Copyright (c) 2026 AllocSight Contributors

#pragma once
#include "alloc_types.hpp"
#include "alloc_filter.hpp"
#include <iostream>
#include <fstream>
#include <unordered_set>

namespace allocsight {

struct RenderOptions {
    int  maxDepth    = 2;
    int  topLimit    = 20;
    int64_t minBytes = 0;
    bool filesOnly   = false;
    bool dirsOnly    = false;
    bool useLogical  = false;
    bool jsonOutput  = false;
};

struct CleanupFinding {
    std::string category;
    std::string tier; // "SAFE" or "REVIEW"
    std::string rationale;
    const FsNode* node = nullptr;
};

class ReportEngine {
public:
    static int64_t metricBytes(const FsNode* n, bool logical) {
        return logical ? n->logicalBytes : n->allocatedBytes;
    }

    static std::string progressBar(double pct, int width = 12) {
        int filled = static_cast<int>(std::round((pct / 100.0) * width));
        if (filled < 0) filled = 0;
        if (filled > width) filled = width;
        return std::string(filled, '#') + std::string(width - filled, '.');
    }

    // 0. Enumerate all logical drives on the system
    static void renderDrives(bool jsonMode, std::ostream& out = std::cout) {
        wchar_t buf[512]{};
        DWORD len = GetLogicalDriveStringsW(511, buf);
        struct DriveRow {
            std::string root;
            std::string fs;
            int64_t total = 0;
            int64_t used  = 0;
            int64_t free  = 0;
            int64_t cluster = 0;
        };
        std::vector<DriveRow> rows;
        for (const wchar_t* p = buf; *p != L'\0' && (p - buf) < static_cast<ptrdiff_t>(len); p += wcslen(p) + 1) {
            UINT dtype = GetDriveTypeW(p);
            if (dtype != DRIVE_FIXED && dtype != DRIVE_REMOVABLE && dtype != DRIVE_RAMDISK) continue;
            VolumeMetrics vm(p);
            if (vm.totalCapacity <= 0) continue;
            rows.push_back(DriveRow{
                wideToUtf8(p),
                vm.fileSystemName.empty() ? "UNKNOWN" : vm.fileSystemName,
                vm.totalCapacity,
                vm.totalCapacity - vm.freeBytes,
                vm.freeBytes,
                vm.clusterBytes
            });
        }

        if (jsonMode) {
            out << "{\n  \"drives\": [\n";
            for (size_t i = 0; i < rows.size(); ++i) {
                const auto& r = rows[i];
                double pct = (r.total > 0) ? (static_cast<double>(r.used) * 100.0 / static_cast<double>(r.total)) : 0.0;
                out << "    {\"drive\": \"" << escapeJson(r.root) << "\", \"fileSystem\": \"" << escapeJson(r.fs)
                    << "\", \"totalBytes\": " << r.total << ", \"totalFormatted\": \"" << humanBytes(r.total)
                    << "\", \"usedBytes\": " << r.used << ", \"usedFormatted\": \"" << humanBytes(r.used)
                    << "\", \"freeBytes\": " << r.free << ", \"freeFormatted\": \"" << humanBytes(r.free)
                    << "\", \"usedPercent\": " << std::fixed << std::setprecision(1) << pct
                    << ", \"clusterBytes\": " << r.cluster << "}"
                    << (i + 1 < rows.size() ? "," : "") << "\n";
            }
            out << "  ]\n}\n";
            return;
        }

        out << "================================================================================\n";
        out << " AllocSight - Logical Volumes Overview\n";
        out << "================================================================================\n";
        out << "Drive   FS       Total Cap     Allocated     Free Space    Usage\n";
        out << "------  -------  ------------  ------------  ------------  ---------------------\n";
        for (const auto& r : rows) {
            double pct = (r.total > 0) ? (static_cast<double>(r.used) * 100.0 / static_cast<double>(r.total)) : 0.0;
            char line[160];
            std::snprintf(line, sizeof(line), "%-6s  %-7s  %12s  %12s  %12s  [%s] %5.1f%%\n",
                r.root.c_str(),
                r.fs.c_str(),
                humanBytes(r.total).c_str(),
                humanBytes(r.used).c_str(),
                humanBytes(r.free).c_str(),
                progressBar(pct, 12).c_str(),
                pct
            );
            out << line;
        }
    }

    // 1. Hierarchical proportional tree view
    static void renderTree(const FsNode* root, const RenderOptions& opts, std::ostream& out = std::cout) {
        if (!root) return;
        int64_t rootTotal = metricBytes(root, opts.useLogical);
        if (opts.jsonOutput) {
            out << "{\n";
            emitNodeJson(root, 0, opts.maxDepth, opts.topLimit, opts.minBytes, opts.useLogical, rootTotal, 2, out);
            out << "\n}\n";
            return;
        }

        uint64_t totalFiles = 0, totalDirs = 0;
        root->countSubtree(totalFiles, totalDirs);

        out << "================================================================================\n";
        out << " AllocSight - Hierarchical Allocation Tree\n";
        out << " Root      : " << wideToUtf8(root->fullPath()) << "\n";
        out << " Allocated : " << humanBytes(root->allocatedBytes)
            << " (Logical: " << humanBytes(root->logicalBytes) << ")\n";
        if (root->isVolume() && root->volumeTotalBytes > 0) {
            out << " Volume Cap: " << humanBytes(root->volumeTotalBytes)
                << " | Free: " << humanBytes(root->volumeFreeBytes)
                << " | System/Unaccounted: " << humanBytes(root->unaccountedSystemBytes()) << "\n";
        }
        out << " Elements  : " << totalDirs << " directories, " << totalFiles << " files\n";
        out << "================================================================================\n";

        emitTreeLevel(root, "", 1, opts, rootTotal, out);
    }

    // 2. Top-N largest files / directories
    static void renderTop(const FsNode* root, const RenderOptions& opts, std::ostream& out = std::cout) {
        if (!root) return;
        std::vector<const FsNode*> items;
        gatherNodes(root, items, opts.filesOnly, opts.dirsOnly, true);

        std::sort(items.begin(), items.end(), [&](const FsNode* a, const FsNode* b) {
            return metricBytes(a, opts.useLogical) > metricBytes(b, opts.useLogical);
        });

        if (opts.minBytes > 0) {
            items.erase(std::remove_if(items.begin(), items.end(), [&](const FsNode* n) {
                return metricBytes(n, opts.useLogical) < opts.minBytes;
            }), items.end());
        }
        if (opts.topLimit > 0 && static_cast<int>(items.size()) > opts.topLimit) {
            items.resize(opts.topLimit);
        }

        int64_t rootTotal = (std::max<int64_t>)(1, metricBytes(root, opts.useLogical));
        if (opts.jsonOutput) {
            out << "{\n  \"root\": \"" << escapeJson(wideToUtf8(root->fullPath())) << "\",\n"
                << "  \"totalAllocatedBytes\": " << root->allocatedBytes << ",\n"
                << "  \"totalLogicalBytes\": " << root->logicalBytes << ",\n"
                << "  \"items\": [\n";
            for (size_t i = 0; i < items.size(); ++i) {
                const auto* n = items[i];
                double pct = static_cast<double>(metricBytes(n, opts.useLogical)) * 100.0 / static_cast<double>(rootTotal);
                out << "    {\"rank\": " << (i + 1)
                    << ", \"type\": \"" << (n->isFile() ? "file" : "directory") << "\""
                    << ", \"allocatedBytes\": " << n->allocatedBytes
                    << ", \"logicalBytes\": " << n->logicalBytes
                    << ", \"allocatedFormatted\": \"" << humanBytes(n->allocatedBytes) << "\""
                    << ", \"percent\": " << std::fixed << std::setprecision(2) << pct
                    << ", \"modified\": \"" << formatDateYMD(n->modifiedAt) << "\""
                    << ", \"path\": \"" << escapeJson(wideToUtf8(n->fullPath())) << "\"}"
                    << (i + 1 < items.size() ? "," : "") << "\n";
            }
            out << "  ]\n}\n";
            return;
        }

        out << "Rank  Type     Allocated    Pct      Modified     Path\n";
        out << "----  -------  -----------  -------  -----------  ----------------------------------------\n";
        for (size_t i = 0; i < items.size(); ++i) {
            const auto* n = items[i];
            int64_t sz = metricBytes(n, opts.useLogical);
            double pct = static_cast<double>(sz) * 100.0 / static_cast<double>(rootTotal);
            char line[128];
            std::snprintf(line, sizeof(line), "%4zu  %-7s  %11s  %6.2f%%  %-11s  ",
                i + 1,
                n->isFile() ? "[FILE]" : "[DIR]",
                humanBytes(sz).c_str(),
                pct,
                formatDateYMD(n->modifiedAt).c_str()
            );
            out << line << wideToUtf8(n->fullPath()) << "\n";
        }
    }

    // 3. Breakdown by File Categories & Extensions
    static void renderCategories(const FsNode* root, const QueryFilter& filter, const RenderOptions& opts, std::ostream& out = std::cout) {
        if (!root) return;
        struct StatBucket {
            uint64_t count = 0;
            int64_t allocatedBytes = 0;
            int64_t logicalBytes = 0;
        };
        std::map<std::string, StatBucket> catMap;
        std::map<std::string, StatBucket> extMap;

        auto traverse = [&](auto& self, const FsNode* n) -> void {
            if (n->isFile()) {
                std::string cat = filter.classifyFile(n->name);
                auto& cb = catMap[cat];
                cb.count++;
                cb.allocatedBytes += n->allocatedBytes;
                cb.logicalBytes += n->logicalBytes;

                std::string ext = "(no ext)";
                auto dot = n->name.find_last_of(L'.');
                if (dot != std::wstring::npos && dot + 1 < n->name.size()) {
                    ext = "." + asciiLower(wideToUtf8(n->name.substr(dot + 1)));
                }
                auto& eb = extMap[ext];
                eb.count++;
                eb.allocatedBytes += n->allocatedBytes;
                eb.logicalBytes += n->logicalBytes;
                return;
            }
            for (const auto& c : n->children) self(self, c.get());
        };
        traverse(traverse, root);

        std::vector<std::pair<std::string, StatBucket>> sortedCats(catMap.begin(), catMap.end());
        std::sort(sortedCats.begin(), sortedCats.end(), [](const auto& a, const auto& b) {
            return a.second.allocatedBytes > b.second.allocatedBytes;
        });

        std::vector<std::pair<std::string, StatBucket>> sortedExts(extMap.begin(), extMap.end());
        std::sort(sortedExts.begin(), sortedExts.end(), [](const auto& a, const auto& b) {
            return a.second.allocatedBytes > b.second.allocatedBytes;
        });
        if (opts.topLimit > 0 && static_cast<int>(sortedExts.size()) > opts.topLimit) {
            sortedExts.resize(opts.topLimit);
        }

        int64_t totalAlloc = (std::max<int64_t>)(1, root->allocatedBytes);
        if (opts.jsonOutput) {
            out << "{\n  \"categories\": [\n";
            for (size_t i = 0; i < sortedCats.size(); ++i) {
                const auto& [name, b] = sortedCats[i];
                double pct = static_cast<double>(b.allocatedBytes) * 100.0 / static_cast<double>(totalAlloc);
                out << "    {\"category\": \"" << escapeJson(name) << "\", \"files\": " << b.count
                    << ", \"allocatedBytes\": " << b.allocatedBytes << ", \"allocatedFormatted\": \"" << humanBytes(b.allocatedBytes)
                    << "\", \"percent\": " << std::fixed << std::setprecision(2) << pct << "}"
                    << (i + 1 < sortedCats.size() ? "," : "") << "\n";
            }
            out << "  ],\n  \"topExtensions\": [\n";
            for (size_t i = 0; i < sortedExts.size(); ++i) {
                const auto& [ext, b] = sortedExts[i];
                double pct = static_cast<double>(b.allocatedBytes) * 100.0 / static_cast<double>(totalAlloc);
                out << "    {\"ext\": \"" << escapeJson(ext) << "\", \"files\": " << b.count
                    << ", \"allocatedBytes\": " << b.allocatedBytes << ", \"allocatedFormatted\": \"" << humanBytes(b.allocatedBytes)
                    << "\", \"percent\": " << std::fixed << std::setprecision(2) << pct << "}"
                    << (i + 1 < sortedExts.size() ? "," : "") << "\n";
            }
            out << "  ]\n}\n";
            return;
        }

        out << "=== AllocSight File Categories ===\n";
        for (const auto& [name, b] : sortedCats) {
            double pct = static_cast<double>(b.allocatedBytes) * 100.0 / static_cast<double>(totalAlloc);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "  %-26s  %11s  (%6.2f%%)  [%llu files]\n",
                name.c_str(), humanBytes(b.allocatedBytes).c_str(), pct, static_cast<unsigned long long>(b.count));
            out << buf;
        }
        out << "\n=== Top Extensions by Disk Allocation ===\n";
        for (const auto& [ext, b] : sortedExts) {
            double pct = static_cast<double>(b.allocatedBytes) * 100.0 / static_cast<double>(totalAlloc);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "  %-16s  %11s  (%6.2f%%)  [%llu files]\n",
                ext.c_str(), humanBytes(b.allocatedBytes).c_str(), pct, static_cast<unsigned long long>(b.count));
            out << buf;
        }
    }

    // Collect cleanup diagnostic candidates across the tree
    static std::vector<CleanupFinding> collectDiagnosticCandidates(const FsNode* root) {
        std::vector<CleanupFinding> findings;
        if (!root) return findings;

        FILETIME nowFt = nowFileTime();
        uint64_t nowTicks = fileTimeToTicks(nowFt);
        constexpr uint64_t kDayTicks = 86400ULL * 10000000ULL;

        auto stripArchiveExt = [](const std::string& fname) -> std::string {
            static const char* kExts[] = {".tar.gz", ".tar.xz", ".tar.bz2", ".zip", ".7z", ".rar", ".tar", ".tgz"};
            for (const char* ext : kExts) {
                size_t elen = std::strlen(ext);
                if (fname.size() > elen && fname.compare(fname.size() - elen, elen, ext) == 0) {
                    return fname.substr(0, fname.size() - elen);
                }
            }
            return "";
        };

        auto inspectNode = [&](auto& self, const FsNode* n) -> void {
            std::string segUtf8 = wideToUtf8(n->name);
            std::string segLow = asciiLower(segUtf8);

            // Build set of sibling directory names when inspecting a container
            std::unordered_set<std::string> siblingDirNames;
            if (n->isContainer()) {
                for (const auto& ch : n->children) {
                    if (ch->isDirectory()) {
                        siblingDirNames.insert(asciiLower(wideToUtf8(ch->name)));
                    }
                }
            }

            if (n->isDirectory()) {
                if (segLow == "$recycle.bin" || segLow == "recycler") {
                    if (n->allocatedBytes > 0) {
                        findings.push_back({"recycle_bin", "SAFE", "Windows Recycle Bin contents", n});
                    }
                    return;
                }
                if (segLow == "temp" || segLow == "tmp" || segLow == "_tmp" ||
                    segLow == "cache" || segLow == "caches" || segLow == "dxcache" ||
                    segLow == "ota-artifacts" || segLow == "squirreltemp" ||
                    segLow == "_cacache" || segLow == "node_cache" || segLow == "pnpm-cache" ||
                    segLow == "cachedextensionvsixs" || segLow == "crashpad" || segLow == "crashdumps" ||
                    segLow == "shadercache" || segLow == "gpucache" || segLow == "code cache" ||
                    segLow == "htmlcache" || segLow == ".bu-chrome-profile") {
                    if (n->allocatedBytes >= 10LL * 1024 * 1024) {
                        findings.push_back({"safe_temp_cache", "SAFE", "Cache / temporary / crash-dump directory (>= 10 MB)", n});
                    }
                    return;
                }
                if (segLow.rfind("codex-runtime-install-", 0) == 0 ||
                    segLow.rfind("edge-profile", 0) == 0) {
                    if (n->allocatedBytes >= 20LL * 1024 * 1024) {
                        findings.push_back({"orphan_staging", "SAFE", "Orphaned runtime staging or automation browser profile", n});
                    }
                    return;
                }
                if (segUtf8.find(" - 副本") != std::string::npos ||
                    segLow.find(" - copy") != std::string::npos ||
                    segLow == "old_backup") {
                    if (n->allocatedBytes >= 50LL * 1024 * 1024) {
                        findings.push_back({"duplicate_copy_folder", "REVIEW", "Duplicate backup / copy folder (>= 50 MB)", n});
                        return;
                    }
                }
                if (segLow == "node_modules" || segLow == ".venv" || segLow == "venv" ||
                    segLow == "__pycache__" || segLow == "target") {
                    if (n->allocatedBytes >= 50LL * 1024 * 1024) {
                        findings.push_back({"dev_artifacts", "REVIEW", "Rebuildable developer dependency or build directory (>= 50 MB)", n});
                        return;
                    }
                }
            }

            // Inspect child files (including sibling-extracted archive detection)
            for (const auto& c : n->children) {
                if (c->isFile()) {
                    if (c->allocatedBytes < 10LL * 1024 * 1024) continue;
                    std::string fLow = asciiLower(wideToUtf8(c->name));

                    if (QueryFilter::globMatch(fLow.c_str(), "*.log") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.dmp") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.tmp") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.bak") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.old") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.crdownload") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.ushaderprecache")) {
                        findings.push_back({"logs_and_temp_files", "SAFE", "Large log, crash dump, incomplete download, or shader cache file", c.get()});
                        continue;
                    }

                    std::string stem = stripArchiveExt(fLow);
                    if (!stem.empty() && c->allocatedBytes >= 50LL * 1024 * 1024 && siblingDirNames.count(stem) > 0) {
                        findings.push_back({"already_extracted_archive", "REVIEW", "Archive already extracted to sibling folder '" + stem + "'", c.get()});
                        continue;
                    }

                    if (c->allocatedBytes >= 100LL * 1024 * 1024 && (
                        QueryFilter::globMatch(fLow.c_str(), "*.iso") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.zip") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.7z") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.rar") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.tar") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.tar.gz") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.whl") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.conda") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.msi") ||
                        QueryFilter::globMatch(fLow.c_str(), "*.pdb") ||
                        QueryFilter::globMatch(fLow.c_str(), "*setup*.exe") ||
                        QueryFilter::globMatch(fLow.c_str(), "*install*.exe"))) {
                        findings.push_back({"large_archives_installers", "REVIEW", "Large archive, wheel package, debug symbol, or installer (>= 100 MB)", c.get()});
                        continue;
                    }

                    if (c->allocatedBytes >= 250LL * 1024 * 1024) {
                        uint64_t modTicks = fileTimeToTicks(c->modifiedAt);
                        uint64_t ageDays = (nowTicks > modTicks && modTicks > 0) ? ((nowTicks - modTicks) / kDayTicks) : 0;
                        if (ageDays >= 180) {
                            findings.push_back({"stale_large_files", "REVIEW", "Large file (>= 250 MB) unmodified for > 6 months", c.get()});
                        }
                    }
                } else {
                    self(self, c.get());
                }
            }
        };

        inspectNode(inspectNode, root);
        std::sort(findings.begin(), findings.end(), [](const CleanupFinding& a, const CleanupFinding& b) {
            return a.node->allocatedBytes > b.node->allocatedBytes;
        });
        return findings;
    }

    // 4. Smart Space Cleanup Diagnostic Analyzer
    static void renderAnalyze(const FsNode* root, const RenderOptions& opts, std::ostream& out = std::cout) {
        if (!root) return;
        auto findings = collectDiagnosticCandidates(root);

        int64_t safeSum = 0;
        int64_t reviewSum = 0;
        for (const auto& f : findings) {
            if (f.tier == "SAFE") safeSum += f.node->allocatedBytes;
            else reviewSum += f.node->allocatedBytes;
        }

        if (opts.jsonOutput) {
            out << "{\n  \"root\": \"" << escapeJson(wideToUtf8(root->fullPath())) << "\",\n"
                << "  \"totalAllocatedBytes\": " << root->allocatedBytes << ",\n"
                << "  \"safeReclaimableBytes\": " << safeSum << ",\n"
                << "  \"safeReclaimableFormatted\": \"" << humanBytes(safeSum) << "\",\n"
                << "  \"reviewReclaimableBytes\": " << reviewSum << ",\n"
                << "  \"reviewReclaimableFormatted\": \"" << humanBytes(reviewSum) << "\",\n"
                << "  \"candidates\": [\n";
            size_t limit = (opts.topLimit > 0 && static_cast<int>(findings.size()) > opts.topLimit)
                ? static_cast<size_t>(opts.topLimit) : findings.size();
            for (size_t i = 0; i < limit; ++i) {
                const auto& f = findings[i];
                std::wstring fullP = f.node->fullPath();
                out << "    {\"tier\": \"" << f.tier << "\", \"category\": \"" << f.category
                    << "\", \"reason\": \"" << escapeJson(f.rationale)
                    << "\", \"type\": \"" << (f.node->isFile() ? "file" : "directory")
                    << "\", \"allocatedBytes\": " << f.node->allocatedBytes
                    << ", \"allocatedFormatted\": \"" << humanBytes(f.node->allocatedBytes)
                    << "\", \"modified\": \"" << formatDateYMD(f.node->modifiedAt)
                    << "\", \"path\": \"" << escapeJson(wideToUtf8(fullP))
                    << "\", \"fileUri\": \"" << escapeJson(pathToFileUri(fullP)) << "\"}"
                    << (i + 1 < limit ? "," : "") << "\n";
            }
            out << "  ]\n}\n";
            return;
        }

        out << "================================================================================\n";
        out << " AllocSight - Smart Space Cleanup Diagnostic\n";
        out << " Scanned Root            : " << wideToUtf8(root->fullPath()) << " [" << humanBytes(root->allocatedBytes) << "]\n";
        out << " Safe-to-Clean Estimate  : " << humanBytes(safeSum) << "\n";
        out << " Review-Needed Estimate  : " << humanBytes(reviewSum) << "\n";
        out << "================================================================================\n";
        if (findings.empty()) {
            out << " No cleanup candidates exceeding thresholds were found in this path.\n";
            return;
        }
        size_t limit = (opts.topLimit > 0 && static_cast<int>(findings.size()) > opts.topLimit)
            ? static_cast<size_t>(opts.topLimit) : findings.size();
        for (size_t i = 0; i < limit; ++i) {
            const auto& f = findings[i];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[%-6s] %11s  %-26s  ",
                f.tier.c_str(), humanBytes(f.node->allocatedBytes).c_str(), f.category.c_str());
            out << buf << wideToUtf8(f.node->fullPath()) << "\n"
                << "         Reason: " << f.rationale << " (Modified: " << formatDateYMD(f.node->modifiedAt) << ")\n";
        }
    }

    // 5. Generate Clickable Markdown Cleanup Report (`allocsight report <path> -o report.md`)
    static void renderMarkdownReport(const FsNode* root, const RenderOptions& opts, std::ostream& out = std::cout) {
        if (!root) return;
        auto findings = collectDiagnosticCandidates(root);

        int64_t safeSum = 0;
        int64_t reviewSum = 0;
        std::vector<CleanupFinding> safeList, reviewList;
        for (const auto& f : findings) {
            if (f.tier == "SAFE") {
                safeSum += f.node->allocatedBytes;
                safeList.push_back(f);
            } else {
                reviewSum += f.node->allocatedBytes;
                reviewList.push_back(f);
            }
        }

        std::wstring rootP = root->fullPath();
        out << "# AllocSight Disk Allocation & Space Reclamation Report\n\n";
        out << "- **Target Path**: [" << wideToUtf8(rootP) << "](" << pathToFileUri(rootP) << ")\n";
        out << "- **Allocated Space**: `" << humanBytes(root->allocatedBytes) << "` (Logical Size: `" << humanBytes(root->logicalBytes) << "`)\n";
        if (root->isVolume() && root->volumeTotalBytes > 0) {
            out << "- **Volume Capacity**: `" << humanBytes(root->volumeTotalBytes) << "` | **Free Space**: `" << humanBytes(root->volumeFreeBytes) << "`\n";
        }
        out << "- **Safe-to-Reclaim Estimate (`[SAFE]`)**: **`" << humanBytes(safeSum) << "`**\n";
        out << "- **Review-Required Estimate (`[REVIEW]`)**: **`" << humanBytes(reviewSum) << "`**\n\n";

        auto emitTable = [&](const std::string& title, const std::vector<CleanupFinding>& list) {
            out << "## " << title << "\n\n";
            if (list.empty()) {
                out << "_No items exceeding diagnostic thresholds were detected._\n\n";
                return;
            }
            out << "| # | Path (Direct File URI) | Allocated | Category | Diagnostic Rationale & Last Modified |\n";
            out << "| :---: | :--- | :---: | :---: | :--- |\n";
            size_t lim = (opts.topLimit > 0 && static_cast<int>(list.size()) > opts.topLimit)
                ? static_cast<size_t>(opts.topLimit) : list.size();
            for (size_t i = 0; i < lim; ++i) {
                const auto& f = list[i];
                std::wstring fullP = f.node->fullPath();
                std::wstring parentP = f.node->parentPath();
                out << "| " << (i + 1) << " | ";
                if (f.node->isDirectory()) {
                    out << "[`[DIR]` " << wideToUtf8(fullP) << "](" << pathToFileUri(fullP) << ")";
                    if (!parentP.empty()) {
                        out << "<br>Parent: [" << wideToUtf8(parentP) << "](" << pathToFileUri(parentP) << ")";
                    }
                } else {
                    if (!parentP.empty()) {
                        out << "Dir: [" << wideToUtf8(parentP) << "](" << pathToFileUri(parentP) << ")<br>";
                    }
                    out << "[`[FILE]` " << wideToUtf8(f.node->name) << "](" << pathToFileUri(fullP) << ")";
                }
                out << " | **" << humanBytes(f.node->allocatedBytes) << "** | `" << f.category
                    << "` | " << f.rationale << " (`" << formatDateYMD(f.node->modifiedAt) << "`) |\n";
            }
            out << "\n";
        };

        emitTable("1. Safe-to-Clean Candidates (Caches, Temp Directories, Logs & Crash Dumps)", safeList);
        emitTable("2. Review-Required Candidates (Extracted Archives, Duplicate Folders, Build Artifacts & Stale Files)", reviewList);
    }

private:
    static void gatherNodes(
        const FsNode* node,
        std::vector<const FsNode*>& out,
        bool filesOnly,
        bool dirsOnly,
        bool isRoot
    ) {
        if (!isRoot) {
            if (filesOnly && node->isFile()) out.push_back(node);
            else if (dirsOnly && node->isDirectory()) out.push_back(node);
            else if (!filesOnly && !dirsOnly) out.push_back(node);
        }
        for (const auto& c : node->children) {
            gatherNodes(c.get(), out, filesOnly, dirsOnly, false);
        }
    }

    static void emitTreeLevel(
        const FsNode* parent,
        const std::string& prefix,
        int curDepth,
        const RenderOptions& opts,
        int64_t rootTotal,
        std::ostream& out
    ) {
        if (opts.maxDepth > 0 && curDepth > opts.maxDepth) return;

        std::vector<const FsNode*> sorted;
        for (const auto& c : parent->children) {
            if (opts.filesOnly && !c->isFile() && !c->isContainer()) continue;
            if (opts.dirsOnly && !c->isContainer()) continue;
            if (metricBytes(c.get(), opts.useLogical) < opts.minBytes) continue;
            sorted.push_back(c.get());
        }

        std::sort(sorted.begin(), sorted.end(), [&](const FsNode* a, const FsNode* b) {
            return metricBytes(a, opts.useLogical) > metricBytes(b, opts.useLogical);
        });

        size_t totalCount = sorted.size();
        size_t limit = (opts.topLimit > 0 && static_cast<int>(totalCount) > opts.topLimit)
            ? static_cast<size_t>(opts.topLimit) : totalCount;

        for (size_t i = 0; i < limit; ++i) {
            const auto* child = sorted[i];
            bool isLast = (i + 1 == limit) && (limit == totalCount);
            int64_t sz = metricBytes(child, opts.useLogical);
            double pct = (rootTotal > 0) ? (static_cast<double>(sz) * 100.0 / static_cast<double>(rootTotal)) : 0.0;

            out << prefix << (isLast ? "`-- " : "|-- ")
                << "[" << std::setw(10) << humanBytes(sz) << " | "
                << std::fixed << std::setprecision(1) << std::setw(5) << pct << "% "
                << progressBar(pct, 12) << "] "
                << (child->isContainer() ? "[DIR] " : "")
                << wideToUtf8(child->name) << "\n";

            if (child->isContainer() && (opts.maxDepth < 0 || curDepth < opts.maxDepth)) {
                emitTreeLevel(child, prefix + (isLast ? "    " : "|   "), curDepth + 1, opts, rootTotal, out);
            }
        }
        if (limit < totalCount) {
            out << prefix << "`-- ... (" << (totalCount - limit) << " smaller items omitted)\n";
        }
    }

    static void emitNodeJson(
        const FsNode* n,
        int curDepth,
        int maxDepth,
        int topLimit,
        int64_t minBytes,
        bool useLogical,
        int64_t rootTotal,
        int indent,
        std::ostream& out
    ) {
        std::string pad(indent, ' ');
        int64_t sz = metricBytes(n, useLogical);
        double pct = (rootTotal > 0) ? (static_cast<double>(sz) * 100.0 / static_cast<double>(rootTotal)) : 0.0;
        std::wstring fullP = n->fullPath();
        out << pad << "\"name\": \"" << escapeJson(wideToUtf8(n->name)) << "\",\n"
            << pad << "\"path\": \"" << escapeJson(wideToUtf8(fullP)) << "\",\n"
            << pad << "\"fileUri\": \"" << escapeJson(pathToFileUri(fullP)) << "\",\n"
            << pad << "\"type\": \"" << (n->isVolume() ? "volume" : (n->isDirectory() ? "directory" : "file")) << "\",\n"
            << pad << "\"allocatedBytes\": " << n->allocatedBytes << ",\n"
            << pad << "\"logicalBytes\": " << n->logicalBytes << ",\n"
            << pad << "\"allocatedFormatted\": \"" << humanBytes(n->allocatedBytes) << "\",\n"
            << pad << "\"percent\": " << std::fixed << std::setprecision(2) << pct;

        if (n->isContainer() && (maxDepth < 0 || curDepth < maxDepth) && !n->children.empty()) {
            std::vector<const FsNode*> sorted;
            for (const auto& c : n->children) {
                if (metricBytes(c.get(), useLogical) >= minBytes) {
                    sorted.push_back(c.get());
                }
            }
            std::sort(sorted.begin(), sorted.end(), [&](const FsNode* a, const FsNode* b) {
                return metricBytes(a, useLogical) > metricBytes(b, useLogical);
            });
            if (topLimit > 0 && static_cast<int>(sorted.size()) > topLimit) sorted.resize(topLimit);

            if (!sorted.empty()) {
                out << ",\n" << pad << "\"children\": [\n";
                for (size_t i = 0; i < sorted.size(); ++i) {
                    out << pad << "  {\n";
                    emitNodeJson(sorted[i], curDepth + 1, maxDepth, topLimit, minBytes, useLogical, rootTotal, indent + 4, out);
                    out << "\n" << pad << "  }" << (i + 1 < sorted.size() ? "," : "") << "\n";
                }
                out << pad << "]";
            }
        }
    }
};

} // namespace allocsight
