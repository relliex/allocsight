// SPDX-License-Identifier: MIT
// AllocSight - High-Concurrency Disk Allocation & AI Space Cleanup Engine
// Copyright (c) 2026 AllocSight Contributors

#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <map>

namespace allocsight {

enum class EntryKind : uint8_t {
    Volume      = 1,
    Directory   = 2,
    RegularFile = 3
};

inline std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), out.data(), len, nullptr, nullptr);
    return out;
}

inline std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), out.data(), len);
    return out;
}

inline std::string asciiLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

inline std::wstring wideLower(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

inline std::string trimWhitespace(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

inline std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Convert a Windows file path into a clickable file:/// URI with percent-encoded special chars
inline std::string pathToFileUri(const std::wstring& winPath) {
    std::string utf8 = wideToUtf8(winPath);
    std::string uri = "file:///";
    uri.reserve(utf8.size() + 16);
    for (size_t i = 0; i < utf8.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c == '\\') {
            uri += '/';
        } else if (c == ' ') {
            uri += "%20";
        } else if (c == '%') {
            uri += "%25";
        } else if (c == '#') {
            uri += "%23";
        } else if (c == '?') {
            uri += "%3F";
        } else if (c == '[' || c == ']') {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            uri += hex;
        } else {
            uri += static_cast<char>(c);
        }
    }
    return uri;
}

struct VolumeMetrics {
    int64_t clusterBytes   = 4096;
    int64_t totalCapacity  = 0;
    int64_t freeBytes      = 0;
    bool    supportsStreams = false;
    bool    isVolumeRoot   = false;
    std::string fileSystemName;

    VolumeMetrics() = default;

    explicit VolumeMetrics(const std::wstring& targetPath) {
        std::wstring root = extractRootPath(targetPath);
        isVolumeRoot = checkIsVolumeRoot(targetPath);
        if (!root.empty()) {
            DWORD spc = 0, bps = 0, freeClusters = 0, totalClusters = 0;
            if (GetDiskFreeSpaceW(root.c_str(), &spc, &bps, &freeClusters, &totalClusters)) {
                clusterBytes = static_cast<int64_t>(spc) * static_cast<int64_t>(bps);
            }
            ULARGE_INTEGER avail{}, total{}, freeTotal{};
            if (GetDiskFreeSpaceExW(root.c_str(), &avail, &total, &freeTotal)) {
                totalCapacity = static_cast<int64_t>(total.QuadPart);
                freeBytes     = static_cast<int64_t>(freeTotal.QuadPart);
            }
            DWORD fsFlags = 0;
            wchar_t fsNameBuf[64]{};
            if (GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, &fsFlags, fsNameBuf, 64)) {
                supportsStreams = (fsFlags & FILE_NAMED_STREAMS) != 0;
                fileSystemName = wideToUtf8(fsNameBuf);
            }
        }
    }

    static std::wstring extractRootPath(const std::wstring& p) {
        if (p.size() >= 2 && p[1] == L':') {
            return p.substr(0, 2) + L"\\";
        }
        if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
            size_t s1 = p.find(L'\\', 2);
            if (s1 != std::wstring::npos) {
                size_t s2 = p.find(L'\\', s1 + 1);
                if (s2 != std::wstring::npos) return p.substr(0, s2 + 1);
                return p + L"\\";
            }
        }
        return L"";
    }

    static bool checkIsVolumeRoot(std::wstring p) {
        while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        return (p.size() == 2 && p[1] == L':') ||
               (p.size() == 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/'));
    }

    int64_t alignToCluster(int64_t rawBytes) const {
        if (clusterBytes <= 0 || rawBytes <= 0) return 0;
        int64_t rem = rawBytes % clusterBytes;
        return (rem == 0) ? rawBytes : (rawBytes + (clusterBytes - rem));
    }
};

struct FsNode {
    EntryKind kind = EntryKind::Directory;
    std::wstring name;
    int64_t logicalBytes   = 0;
    int64_t allocatedBytes = 0;
    uint32_t winAttrs      = 0;
    FILETIME createdAt{};
    FILETIME accessedAt{};
    FILETIME modifiedAt{};
    bool isAltStream       = false;

    FsNode* parent = nullptr;
    std::vector<std::unique_ptr<FsNode>> children;

    int64_t volumeTotalBytes = 0;
    int64_t volumeFreeBytes  = 0;

    bool isVolume() const { return kind == EntryKind::Volume; }
    bool isDirectory() const { return kind == EntryKind::Directory; }
    bool isFile() const { return kind == EntryKind::RegularFile; }
    bool isContainer() const { return kind == EntryKind::Volume || kind == EntryKind::Directory; }

    FsNode* appendChild(std::unique_ptr<FsNode> child) {
        child->parent = this;
        FsNode* ptr = child.get();
        children.push_back(std::move(child));
        return ptr;
    }

    std::wstring fullPath() const {
        std::vector<const FsNode*> chain;
        for (const FsNode* cur = this; cur != nullptr; cur = cur->parent) {
            chain.push_back(cur);
        }
        std::reverse(chain.begin(), chain.end());
        std::wstring res;
        for (size_t i = 0; i < chain.size(); ++i) {
            const std::wstring& seg = chain[i]->name;
            if (i > 0 && !res.empty() && res.back() != L'\\' && !seg.empty() && seg.front() != L':') {
                res += L'\\';
            }
            res += seg;
        }
        return res;
    }

    std::wstring parentPath() const {
        return parent ? parent->fullPath() : L"";
    }

    int64_t unaccountedSystemBytes() const {
        if (!isVolume() || volumeTotalBytes <= 0) return 0;
        int64_t accounted = allocatedBytes + volumeFreeBytes;
        return (volumeTotalBytes > accounted) ? (volumeTotalBytes - accounted) : 0;
    }

    void aggregateBottomUp() {
        if (isContainer()) {
            int64_t logTotal = 0;
            int64_t allocTotal = 0;
            for (auto& c : children) {
                c->aggregateBottomUp();
                logTotal   += c->logicalBytes;
                allocTotal += c->allocatedBytes;
            }
            logicalBytes   = logTotal;
            allocatedBytes = allocTotal;
        }
    }

    void countSubtree(uint64_t& outFiles, uint64_t& outDirs) const {
        for (const auto& c : children) {
            if (c->isFile()) {
                ++outFiles;
            } else if (c->isDirectory()) {
                ++outDirs;
                c->countSubtree(outFiles, outDirs);
            } else if (c->isVolume()) {
                c->countSubtree(outFiles, outDirs);
            }
        }
    }
};

inline bool tryAcquireBackupPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeBackupPrivilege", &luid)) {
        CloseHandle(hToken);
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
    DWORD err = GetLastError();
    CloseHandle(hToken);
    return ok && (err == ERROR_SUCCESS);
}

inline std::string humanBytes(int64_t bytes) {
    if (bytes < 0) bytes = 0;
    static const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double val = static_cast<double>(bytes);
    int idx = 0;
    while (val >= 1024.0 && idx < 5) {
        val /= 1024.0;
        ++idx;
    }
    char buf[64];
    if (idx == 0) {
        std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(bytes));
    } else {
        std::snprintf(buf, sizeof(buf), "%.1f %s", val, kUnits[idx]);
    }
    return buf;
}

inline uint64_t fileTimeToTicks(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | static_cast<uint64_t>(ft.dwLowDateTime);
}

inline FILETIME nowFileTime() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return ft;
}

inline std::string formatDateYMD(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return "N/A";
    FILETIME localFt{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &localFt);
    FileTimeToSystemTime(&localFt, &st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

} // namespace allocsight
