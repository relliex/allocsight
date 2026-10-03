// SPDX-License-Identifier: MIT
// AllocSight - High-Concurrency Disk Allocation & AI Space Cleanup Engine
// Copyright (c) 2026 AllocSight Contributors

#pragma once
#include "alloc_types.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <deque>
#include <unordered_set>
#include <condition_variable>
#include <chrono>
#include <io.h>

#ifndef FILE_ATTRIBUTE_RECALL_ON_OPEN
#define FILE_ATTRIBUTE_RECALL_ON_OPEN 0x00040000UL
#endif
#ifndef FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
#define FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS 0x00400000UL
#endif
#ifndef IsReparseTagNameSurrogate
#define IsReparseTagNameSurrogate(_tag) (((_tag) & 0x20000000UL) != 0)
#endif

namespace allocsight {

// Global cancellation flag for graceful Ctrl+C handling
inline std::atomic<bool> g_cancelRequested{false};

inline BOOL WINAPI ConsoleCtrlHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT) {
        g_cancelRequested.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

struct AltStreamEntry {
    std::wstring streamName;
    int64_t byteLength = 0;
};

// Enumerates NTFS Alternate Data Streams using documented Win32 FindFirstStreamW / FindNextStreamW API
class NtfsStreamInspector {
public:
    static std::vector<AltStreamEntry> enumerateStreams(const std::wstring& path, uint32_t attrs) {
        std::vector<AltStreamEntry> out;
        if ((attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0) {
            return out;
        }

        std::wstring longPath = (path.rfind(L"\\\\?\\", 0) == 0) ? path : (L"\\\\?\\" + path);
        WIN32_FIND_STREAM_DATA streamData{};
        HANDLE hFind = FindFirstStreamW(longPath.c_str(), FindStreamInfoStandard, &streamData, 0);
        if (hFind == INVALID_HANDLE_VALUE) {
            return out;
        }

        do {
            std::wstring sname(streamData.cStreamName);
            // Skip the primary default file stream ("::$DATA") and directory index streams
            if (!sname.empty() && sname != L"::$DATA" && sname.rfind(L"::$", 0) != 0) {
                out.push_back(AltStreamEntry{sname, streamData.StreamSize.QuadPart});
            }
        } while (FindNextStreamW(hFind, &streamData));

        FindClose(hFind);
        return out;
    }
};

struct ScanConfig {
    bool includeAltStreams = false;
    bool detectHardLinks   = false;
    int  workerThreads     = 0; // 0 = auto-detect hardware concurrency (up to 16)
    int  maxScanDepth      = -1;
    bool quietMode         = false;
};

struct ScanTelemetry {
    std::atomic<uint64_t> filesCount{0};
    std::atomic<uint64_t> dirsCount{0};
    std::atomic<uint64_t> altStreamsCount{0};
    std::atomic<uint64_t> hardLinksCount{0};
    std::atomic<int64_t>  hardLinksBytesSaved{0};
    std::atomic<uint64_t> accessErrors{0};
    std::atomic<int64_t>  logicalTotal{0};
    std::atomic<int64_t>  allocatedTotal{0};
    std::atomic<bool>     wasCancelled{false};
    bool backupPrivilegeEnabled = false;
    int  threadsUsed = 1;
};

class ParallelScanner {
public:
    static int defaultThreadCount() {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) return 12;
        return static_cast<int>((std::min)(16u, (std::max)(8u, hw)));
    }

    static std::wstring normalizePath(std::wstring p) {
        for (wchar_t& c : p) {
            if (c == L'/') c = L'\\';
        }
        while (p.size() > 1 && p.back() == L'\\') {
            p.pop_back();
        }
        return p;
    }

    static inline bool needsPhysicalQuery(uint32_t attrs) {
        return (attrs & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED)) != 0;
    }

    static inline int64_t computeAllocatedFast(
        uint32_t attrs,
        int64_t logicalSize,
        const VolumeMetrics& vol
    ) {
        if ((attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0) {
            return 0;
        }
        return vol.alignToCluster(logicalSize);
    }

    static int64_t computeAllocatedBytes(
        const std::wstring& fullPath,
        uint32_t attrs,
        int64_t logicalSize,
        const VolumeMetrics& vol
    ) {
        // Cloud-only unpinned placeholders occupy 0 local disk clusters
        if ((attrs & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) != 0) {
            return 0;
        }
        int64_t rawBytes = logicalSize;
        if ((attrs & (FILE_ATTRIBUTE_SPARSE_FILE | FILE_ATTRIBUTE_COMPRESSED)) != 0) {
            DWORD high = 0;
            std::wstring longPath = (fullPath.rfind(L"\\\\?\\", 0) == 0) ? fullPath : (L"\\\\?\\" + fullPath);
            DWORD low = GetCompressedFileSizeW(longPath.c_str(), &high);
            if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
                rawBytes = (static_cast<int64_t>(high) << 32) | static_cast<int64_t>(low);
            }
        }
        return vol.alignToCluster(rawBytes);
    }

    // Prevent directory cycles caused by symbolic links or volume mount junctions
    // using standard <winnt.h> reparse tag classification macros
    static bool isSafeDirectoryTraversal(const WIN32_FIND_DATAW& fd) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            uint32_t tag = fd.dwReserved0;
            if (IsReparseTagNameSurrogate(tag) ||
                tag == IO_REPARSE_TAG_SYMLINK ||
                tag == IO_REPARSE_TAG_MOUNT_POINT) {
                return false;
            }
        }
        return true;
    }

    static std::unique_ptr<FsNode> scan(
        const std::wstring& targetPath,
        const ScanConfig& cfg,
        ScanTelemetry& telemetry,
        VolumeMetrics* outVol = nullptr
    ) {
        telemetry.backupPrivilegeEnabled = tryAcquireBackupPrivilege();
        std::wstring norm = normalizePath(targetPath);

        wchar_t resolved[4096];
        std::wstring query = (norm.size() == 2 && norm[1] == L':') ? (norm + L"\\") : norm;
        if (GetFullPathNameW(query.c_str(), 4096, resolved, nullptr) > 0) {
            norm = normalizePath(resolved);
        }

        VolumeMetrics vol(norm);
        if (outVol) *outVol = vol;

        bool checkStreams = cfg.includeAltStreams && vol.supportsStreams;

        auto root = std::make_unique<FsNode>();
        root->kind = vol.isVolumeRoot ? EntryKind::Volume : EntryKind::Directory;
        root->name = norm;
        root->volumeTotalBytes = vol.totalCapacity;
        root->volumeFreeBytes  = vol.freeBytes;

        FILETIME nowFt = nowFileTime();
        root->createdAt  = nowFt;
        root->accessedAt = nowFt;
        root->modifiedAt = nowFt;

        struct DirTask {
            FsNode* dirNode;
            std::wstring dirPath;
            int depth;
        };

        std::mutex mtx;
        std::condition_variable cv;
        std::queue<DirTask> taskQueue;
        int busyWorkers = 0;
        bool finished = false;

        std::mutex hardLinkMtx;
        std::unordered_set<uint64_t> seenFileIds;

        taskQueue.push(DirTask{root.get(), norm, 0});
        telemetry.dirsCount.fetch_add(1, std::memory_order_relaxed);

        int nThreads = (cfg.workerThreads > 0) ? cfg.workerThreads : defaultThreadCount();
        telemetry.threadsUsed = nThreads;
        std::vector<std::thread> pool;
        pool.reserve(nThreads);

        std::atomic<bool> progressDone{false};
        std::thread ticker;
        bool isInteractive = (!cfg.quietMode && _isatty(_fileno(stderr)) != 0);
        if (isInteractive) {
            HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
            DWORD mode = 0;
            if (GetConsoleMode(hErr, &mode)) {
                SetConsoleMode(hErr, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
            }
            ticker = std::thread([&]() {
                auto t0 = std::chrono::steady_clock::now();
                while (!progressDone.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                    if (progressDone.load(std::memory_order_relaxed)) break;
                    auto now = std::chrono::steady_clock::now();
                    double sec = std::chrono::duration<double>(now - t0).count();
                    uint64_t fc = telemetry.filesCount.load(std::memory_order_relaxed);
                    uint64_t dc = telemetry.dirsCount.load(std::memory_order_relaxed);
                    int64_t allocTot = telemetry.allocatedTotal.load(std::memory_order_relaxed);
                    std::fprintf(stderr, "\r\x1b[2K[Scanning: %llu files, %llu dirs | %s | %.1fs | %d threads] ",
                                 static_cast<unsigned long long>(fc),
                                 static_cast<unsigned long long>(dc),
                                 humanBytes(allocTot).c_str(),
                                 sec,
                                 telemetry.threadsUsed);
                    std::fflush(stderr);
                }
                std::fprintf(stderr, "\r\x1b[2K");
                std::fflush(stderr);
            });
        }

        auto workerLoop = [&]() {
            std::vector<DirTask> localTasks;

            while (true) {
                if (g_cancelRequested.load(std::memory_order_relaxed)) {
                    telemetry.wasCancelled.store(true, std::memory_order_relaxed);
                    std::unique_lock<std::mutex> lk(mtx);
                    finished = true;
                    cv.notify_all();
                    return;
                }

                DirTask task{};
                if (!localTasks.empty()) {
                    task = std::move(localTasks.back());
                    localTasks.pop_back();
                } else {
                    std::unique_lock<std::mutex> lk(mtx);
                    cv.wait(lk, [&]() {
                        return !taskQueue.empty() || finished || g_cancelRequested.load(std::memory_order_relaxed);
                    });
                    if (g_cancelRequested.load(std::memory_order_relaxed)) {
                        telemetry.wasCancelled.store(true, std::memory_order_relaxed);
                        finished = true;
                        cv.notify_all();
                        return;
                    }
                    if (taskQueue.empty() && finished) return;

                    // Batch dequeue up to 4 tasks from global queue to minimize lock contention
                    size_t batch = (std::min)(size_t(4), taskQueue.size());
                    for (size_t b = 0; b < batch; ++b) {
                        localTasks.push_back(std::move(taskQueue.front()));
                        taskQueue.pop();
                    }
                    ++busyWorkers;
                    task = std::move(localTasks.back());
                    localTasks.pop_back();
                }

                std::wstring pattern = (task.dirPath.rfind(L"\\\\?\\", 0) == 0)
                    ? (task.dirPath + L"\\*")
                    : (L"\\\\?\\" + task.dirPath + L"\\*");

                WIN32_FIND_DATAW fd{};
                HANDLE hFind = FindFirstFileExW(
                    pattern.c_str(),
                    FindExInfoBasic,
                    &fd,
                    FindExSearchNameMatch,
                    nullptr,
                    FIND_FIRST_EX_LARGE_FETCH
                );

                if (hFind == INVALID_HANDLE_VALUE) {
                    std::wstring fallback = task.dirPath + L"\\*";
                    hFind = FindFirstFileExW(
                        fallback.c_str(),
                        FindExInfoBasic,
                        &fd,
                        FindExSearchNameMatch,
                        nullptr,
                        FIND_FIRST_EX_LARGE_FETCH
                    );
                }

                std::vector<std::unique_ptr<FsNode>> discoveredNodes;
                std::vector<DirTask> childTasks;

                if (hFind == INVALID_HANDLE_VALUE) {
                    telemetry.accessErrors.fetch_add(1, std::memory_order_relaxed);
                } else {
                    do {
                        const wchar_t* entryName = fd.cFileName;
                        if (entryName[0] == L'.' && (entryName[1] == L'\0' || (entryName[1] == L'.' && entryName[2] == L'\0'))) {
                            continue;
                        }
                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && !isSafeDirectoryTraversal(fd)) {
                            continue;
                        }

                        std::wstring segName(entryName);
                        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                        bool needsPath = isDir || checkStreams || cfg.detectHardLinks || needsPhysicalQuery(fd.dwFileAttributes);
                        std::wstring childFullPath;
                        if (needsPath) {
                            childFullPath = task.dirPath + L"\\" + segName;
                        }

                        if (isDir) {
                            auto dirNode = std::make_unique<FsNode>();
                            dirNode->kind = EntryKind::Directory;
                            dirNode->name = segName;
                            dirNode->winAttrs = fd.dwFileAttributes;
                            dirNode->createdAt = fd.ftCreationTime;
                            dirNode->accessedAt = fd.ftLastAccessTime;
                            dirNode->modifiedAt = fd.ftLastWriteTime;

                            if (checkStreams) {
                                for (const auto& s : NtfsStreamInspector::enumerateStreams(childFullPath, fd.dwFileAttributes)) {
                                    auto sNode = std::make_unique<FsNode>();
                                    sNode->kind = EntryKind::RegularFile;
                                    sNode->name = segName + s.streamName;
                                    sNode->logicalBytes = s.byteLength;
                                    sNode->allocatedBytes = vol.alignToCluster(s.byteLength);
                                    sNode->winAttrs = fd.dwFileAttributes;
                                    sNode->createdAt = fd.ftCreationTime;
                                    sNode->accessedAt = fd.ftLastAccessTime;
                                    sNode->modifiedAt = fd.ftLastWriteTime;
                                    sNode->isAltStream = true;
                                    dirNode->appendChild(std::move(sNode));
                                    telemetry.altStreamsCount.fetch_add(1, std::memory_order_relaxed);
                                }
                            }

                            FsNode* rawDir = dirNode.get();
                            discoveredNodes.push_back(std::move(dirNode));
                            telemetry.dirsCount.fetch_add(1, std::memory_order_relaxed);

                            if (cfg.maxScanDepth < 0 || task.depth < cfg.maxScanDepth) {
                                childTasks.push_back(DirTask{rawDir, std::move(childFullPath), task.depth + 1});
                            }
                        } else {
                            int64_t logSz = (static_cast<int64_t>(fd.nFileSizeHigh) << 32) | static_cast<int64_t>(fd.nFileSizeLow);
                            int64_t allocSz = needsPhysicalQuery(fd.dwFileAttributes)
                                ? computeAllocatedBytes(childFullPath, fd.dwFileAttributes, logSz, vol)
                                : computeAllocatedFast(fd.dwFileAttributes, logSz, vol);

                            auto fileNode = std::make_unique<FsNode>();
                            fileNode->kind = EntryKind::RegularFile;
                            fileNode->name = segName;
                            fileNode->logicalBytes = logSz;
                            fileNode->allocatedBytes = allocSz;
                            fileNode->winAttrs = fd.dwFileAttributes;
                            fileNode->createdAt = fd.ftCreationTime;
                            fileNode->accessedAt = fd.ftLastAccessTime;
                            fileNode->modifiedAt = fd.ftLastWriteTime;

                            if (cfg.detectHardLinks && !childFullPath.empty()) {
                                std::wstring longP = (childFullPath.rfind(L"\\\\?\\", 0) == 0) ? childFullPath : (L"\\\\?\\" + childFullPath);
                                HANDLE hF = CreateFileW(
                                    longP.c_str(),
                                    FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr
                                );
                                if (hF != INVALID_HANDLE_VALUE) {
                                    BY_HANDLE_FILE_INFORMATION bhfi{};
                                    if (GetFileInformationByHandle(hF, &bhfi) && bhfi.nNumberOfLinks > 1) {
                                        uint64_t fid = (static_cast<uint64_t>(bhfi.nFileIndexHigh) << 32) | bhfi.nFileIndexLow;
                                        bool isDup = false;
                                        {
                                            std::lock_guard<std::mutex> lk(hardLinkMtx);
                                            isDup = !seenFileIds.insert(fid).second;
                                        }
                                        if (isDup) {
                                            fileNode->isHardLink = true;
                                            telemetry.hardLinksCount.fetch_add(1, std::memory_order_relaxed);
                                            telemetry.hardLinksBytesSaved.fetch_add(allocSz, std::memory_order_relaxed);
                                        }
                                    }
                                    CloseHandle(hF);
                                }
                            }

                            if (checkStreams) {
                                for (const auto& s : NtfsStreamInspector::enumerateStreams(childFullPath, fd.dwFileAttributes)) {
                                    auto sNode = std::make_unique<FsNode>();
                                    sNode->kind = EntryKind::RegularFile;
                                    sNode->name = segName + s.streamName;
                                    sNode->logicalBytes = s.byteLength;
                                    sNode->allocatedBytes = vol.alignToCluster(s.byteLength);
                                    sNode->winAttrs = fd.dwFileAttributes;
                                    sNode->createdAt = fd.ftCreationTime;
                                    sNode->accessedAt = fd.ftLastAccessTime;
                                    sNode->modifiedAt = fd.ftLastWriteTime;
                                    sNode->isAltStream = true;
                                    discoveredNodes.push_back(std::move(sNode));
                                    telemetry.altStreamsCount.fetch_add(1, std::memory_order_relaxed);
                                }
                            }

                            discoveredNodes.push_back(std::move(fileNode));
                            telemetry.filesCount.fetch_add(1, std::memory_order_relaxed);
                            telemetry.logicalTotal.fetch_add(logSz, std::memory_order_relaxed);
                            telemetry.allocatedTotal.fetch_add(allocSz, std::memory_order_relaxed);
                        }
                    } while (FindNextFileW(hFind, &fd));
                    FindClose(hFind);
                }

                for (auto& child : discoveredNodes) {
                    task.dirNode->appendChild(std::move(child));
                }

                // Hybrid work distribution: keep up to 2 tasks in localTasks (LIFO), push excess to global queue
                if (!childTasks.empty()) {
                    size_t keep = (std::min)(size_t(2), childTasks.size());
                    for (size_t k = 0; k < keep; ++k) {
                        localTasks.push_back(std::move(childTasks.back()));
                        childTasks.pop_back();
                    }
                }

                if (!childTasks.empty()) {
                    std::unique_lock<std::mutex> lk(mtx);
                    for (auto& ct : childTasks) {
                        taskQueue.push(std::move(ct));
                    }
                    cv.notify_all();
                }

                if (localTasks.empty()) {
                    std::unique_lock<std::mutex> lk(mtx);
                    --busyWorkers;
                    if (taskQueue.empty() && busyWorkers == 0) {
                        finished = true;
                        cv.notify_all();
                    }
                }
            }
        };

        for (int i = 0; i < nThreads; ++i) {
            pool.emplace_back(workerLoop);
        }
        for (auto& t : pool) {
            t.join();
        }

        if (isInteractive && ticker.joinable()) {
            progressDone.store(true, std::memory_order_relaxed);
            ticker.join();
        }

        if (telemetry.wasCancelled.load(std::memory_order_relaxed)) {
            std::fprintf(stderr, "\n[AllocSight: Scan interrupted by Ctrl+C. Compiling partial report...]\n");
            std::fflush(stderr);
        }

        root->aggregateBottomUp();
        return root;
    }
};

} // namespace allocsight
