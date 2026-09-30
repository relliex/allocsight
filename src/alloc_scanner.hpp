// SPDX-License-Identifier: MIT
// AllocSight - High-Concurrency Disk Allocation & AI Space Cleanup Engine
// Copyright (c) 2026 AllocSight Contributors

#pragma once
#include "alloc_types.hpp"
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>

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
    int  workerThreads     = 0; // 0 = auto-detect hardware concurrency (up to 16)
    int  maxScanDepth      = -1;
    bool quietMode         = false;
};

struct ScanTelemetry {
    std::atomic<uint64_t> filesCount{0};
    std::atomic<uint64_t> dirsCount{0};
    std::atomic<uint64_t> altStreamsCount{0};
    std::atomic<uint64_t> accessErrors{0};
    std::atomic<int64_t>  logicalTotal{0};
    std::atomic<int64_t>  allocatedTotal{0};
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

        taskQueue.push(DirTask{root.get(), norm, 0});
        telemetry.dirsCount.fetch_add(1, std::memory_order_relaxed);

        int nThreads = (cfg.workerThreads > 0) ? cfg.workerThreads : defaultThreadCount();
        telemetry.threadsUsed = nThreads;
        std::vector<std::thread> pool;
        pool.reserve(nThreads);

        auto workerLoop = [&]() {
            while (true) {
                DirTask task{};
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    cv.wait(lk, [&]() { return !taskQueue.empty() || finished; });
                    if (taskQueue.empty() && finished) return;
                    task = std::move(taskQueue.front());
                    taskQueue.pop();
                    ++busyWorkers;
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
                        std::wstring childFullPath = task.dirPath + L"\\" + segName;

                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
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
                            int64_t allocSz = computeAllocatedBytes(childFullPath, fd.dwFileAttributes, logSz, vol);

                            auto fileNode = std::make_unique<FsNode>();
                            fileNode->kind = EntryKind::RegularFile;
                            fileNode->name = segName;
                            fileNode->logicalBytes = logSz;
                            fileNode->allocatedBytes = allocSz;
                            fileNode->winAttrs = fd.dwFileAttributes;
                            fileNode->createdAt = fd.ftCreationTime;
                            fileNode->accessedAt = fd.ftLastAccessTime;
                            fileNode->modifiedAt = fd.ftLastWriteTime;

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

                {
                    std::unique_lock<std::mutex> lk(mtx);
                    for (auto& ct : childTasks) {
                        taskQueue.push(std::move(ct));
                    }
                    --busyWorkers;
                    if (taskQueue.empty() && busyWorkers == 0) {
                        finished = true;
                        cv.notify_all();
                    } else if (!childTasks.empty()) {
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

        root->aggregateBottomUp();
        return root;
    }
};

} // namespace allocsight
