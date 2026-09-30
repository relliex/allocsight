// SPDX-License-Identifier: MIT
// AllocSight - High-Concurrency Disk Allocation & AI Space Cleanup Engine
// Copyright (c) 2026 AllocSight Contributors

#pragma once
#include "alloc_types.hpp"
#include <string_view>

namespace allocsight {

struct FileCategory {
    std::string name;
    std::vector<std::string> extensions;
};

inline std::vector<FileCategory> defaultFileCategories() {
    return {
        {"AI Models & Weights",      {"gguf","safetensors","pt","pth","onnx","ckpt","tflite","pb","h5"}},
        {"Archives & Packages",      {"zip","7z","rar","tar","gz","tgz","bz2","xz","zst","whl","conda","nupkg","vsix","cab"}},
        {"Virtual Disks & Images",   {"vmdk","vhd","vhdx","vdi","qcow2","iso","img","vmem","nvram"}},
        {"Installers & Binaries",    {"exe","msi","dll","sys","lib","pdb","so","a","ocx"}},
        {"Datasets & Databases",     {"jsonl","ndjson","parquet","arrow","feather","csv","tsv","db","sqlite","sqlite3","mdf","ldf"}},
        {"Video & Audio Media",      {"mp4","mkv","mov","avi","wmv","flv","webm","m4v","mp3","flac","wav","aac","ogg","m4a","ncm"}},
        {"Images & Textures",        {"png","jpg","jpeg","webp","gif","bmp","tif","tiff","psd","svg","ico","dds","hdr","exr"}},
        {"Logs, Dumps & Temp",       {"log","dmp","tmp","temp","bak","old","crdownload","part","ushaderprecache"}}
    };
}

enum class MetricKind : uint8_t {
    AllocatedBytes,
    LogicalBytes,
    ModifiedAgeTicks,
    CreatedAgeTicks,
    AccessedAgeTicks
};

struct MetricPredicate {
    MetricKind kind;
    bool       isUpperBound; // true: value <= threshold, false: value >= threshold
    uint64_t   threshold;
};

struct AttributePredicate {
    uint32_t winAttrMask = 0;
    bool     matchAltStream = false;
    bool     mustBeAbsent = false;
};

class QueryFilter {
public:
    explicit QueryFilter(const std::string& expr = "", const std::vector<FileCategory>& categories = defaultFileCategories())
        : rawExpr_(expr), categories_(categories) {
        nowTicks_ = fileTimeToTicks(nowFileTime());
        parseExpression(expr);
    }

    bool isEmpty() const { return empty_; }
    const std::string& rawExpression() const { return rawExpr_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

    // Non-recursive, linear-space iterative two-pointer wildcard matcher
    static bool wildcardMatch(std::string_view text, std::string_view pattern) {
        size_t tIdx = 0;
        size_t pIdx = 0;
        size_t starIdx = std::string_view::npos;
        size_t matchIdx = 0;

        while (tIdx < text.size()) {
            if (pIdx < pattern.size() && (pattern[pIdx] == '?' || foldAscii(pattern[pIdx]) == foldAscii(text[tIdx]))) {
                ++tIdx;
                ++pIdx;
            } else if (pIdx < pattern.size() && pattern[pIdx] == '*') {
                starIdx = pIdx;
                matchIdx = tIdx;
                ++pIdx;
            } else if (starIdx != std::string_view::npos) {
                pIdx = starIdx + 1;
                ++matchIdx;
                tIdx = matchIdx;
            } else {
                return false;
            }
        }

        while (pIdx < pattern.size() && pattern[pIdx] == '*') {
            ++pIdx;
        }
        return pIdx == pattern.size();
    }

    static bool globMatch(std::string_view text, std::string_view pattern) {
        return wildcardMatch(text, pattern);
    }

    bool matchesFile(const FsNode* node) const {
        if (empty_ || !node->isFile()) return true;

        std::string fname = asciiLower(wideToUtf8(node->name));

        for (const auto& exMask : excludeFileGlobs_) {
            if (wildcardMatch(fname, exMask)) return false;
        }

        if (!includeFileGlobs_.empty()) {
            bool anyMatch = false;
            for (const auto& inMask : includeFileGlobs_) {
                if (wildcardMatch(fname, inMask)) {
                    anyMatch = true;
                    break;
                }
            }
            if (!anyMatch) return false;
        }

        if (!excludeDirGlobs_.empty() || !includeDirGlobs_.empty()) {
            bool matchedDir = includeDirGlobs_.empty();
            for (const FsNode* cur = node->parent; cur != nullptr; cur = cur->parent) {
                if (!cur->isVolume()) {
                    std::string dname = asciiLower(wideToUtf8(cur->name));
                    for (const auto& exD : excludeDirGlobs_) {
                        if (wildcardMatch(dname, exD)) return false;
                    }
                    if (!matchedDir) {
                        for (const auto& inD : includeDirGlobs_) {
                            if (wildcardMatch(dname, inD)) {
                                matchedDir = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (!matchedDir) return false;
        }

        for (const auto& pred : metricPredicates_) {
            uint64_t actual = extractNodeMetric(node, pred.kind);
            if (pred.isUpperBound) {
                if (actual > pred.threshold) return false;
            } else {
                if (actual < pred.threshold) return false;
            }
        }

        for (const auto& attrPred : attrPredicates_) {
            bool hasAttr = attrPred.matchAltStream
                ? node->isAltStream
                : ((node->winAttrs & attrPred.winAttrMask) != 0);
            if (attrPred.mustBeAbsent && hasAttr) return false;
            if (!attrPred.mustBeAbsent && !hasAttr) return false;
        }

        return true;
    }

    std::unique_ptr<FsNode> applyToTree(const FsNode* root) const {
        if (!root) return nullptr;
        auto out = cloneMatchingSubtree(root, true);
        if (out) out->aggregateBottomUp();
        return out;
    }

    std::string classifyFile(const std::wstring& fileName) const {
        auto dot = fileName.find_last_of(L'.');
        if (dot == std::wstring::npos || dot + 1 >= fileName.size()) return "Other / Unclassified";
        std::string ext = asciiLower(wideToUtf8(fileName.substr(dot + 1)));
        for (const auto& cat : categories_) {
            for (const auto& e : cat.extensions) {
                if (e == ext) return cat.name;
            }
        }
        return "Other / Unclassified";
    }

private:
    static char foldAscii(char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    uint64_t extractNodeMetric(const FsNode* node, MetricKind kind) const {
        switch (kind) {
            case MetricKind::AllocatedBytes:
                return (node->allocatedBytes > 0) ? static_cast<uint64_t>(node->allocatedBytes) : 0ULL;
            case MetricKind::LogicalBytes:
                return (node->logicalBytes > 0) ? static_cast<uint64_t>(node->logicalBytes) : 0ULL;
            case MetricKind::ModifiedAgeTicks: {
                uint64_t t = fileTimeToTicks(node->modifiedAt);
                return (nowTicks_ > t) ? (nowTicks_ - t) : 0ULL;
            }
            case MetricKind::CreatedAgeTicks: {
                uint64_t t = fileTimeToTicks(node->createdAt);
                return (nowTicks_ > t) ? (nowTicks_ - t) : 0ULL;
            }
            case MetricKind::AccessedAgeTicks: {
                uint64_t t = fileTimeToTicks(node->accessedAt);
                return (nowTicks_ > t) ? (nowTicks_ - t) : 0ULL;
            }
        }
        return 0ULL;
    }

    std::unique_ptr<FsNode> cloneMatchingSubtree(const FsNode* node, bool isRoot) const {
        if (node->isFile()) {
            if (!matchesFile(node)) return nullptr;
            auto copy = std::make_unique<FsNode>();
            copy->kind = node->kind;
            copy->name = node->name;
            copy->logicalBytes = node->logicalBytes;
            copy->allocatedBytes = node->allocatedBytes;
            copy->winAttrs = node->winAttrs;
            copy->createdAt = node->createdAt;
            copy->accessedAt = node->accessedAt;
            copy->modifiedAt = node->modifiedAt;
            copy->isAltStream = node->isAltStream;
            return copy;
        }

        if (node->isContainer()) {
            if (!isRoot && !node->isVolume() && !excludeDirGlobs_.empty()) {
                std::string dname = asciiLower(wideToUtf8(node->name));
                for (const auto& exD : excludeDirGlobs_) {
                    if (wildcardMatch(dname, exD)) return nullptr;
                }
            }

            auto copy = std::make_unique<FsNode>();
            copy->kind = node->kind;
            copy->name = node->name;
            copy->winAttrs = node->winAttrs;
            copy->createdAt = node->createdAt;
            copy->accessedAt = node->accessedAt;
            copy->modifiedAt = node->modifiedAt;
            copy->isAltStream = node->isAltStream;
            copy->volumeTotalBytes = node->volumeTotalBytes;
            copy->volumeFreeBytes = node->volumeFreeBytes;

            for (const auto& c : node->children) {
                auto fc = cloneMatchingSubtree(c.get(), false);
                if (fc) copy->appendChild(std::move(fc));
            }

            if (copy->children.empty() && !isRoot) return nullptr;
            return copy;
        }
        return nullptr;
    }

    void parseExpression(const std::string& expr) {
        std::string trimmed = trimWhitespace(expr);
        if (trimmed.empty()) {
            empty_ = true;
            return;
        }
        empty_ = false;

        size_t pos = 0;
        while (pos <= trimmed.size()) {
            size_t semi = trimmed.find(';', pos);
            std::string tok = (semi == std::string::npos)
                ? trimmed.substr(pos)
                : trimmed.substr(pos, semi - pos);
            tok = trimWhitespace(tok);
            if (!tok.empty()) parseClause(tok);
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
    }

    void parseClause(const std::string& rawClause) {
        bool negated = false;
        std::string tok = rawClause;
        if (!tok.empty() && (tok[0] == '!' || tok[0] == '-')) {
            negated = true;
            tok = trimWhitespace(tok.substr(1));
        }
        if (tok.empty()) return;

        std::string low = asciiLower(tok);

        // Directory filter via explicit key ("dir:<glob>" or "folder:<glob>") or trailing slash ("node_modules/")
        if (low.rfind("dir:", 0) == 0 || low.rfind("folder:", 0) == 0 || low.rfind("@dir:", 0) == 0) {
            size_t colon = low.find(':');
            std::string mask = trimWhitespace(low.substr(colon + 1));
            if (!mask.empty()) {
                if (negated) excludeDirGlobs_.push_back(mask);
                else includeDirGlobs_.push_back(mask);
            }
            return;
        }
        if (low.size() > 1 && low.back() == '/') {
            std::string mask = low.substr(0, low.size() - 1);
            if (negated) excludeDirGlobs_.push_back(mask);
            else includeDirGlobs_.push_back(mask);
            return;
        }

        if (low.rfind("@category:", 0) == 0 || low.rfind("category:", 0) == 0) {
            size_t colon = low.find(':');
            std::string qname = asciiLower(trimWhitespace(tok.substr(colon + 1)));
            bool matchedCat = false;
            for (const auto& cat : categories_) {
                std::string cname = asciiLower(cat.name);
                if (cname == qname || cname.find(qname) != std::string::npos) {
                    matchedCat = true;
                    for (const auto& ext : cat.extensions) {
                        if (negated) excludeFileGlobs_.push_back("*." + ext);
                        else includeFileGlobs_.push_back("*." + ext);
                    }
                }
            }
            if (!matchedCat) warnings_.push_back("Unknown category filter: " + tok);
            return;
        }

        if (low.rfind("@attr:", 0) == 0 || low.rfind("attr:", 0) == 0) {
            size_t colon = low.find(':');
            parseAttributeClause(low.substr(colon + 1), negated);
            return;
        }

        size_t opIdx = tok.find_first_of("<>");
        if (opIdx != std::string::npos) {
            char op = tok[opIdx];
            std::string lhs = asciiLower(trimWhitespace(tok.substr(0, opIdx)));
            std::string rhs = asciiLower(trimWhitespace(tok.substr(opIdx + 1)));
            if (parseMetricComparison(lhs, op, rhs)) return;
            warnings_.push_back("Invalid size or age clause: " + rawClause);
            return;
        }

        if (negated) excludeFileGlobs_.push_back(low);
        else includeFileGlobs_.push_back(low);
    }

    static bool isTimeFieldKeyword(const std::string& k) {
        return k == "age" || k == "modified" || k == "mtime" || k == "mod" ||
               k == "created" || k == "ctime" || k == "accessed" || k == "atime";
    }

    bool parseMetricComparison(const std::string& lhs, char op, const std::string& rhs) {
        size_t i = 0;
        while (i < rhs.size() && ((rhs[i] >= '0' && rhs[i] <= '9') || rhs[i] == '.')) ++i;
        if (i == 0) return false;
        double val = std::stod(rhs.substr(0, i));
        std::string unit = trimWhitespace(rhs.substr(i));
        if (unit.empty()) unit = "b";

        uint64_t byteScale = 0;
        if (unit == "b" || unit == "byte" || unit == "bytes") byteScale = 1ULL;
        else if (unit == "k" || unit == "kb" || unit == "kib") byteScale = 1024ULL;
        else if (unit == "m" || unit == "mb" || unit == "mib") byteScale = 1024ULL * 1024ULL;
        else if (unit == "g" || unit == "gb" || unit == "gib") byteScale = 1024ULL * 1024ULL * 1024ULL;
        else if (unit == "t" || unit == "tb" || unit == "tib") byteScale = 1024ULL * 1024ULL * 1024ULL * 1024ULL;

        if (byteScale > 0 && !(unit == "m" && isTimeFieldKeyword(lhs))) {
            MetricKind kind = MetricKind::AllocatedBytes;
            if (!lhs.empty()) {
                if (lhs == "logical" || lhs == "log" || lhs == "size") {
                    kind = MetricKind::LogicalBytes;
                } else if (lhs == "allocated" || lhs == "alloc" || lhs == "cluster" || lhs == "disk") {
                    kind = MetricKind::AllocatedBytes;
                } else {
                    return false;
                }
            }
            uint64_t threshold = static_cast<uint64_t>(val * static_cast<double>(byteScale));
            metricPredicates_.push_back(MetricPredicate{kind, (op == '<'), threshold});
            return true;
        }

        constexpr uint64_t kSecTicks = 10000000ULL;
        uint64_t timeScale = 0;
        if (unit == "s" || unit == "sec" || unit == "secs" || unit == "second" || unit == "seconds") timeScale = kSecTicks;
        else if (unit == "m" || unit == "min" || unit == "mins" || unit == "minute" || unit == "minutes") timeScale = 60ULL * kSecTicks;
        else if (unit == "h" || unit == "hr" || unit == "hrs" || unit == "hour" || unit == "hours") timeScale = 3600ULL * kSecTicks;
        else if (unit == "d" || unit == "day" || unit == "days") timeScale = 86400ULL * kSecTicks;
        else if (unit == "w" || unit == "wk" || unit == "wks" || unit == "week" || unit == "weeks") timeScale = 7ULL * 86400ULL * kSecTicks;
        else if (unit == "mo" || unit == "mon" || unit == "month" || unit == "months") timeScale = 30ULL * 86400ULL * kSecTicks;
        else if (unit == "y" || unit == "yr" || unit == "yrs" || unit == "year" || unit == "years") timeScale = 365ULL * 86400ULL * kSecTicks;

        if (timeScale == 0) return false;
        uint64_t ticks = static_cast<uint64_t>(val * static_cast<double>(timeScale));

        MetricKind kind = MetricKind::ModifiedAgeTicks;
        if (!lhs.empty()) {
            if (lhs == "created" || lhs == "ctime") kind = MetricKind::CreatedAgeTicks;
            else if (lhs == "accessed" || lhs == "atime") kind = MetricKind::AccessedAgeTicks;
            else if (lhs == "modified" || lhs == "mtime" || lhs == "mod" || lhs == "age") kind = MetricKind::ModifiedAgeTicks;
            else return false;
        }

        metricPredicates_.push_back(MetricPredicate{kind, (op == '<'), ticks});
        return true;
    }

    void parseAttributeClause(const std::string& expr, bool globalNeg) {
        size_t i = 0;
        char op = '+';
        while (i < expr.size()) {
            if (expr[i] == '+' || expr[i] == '-' || expr[i] == ',') {
                if (expr[i] != ',') op = expr[i];
                ++i;
                continue;
            }
            size_t j = i;
            while (j < expr.size() && expr[j] != '+' && expr[j] != '-' && expr[j] != ',') ++j;
            std::string token = trimWhitespace(expr.substr(i, j - i));
            i = j;
            if (token.empty()) continue;

            uint32_t flag = 0;
            bool streamFlag = false;
            if (token == "archive") flag = FILE_ATTRIBUTE_ARCHIVE;
            else if (token == "system" || token == "sys") flag = FILE_ATTRIBUTE_SYSTEM;
            else if (token == "readonly" || token == "ro") flag = FILE_ATTRIBUTE_READONLY;
            else if (token == "hidden" || token == "hid") flag = FILE_ATTRIBUTE_HIDDEN;
            else if (token == "compressed" || token == "comp") flag = FILE_ATTRIBUTE_COMPRESSED;
            else if (token == "encrypted" || token == "enc") flag = FILE_ATTRIBUTE_ENCRYPTED;
            else if (token == "offline") flag = FILE_ATTRIBUTE_OFFLINE;
            else if (token == "temporary" || token == "temp") flag = FILE_ATTRIBUTE_TEMPORARY;
            else if (token == "sparse") flag = FILE_ATTRIBUTE_SPARSE_FILE;
            else if (token == "ads" || token == "stream" || token == "streams") streamFlag = true;
            else {
                warnings_.push_back("Unknown attribute flag: " + token);
                continue;
            }

            bool mustBeAbsent = globalNeg ^ (op == '-');
            attrPredicates_.push_back(AttributePredicate{flag, streamFlag, mustBeAbsent});
        }
    }

    std::string rawExpr_;
    std::vector<FileCategory> categories_;
    bool empty_ = true;
    uint64_t nowTicks_ = 0;
    std::vector<std::string> warnings_;

    std::vector<std::string> includeFileGlobs_;
    std::vector<std::string> excludeFileGlobs_;
    std::vector<std::string> includeDirGlobs_;
    std::vector<std::string> excludeDirGlobs_;

    std::vector<MetricPredicate> metricPredicates_;
    std::vector<AttributePredicate> attrPredicates_;
};

} // namespace allocsight
