#pragma once

#include <Windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <variant>
#include <unordered_map>
#include <mutex>
#include <cstring> 

using FileIdVariant = std::variant<ULONGLONG, FILE_ID_128>;

enum class OutputFormat { TXT, CSV, JSON };
enum class ReplaceType { COPY, TYPE, EXPLORER, ALL };

struct USNEntry {
    FileIdVariant fileId;
    ULONGLONG usn;
    std::wstring name;
    FILETIME date;
    std::string reason;
    std::wstring directory;
};

struct FileEvent {
    FILETIME date;
    std::string reason;
    std::wstring name;
    std::wstring directory;
};

struct AggregatedUSNEntry {
    std::wstring name;
    std::wstring directory;
    FileIdVariant fileId;
    std::vector<FileEvent> events;
};

struct FileIdHash {
    std::size_t operator()(const FileIdVariant& fid) const {
        if (std::holds_alternative<ULONGLONG>(fid)) {
            return std::hash<ULONGLONG>{}(std::get<ULONGLONG>(fid));
        }
        else {
            const auto& fid128 = std::get<FILE_ID_128>(fid);
            return std::hash<std::string>{}(std::string(reinterpret_cast<const char*>(&fid128), sizeof(FILE_ID_128)));
        }
    }
};

struct FileIdEqual {
    bool operator()(const FileIdVariant& a, const FileIdVariant& b) const {
        if (a.index() != b.index()) return false;
        if (std::holds_alternative<ULONGLONG>(a)) {
            return std::get<ULONGLONG>(a) == std::get<ULONGLONG>(b);
        }
        else {
            const auto& a128 = std::get<FILE_ID_128>(a);
            const auto& b128 = std::get<FILE_ID_128>(b);
            return memcmp(&a128, &b128, sizeof(FILE_ID_128)) == 0;
        }
    }
};