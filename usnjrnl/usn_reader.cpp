#include "usn_reader.h"
#include "usn_utils.h"
#include <Windows.h>
#include <winioctl.h>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>
#include <unordered_map>
#include <format>
#include <iostream>
#include <thread>
#include <mutex>
#include <shellapi.h>
#include <fstream>
#include <ranges>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <set>

#include "time_utils.h"

USNJournalReader::USNJournalReader(const std::wstring& volumeLetter) : volumeLetter_(volumeLetter) {}

void USNJournalReader::Run() {
    std::wcout << L"[*] Starting USN Journal analysis...\n";
    auto startTime = std::chrono::high_resolution_clock::now();

    if (!Dump()) {
        std::wcerr << L"[-] Failed to read the USN Journal.\n";
        return;
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(endTime - startTime).count();

    std::wcout << std::format(L"[+] Completed in {:.3f} seconds\n", duration);
    std::wcout << std::format(L"[+] Total records: {}\n", entries_.size());
    std::wcout << std::format(L"[+] Total aggregated files: {}\n", EventsFileID().size());

    if (onlyReplace_) {
        if (consoleOutput_)
            WriteReplacesToConsole();
        else
            WriteReplacesToFile();
    }
    else {
        if (consoleOutput_) {
            WriteIndividualToConsole();
            WriteReplacesToConsole();
        }
        else {
            WriteIndividualToFile();
            WriteReplacesToFile();
        }
    }
}

std::vector<USNEntry> USNJournalReader::GetEntriesCopy() {
    std::lock_guard<std::mutex> lock(entriesMutex_);
    return entries_;
}

std::vector<AggregatedUSNEntry> USNJournalReader::EventsFileID() {
    std::unordered_map<FileIdVariant, AggregatedUSNEntry, FileIdHash, FileIdEqual> aggMap;

    for (auto& entry : GetEntriesCopy()) {
        auto it = aggMap.find(entry.fileId);
        if (it == aggMap.end()) {
            AggregatedUSNEntry agg;
            agg.fileId = entry.fileId;
            agg.events.push_back({ entry.date, entry.reason, entry.name, entry.directory });
            aggMap[entry.fileId] = agg;
        }
        else {
            it->second.events.push_back({ entry.date, entry.reason, entry.name, entry.directory });
        }
    }

    std::vector<AggregatedUSNEntry> result;
    for (auto& [_, agg] : aggMap) {
        std::sort(agg.events.begin(), agg.events.end(),
            [](const FileEvent& a, const FileEvent& b) {
                return CompareFileTime(&a.date, &b.date) < 0;
            });
        if (!agg.events.empty()) {
            const auto& lastEvent = agg.events.back();
            agg.name = lastEvent.name;
            agg.directory = lastEvent.directory;
        }
        result.push_back(agg);
    }

    return result;
}

void USNJournalReader::EnableAfterLogonFilter(time_t logonTime) {
    filterAfterLogon_ = true;
    logonTime_ = logonTime;
}

std::string USNJournalReader::FileIdToString(const FileIdVariant& fid) {
    if (std::holds_alternative<ULONGLONG>(fid)) {
        return std::to_string(std::get<ULONGLONG>(fid));
    }
    else {
        const auto& fid128 = std::get<FILE_ID_128>(fid);
        return std::string(reinterpret_cast<const char*>(&fid128), sizeof(FILE_ID_128));
    }
}

bool USNJournalReader::Dump() {
    if (!OpenVolume() || !QueryJournal() || !AllocateBuffer())
        return false;

    READ_USN_JOURNAL_DATA_V0 readData{};
    readData.StartUsn = journalData_.FirstUsn;
    readData.ReasonMask = 0xFFFFFFFF;
    readData.UsnJournalID = journalData_.UsnJournalID;

    const DWORD bufferSize = 32 * 1024 * 1024;
    DWORD bytesReturned = 0;

    entries_.reserve(200000);

    while (DeviceIoControl(volumeHandle_, FSCTL_READ_USN_JOURNAL, &readData, sizeof(readData),
        buffer_.get(), bufferSize, &bytesReturned, nullptr)) {
        if (bytesReturned <= sizeof(USN))
            break;

        BYTE* ptr = buffer_.get() + sizeof(USN);
        BYTE* end = buffer_.get() + bytesReturned;

        while (ptr < end) {
            auto common = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(ptr);
            if (common->RecordLength == 0) break;

            std::wstring name = L"?";
            std::wstring directory = L"?";
            FILETIME ft{}, localTime{};
            std::string reasonStr;
            ULONGLONG usn = 0;
            FileIdVariant fileId{ 0ULL };

            if (common->MajorVersion == 2) {
                auto rec = reinterpret_cast<USN_RECORD_V2*>(ptr);
                name.assign(reinterpret_cast<WCHAR*>((BYTE*)rec + rec->FileNameOffset),
                    rec->FileNameLength / sizeof(WCHAR));
                directory = GetDirectoryById(rec->ParentFileReferenceNumber);
                ft.dwLowDateTime = rec->TimeStamp.LowPart;
                ft.dwHighDateTime = rec->TimeStamp.HighPart;
                FileTimeToLocalFileTime(&ft, &localTime);
                reasonStr = ReasonToString(rec->Reason);
                usn = rec->Usn;
                fileId = rec->FileReferenceNumber;
            }
            else if (common->MajorVersion == 3) {
                auto rec = reinterpret_cast<USN_RECORD_V3*>(ptr);
                name.assign(reinterpret_cast<WCHAR*>((BYTE*)rec + rec->FileNameOffset),
                    rec->FileNameLength / sizeof(WCHAR));
                directory = GetDirectoryById(rec->ParentFileReferenceNumber);
                ft.dwLowDateTime = rec->TimeStamp.LowPart;
                ft.dwHighDateTime = rec->TimeStamp.HighPart;
                FileTimeToLocalFileTime(&ft, &localTime);
                reasonStr = ReasonToString(rec->Reason);
                usn = rec->Usn;
                fileId = rec->FileReferenceNumber;  // FILE_ID_128
            }
            else if (common->MajorVersion == 4) {
                auto rec = reinterpret_cast<USN_RECORD_V4*>(ptr);
                name = L"[Requires lookup]";
                directory = GetDirectoryById(rec->FileReferenceNumber);
                reasonStr = ReasonToString(rec->Reason);
                usn = rec->Usn;
                fileId = rec->FileReferenceNumber;  // FILE_ID_128
            }

            PushEntry(fileId, usn, name, localTime, reasonStr, directory);
            ptr += common->RecordLength;
        }

        readData.StartUsn = *(USN*)buffer_.get();
    }

    Cleanup();
    return true;
}

bool USNJournalReader::OpenVolume() {
    std::wstring devicePath = L"\\\\.\\" + volumeLetter_;
    volumeHandle_ = CreateFileW(devicePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    return volumeHandle_ != INVALID_HANDLE_VALUE;
}

bool USNJournalReader::QueryJournal() {
    DWORD bytesReturned = 0;
    return DeviceIoControl(volumeHandle_, FSCTL_QUERY_USN_JOURNAL, nullptr, 0,
        &journalData_, sizeof(journalData_), &bytesReturned, nullptr);
}

bool USNJournalReader::AllocateBuffer() {
    const DWORD bufferSize = 32 * 1024 * 1024;
    buffer_ = std::make_unique<BYTE[]>(bufferSize);  // RAII
    return buffer_ != nullptr;
}

std::wstring USNJournalReader::GetDirectoryById(ULONGLONG fileId) {
    FileIdVariant key = fileId;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = pathCache_.find(key);
        if (it != pathCache_.end()) return it->second;
    }

    FILE_ID_DESCRIPTOR desc{};
    desc.dwSize = sizeof(desc);
    desc.Type = FileIdType;
    desc.FileId.QuadPart = (LONGLONG)fileId;

    HANDLE fileHandle = OpenFileById(volumeHandle_, &desc, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, FILE_FLAG_BACKUP_SEMANTICS);

    std::wstring directory = L"?";
    if (fileHandle != INVALID_HANDLE_VALUE) {
        WCHAR path[MAX_PATH] = {};
        DWORD ret = GetFinalPathNameByHandleW(fileHandle, path, MAX_PATH, FILE_NAME_NORMALIZED);
        CloseHandle(fileHandle);

        if (ret > 0 && ret < MAX_PATH) {
            std::wstring fullPath(path);
            if (fullPath.rfind(L"\\\\?\\", 0) == 0)
                fullPath = fullPath.substr(4);
            directory = fullPath;
        }
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        pathCache_[key] = directory;
    }

    return directory;
}

std::wstring USNJournalReader::GetDirectoryById(const FILE_ID_128& fileId128) {
    FileIdVariant key = fileId128;
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        auto it = pathCache_.find(key);
        if (it != pathCache_.end()) return it->second;
    }

    FILE_ID_DESCRIPTOR desc{};
    desc.dwSize = sizeof(desc);
#if (_WIN32_WINNT >= 0x0602)
    desc.Type = ExtendedFileIdType;
    desc.ExtendedFileId = fileId128;
#else
    return L"[Unsupported: FILE_ID_128]";
#endif

    HANDLE fileHandle = OpenFileById(volumeHandle_, &desc, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, FILE_FLAG_BACKUP_SEMANTICS);

    std::wstring directory = L"?";
    if (fileHandle != INVALID_HANDLE_VALUE) {
        WCHAR path[MAX_PATH] = {};
        DWORD ret = GetFinalPathNameByHandleW(fileHandle, path, MAX_PATH, FILE_NAME_NORMALIZED);
        CloseHandle(fileHandle);

        if (ret > 0 && ret < MAX_PATH) {
            std::wstring fullPath(path);
            if (fullPath.rfind(L"\\\\?\\", 0) == 0)
                fullPath = fullPath.substr(4);
            directory = fullPath;
        }
    }

    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        pathCache_[key] = directory;
    }

    return directory;
}

std::string USNJournalReader::ReasonToString(DWORD reason) const {
    struct ReasonFlag { DWORD flag; const char* desc; };
    static const ReasonFlag reasons[] = {
        {USN_REASON_DATA_OVERWRITE, "Data Overwrite"},
        {USN_REASON_DATA_EXTEND, "Data Extend"},
        {USN_REASON_DATA_TRUNCATION, "Data Truncation"},
        {USN_REASON_NAMED_DATA_OVERWRITE, "Named Data Overwrite"},
        {USN_REASON_NAMED_DATA_EXTEND, "Named Data Extend"},
        {USN_REASON_NAMED_DATA_TRUNCATION, "Named Data Truncation"},
        {USN_REASON_FILE_CREATE, "File Create"},
        {USN_REASON_FILE_DELETE, "File Delete"},
        {USN_REASON_EA_CHANGE, "EA Change"},
        {USN_REASON_SECURITY_CHANGE, "Security Change"},
        {USN_REASON_RENAME_OLD_NAME, "Rename Old Name"},
        {USN_REASON_RENAME_NEW_NAME, "Rename New Name"},
        {USN_REASON_INDEXABLE_CHANGE, "Indexable Change"},
        {USN_REASON_BASIC_INFO_CHANGE, "Basic Info Change"},
        {USN_REASON_HARD_LINK_CHANGE, "Hard Link Change"},
        {USN_REASON_COMPRESSION_CHANGE, "Compression Change"},
        {USN_REASON_ENCRYPTION_CHANGE, "Encryption Change"},
        {USN_REASON_OBJECT_ID_CHANGE, "Object ID Change"},
        {USN_REASON_REPARSE_POINT_CHANGE, "Reparse Point Change"},
        {USN_REASON_STREAM_CHANGE, "Stream Change"},
        {USN_REASON_TRANSACTED_CHANGE, "Transacted Change"},
        {USN_REASON_INTEGRITY_CHANGE, "Integrity Change"},
        {USN_REASON_CLOSE, "Close"}
    };

    std::string result;
    for (const auto& r : reasons)
        if (reason & r.flag) {
            if (!result.empty()) result += " | ";
            result += r.desc;
        }

    return result.empty() ? "?" : result;
}

bool USNJournalReader::CheckPatternSequential(
    const std::vector<std::string>& window,
    const std::vector<std::vector<std::string>>& pattern
) {
    if (window.size() != pattern.size())
        return false;

    for (size_t i = 0; i < pattern.size(); ++i) {
        for (const auto& requiredFlag : pattern[i]) {
            if (window[i].find(requiredFlag) == std::string::npos)
                return false;
        }
    }

    return true;
}

bool USNJournalReader::IsCopyReplacement(const std::vector<FileEvent>& events) {
    if (events.size() < 5)
        return false;

    for (size_t i = 0; i + 5 <= events.size(); ++i) {
        std::vector<std::string> window;
        for (size_t j = 0; j < 5; ++j)
            window.push_back(events[i + j].reason);

        if (CheckPatternSequential(window, COPY_PATTERN_1) ||
            CheckPatternSequential(window, COPY_PATTERN_2))
            return true;
    }

    return false;
}

bool USNJournalReader::IsTypeReplacement(const std::vector<FileEvent>& events) {
    if (events.size() < 2)
        return false;

    for (size_t i = 0; i + 2 <= events.size(); ++i) {
        std::vector<std::string> window;
        for (size_t j = 0; j < 2; ++j)
            window.push_back(events[i + j].reason);

        if (CheckPatternSequential(window, TYPE_PATTERN_1) ||
            CheckPatternSequential(window, TYPE_PATTERN_2))
            return true;
    }

    return false;
}

bool USNJournalReader::IsExplorerReplacement(const std::vector<USNEntry>& orderedEntries, size_t startIndex) {
    if (startIndex + 4 > orderedEntries.size())
        return false;

    std::wstring commonName = orderedEntries[startIndex].name;
    for (size_t i = 1; i < 4; ++i) {
        if (orderedEntries[startIndex + i].name != commonName)
            return false;
    }

    std::vector<std::string> window;
    for (size_t j = 0; j < 4; ++j)
        window.push_back(orderedEntries[startIndex + j].reason);

    return CheckPatternSequential(window, EXPLORER_PATTERN);
}

void USNJournalReader::PushEntry(
    FileIdVariant fileId,
    ULONGLONG usn,
    const std::wstring& name,
    const FILETIME& date,
    const std::string& reason,
    const std::wstring& dir)
{
    if (filterAfterLogon_) {
        time_t eventTime = LocalFileTimeToTimeT(date);
        if (eventTime < logonTime_)
            return;
    }

    if (filterAfterDate_) {
        time_t eventTime = LocalFileTimeToTimeT(date);
        if (eventTime < filterDate_)
            return;
    }

    if (!filterNames_.empty()) {
        std::string nameUtf8 = to_utf8(name);
        bool match = false;
        for (const auto& filter : filterNames_) {
            if (nameUtf8.find(filter) != std::string::npos) {
                match = true;
                break;
            }
        }
        if (!match) return;
    }

    if (!filterReasons_.empty()) {
        bool match = false;
        for (const auto& filter : filterReasons_) {
            if (reason.find(filter) != std::string::npos) {
                match = true;
                break;
            }
        }
        if (!match) return;
    }

    if (!filterIds_.empty()) {
        std::string idStr = FileIdToString(fileId);
        bool match = false;
        for (const auto& filter : filterIds_) {
            if (idStr.find(filter) != std::string::npos) {
                match = true;
                break;
            }
        }
        if (!match) return;
    }

    if (!filterPaths_.empty()) {
        std::string dirUtf8 = to_utf8(dir);
        bool match = false;
        for (const auto& filter : filterPaths_) {
            if (filterPathRecursive_) {
                if (dirUtf8.find(filter) != std::string::npos) {
                    match = true;
                    break;
                }
            }
            else {
                if (dirUtf8 == filter || dirUtf8.find(filter + "\\") == 0) {
                    match = true;
                    break;
                }
            }
        }
        if (!match) return;
    }

    USNEntry entry{ fileId, usn, name, date, reason, dir };
    std::lock_guard<std::mutex> lock(entriesMutex_);
    entries_.push_back(entry);
}

void USNJournalReader::Cleanup() {
    if (volumeHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(volumeHandle_);
        volumeHandle_ = INVALID_HANDLE_VALUE;
    }
    buffer_.reset();  // RAII cleanup
}

void USNJournalReader::WriteIndividualToFile() {
    for (size_t i = 0; i < outputFormats_.size(); ++i) {
        OutputFormat fmt = outputFormats_[i];
        std::string filename = (i < outputFiles_.size()) ? outputFiles_[i] : outputFiles_.back();
        if (fmt == OutputFormat::TXT && filename.find('.') == std::string::npos) filename += ".txt";
        else if (fmt == OutputFormat::CSV && filename.find('.') == std::string::npos) filename += ".csv";
        else if (fmt == OutputFormat::JSON && filename.find('.') == std::string::npos) filename += ".json";

        std::ofstream out(filename);
        if (!out) {
            std::wcerr << L"[-] Failed to open output file: " << std::wstring(filename.begin(), filename.end()) << L"\n";
            continue;
        }

        if (fmt == OutputFormat::TXT) {
            for (const auto& entry : entries_) {
                out << "Name: " << to_utf8(entry.name) << "\n";
                out << "Directory: " << to_utf8(entry.directory) << "\n";
                out << "File ID: " << FileIdToString(entry.fileId) << "\n";
                out << "USN: " << entry.usn << "\n";
                out << "Date: " << formatFileTime(entry.date) << "\n";
                out << "Reason: " << entry.reason << "\n";
                out << "---\n";
            }
        }
        else if (fmt == OutputFormat::CSV) {
            out << "Name,Directory,File ID,USN,Date,Reason\n";
            for (const auto& entry : entries_) {
                out << "\"" << to_utf8(entry.name) << "\",";
                out << "\"" << to_utf8(entry.directory) << "\",";
                out << "\"" << FileIdToString(entry.fileId) << "\",";
                out << entry.usn << ",";
                out << "\"" << formatFileTime(entry.date) << "\",";
                out << "\"" << entry.reason << "\"\n";
            }
        }
        else if (fmt == OutputFormat::JSON) {
            out << "[\n";
            for (size_t j = 0; j < entries_.size(); ++j) {
                const auto& entry = entries_[j];
                out << "  {\n";
                out << "    \"name\": \"" << to_utf8(entry.name) << "\",\n";
                out << "    \"directory\": \"" << to_utf8(entry.directory) << "\",\n";
                out << "    \"fileId\": \"" << FileIdToString(entry.fileId) << "\",\n";
                out << "    \"usn\": " << entry.usn << ",\n";
                out << "    \"date\": \"" << formatFileTime(entry.date) << "\",\n";
                out << "    \"reason\": \"" << entry.reason << "\"\n";
                out << "  }";
                if (j < entries_.size() - 1) out << ",";
                out << "\n";
            }
            out << "]\n";
        }
    }
}

void USNJournalReader::WriteIndividualToConsole() {
    for (size_t i = 0; i < outputFormats_.size(); ++i) {
        OutputFormat fmt = outputFormats_[i];
        if (fmt == OutputFormat::TXT) {
            for (const auto& entry : entries_) {
                std::cout << "Name: " << to_utf8(entry.name) << "\n";
                std::cout << "Directory: " << to_utf8(entry.directory) << "\n";
                std::cout << "File ID: " << FileIdToString(entry.fileId) << "\n";
                std::cout << "USN: " << entry.usn << "\n";
                std::cout << "Date: " << formatFileTime(entry.date) << "\n";
                std::cout << "Reason: " << entry.reason << "\n";
                std::cout << "---\n";
            }
        }
        else if (fmt == OutputFormat::CSV) {
            std::cout << "Name,Directory,File ID,USN,Date,Reason\n";
            for (const auto& entry : entries_) {
                std::cout << "\"" << to_utf8(entry.name) << "\",";
                std::cout << "\"" << to_utf8(entry.directory) << "\",";
                std::cout << "\"" << FileIdToString(entry.fileId) << "\",";
                std::cout << entry.usn << ",";
                std::cout << "\"" << formatFileTime(entry.date) << "\",";
                std::cout << "\"" << entry.reason << "\"\n";
            }
        }
        else if (fmt == OutputFormat::JSON) {
            std::cout << "[\n";
            for (size_t j = 0; j < entries_.size(); ++j) {
                const auto& entry = entries_[j];
                std::cout << "  {\n";
                std::cout << "    \"name\": \"" << to_utf8(entry.name) << "\",\n";
                std::cout << "    \"directory\": \"" << to_utf8(entry.directory) << "\",\n";
                std::cout << "    \"fileId\": \"" << FileIdToString(entry.fileId) << "\",\n";
                std::cout << "    \"usn\": " << entry.usn << ",\n";
                std::cout << "    \"date\": \"" << formatFileTime(entry.date) << "\",\n";
                std::cout << "    \"reason\": \"" << entry.reason << "\"\n";
                std::cout << "  }";
                if (j < entries_.size() - 1) std::cout << ",";
                std::cout << "\n";
            }
            std::cout << "]\n";
        }
    }
}

void USNJournalReader::WriteReplacesToFile() {
    auto agg = EventsFileID();
    size_t copyCount = 0, typeCount = 0, explorerCount = 0;

    if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::COPY) != detectReplaces_.end() ||
        std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
        copyCount = std::count_if(agg.begin(), agg.end(), [this](const AggregatedUSNEntry& a) { return IsCopyReplacement(a.events); });
    }
    if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::TYPE) != detectReplaces_.end() ||
        std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
        typeCount = std::count_if(agg.begin(), agg.end(), [this](const AggregatedUSNEntry& a) { return IsTypeReplacement(a.events); });
    }
    if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::EXPLORER) != detectReplaces_.end() ||
        std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
        auto allEntries = GetEntriesCopy();
        std::sort(allEntries.begin(), allEntries.end(),
            [](const USNEntry& a, const USNEntry& b) {
                return CompareFileTime(&a.date, &b.date) < 0;
            });
        for (size_t i = 0; i < allEntries.size(); ++i) {
            if (IsExplorerReplacement(allEntries, i)) {
                explorerCount++;
                i += 3;
            }
        }
    }

    for (const auto& fmt : outputFormats_) {
        std::string ext = GetExtension(fmt);
        if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::COPY) != detectReplaces_.end() ||
            std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
            std::string filename = "copy_replaces." + ext;
            std::ofstream out(filename);
            if (!out) {
                std::wcerr << L"[-] Failed to open copy_replaces file.\n";
                continue;
            }
            WriteReplacesHeader(out, fmt, "Copy", copyCount);
            size_t index = 0;
            for (const auto& a : agg) {
                if (IsCopyReplacement(a.events)) {
                    WriteReplaceEntry(out, fmt, a, "Copy", ++index == copyCount);
                }
            }
            if (fmt == OutputFormat::JSON) out << "}\n";
        }

        if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::TYPE) != detectReplaces_.end() ||
            std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
            std::string filename = "type_replaces." + ext;
            std::ofstream out(filename);
            if (!out) {
                std::wcerr << L"[-] Failed to open type_replaces file.\n";
                continue;
            }
            WriteReplacesHeader(out, fmt, "Type", typeCount);
            size_t index = 0;
            for (const auto& a : agg) {
                if (IsTypeReplacement(a.events)) {
                    WriteReplaceEntry(out, fmt, a, "Type", ++index == typeCount);
                }
            }
            if (fmt == OutputFormat::JSON) out << "}\n";
        }

        if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::EXPLORER) != detectReplaces_.end() ||
            std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
            std::string filename = "explorer_replaces." + ext;
            std::ofstream out(filename);
            if (!out) {
                std::wcerr << L"[-] Failed to open explorer_replaces file.\n";
                continue;
            }
            WriteReplacesHeader(out, fmt, "Explorer", explorerCount);
            auto allEntries = GetEntriesCopy();
            std::sort(allEntries.begin(), allEntries.end(),
                [](const USNEntry& a, const USNEntry& b) {
                    return CompareFileTime(&a.date, &b.date) < 0;
                });
            size_t index = 0;
            for (size_t i = 0; i < allEntries.size(); ++i) {
                if (IsExplorerReplacement(allEntries, i)) {
                    WriteExplorerReplaceEntry(out, fmt, allEntries, i, ++index == explorerCount);
                    i += 3;
                }
            }
            if (fmt == OutputFormat::JSON) out << "}\n";
        }
    }
}

void USNJournalReader::WriteReplacesToConsole() {
    auto agg = EventsFileID();
    size_t copyCount = 0, typeCount = 0, explorerCount = 0;

    if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::COPY) != detectReplaces_.end() ||
        std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
        copyCount = std::count_if(agg.begin(), agg.end(), [this](const AggregatedUSNEntry& a) { return IsCopyReplacement(a.events); });
    }
    if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::TYPE) != detectReplaces_.end() ||
        std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
        typeCount = std::count_if(agg.begin(), agg.end(), [this](const AggregatedUSNEntry& a) { return IsTypeReplacement(a.events); });
    }
    if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::EXPLORER) != detectReplaces_.end() ||
        std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
        auto allEntries = GetEntriesCopy();
        std::sort(allEntries.begin(), allEntries.end(),
            [](const USNEntry& a, const USNEntry& b) {
                return CompareFileTime(&a.date, &b.date) < 0;
            });
        for (size_t i = 0; i < allEntries.size(); ++i) {
            if (IsExplorerReplacement(allEntries, i)) {
                explorerCount++;
                i += 3;
            }
        }
    }

    for (const auto& fmt : outputFormats_) {
        if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::COPY) != detectReplaces_.end() ||
            std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
            WriteReplacesHeader(std::cout, fmt, "Copy", copyCount);
            size_t index = 0;
            for (const auto& a : agg) {
                if (IsCopyReplacement(a.events)) {
                    WriteReplaceEntry(std::cout, fmt, a, "Copy", ++index == copyCount);
                }
            }
            if (fmt == OutputFormat::JSON) std::cout << "}\n";  // Close JSON object
        }

        if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::TYPE) != detectReplaces_.end() ||
            std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
            WriteReplacesHeader(std::cout, fmt, "Type", typeCount);
            size_t index = 0;
            for (const auto& a : agg) {
                if (IsTypeReplacement(a.events)) {
                    WriteReplaceEntry(std::cout, fmt, a, "Type", ++index == typeCount);
                }
            }
            if (fmt == OutputFormat::JSON) std::cout << "}\n";
        }

        if (std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::EXPLORER) != detectReplaces_.end() ||
            std::find(detectReplaces_.begin(), detectReplaces_.end(), ReplaceType::ALL) != detectReplaces_.end()) {
            WriteReplacesHeader(std::cout, fmt, "Explorer", explorerCount);
            auto allEntries = GetEntriesCopy();
            std::sort(allEntries.begin(), allEntries.end(),
                [](const USNEntry& a, const USNEntry& b) {
                    return CompareFileTime(&a.date, &b.date) < 0;
                });
            size_t index = 0;
            for (size_t i = 0; i < allEntries.size(); ++i) {
                if (IsExplorerReplacement(allEntries, i)) {
                    WriteExplorerReplaceEntry(std::cout, fmt, allEntries, i, ++index == explorerCount);
                    i += 3;
                }
            }
            if (fmt == OutputFormat::JSON) std::cout << "}\n";
        }
    }
}

std::string USNJournalReader::GetExtension(OutputFormat fmt) const {
    switch (fmt) {
    case OutputFormat::TXT: return "txt";
    case OutputFormat::CSV: return "csv";
    case OutputFormat::JSON: return "json";
    default: return "txt";
    }
}

void USNJournalReader::WriteReplacesHeader(std::ostream& out, OutputFormat fmt, const std::string& type, size_t count) {
    if (fmt == OutputFormat::TXT) {
        out << "[+] " << type << " replacements detected: " << count << "\n\n";
    }
    else if (fmt == OutputFormat::CSV) {
        out << "Type,Name,Directory,File ID,Replace\n";
    }
    else if (fmt == OutputFormat::JSON) {
        out << "{\n  \"type\": \"" << type << "\",\n  \"count\": " << count << ",\n  \"entries\": [\n";
    }
}

void USNJournalReader::WriteReplaceEntry(std::ostream& out, OutputFormat fmt, const AggregatedUSNEntry& a, const std::string& replaceType, bool isLast) {
    if (fmt == OutputFormat::TXT) {
        out << "Name: " << to_utf8(a.name) << "\n";
        out << "Directory: " << to_utf8(a.directory) << "\n";
        out << "File ID: " << FileIdToString(a.fileId) << "\n";
        out << "Replace: " << replaceType << "\n";
        out << "Events:\n";
        for (const auto& e : a.events) {
            out << "  Date: " << formatFileTime(e.date) << " | Reason: " << e.reason
                << " | Directory: " << to_utf8(e.directory) << "\n";
        }
        out << "---\n";
    }
    else if (fmt == OutputFormat::CSV) {
        out << "\"" << replaceType << "\",";
        out << "\"" << to_utf8(a.name) << "\",";
        out << "\"" << to_utf8(a.directory) << "\",";
        out << "\"" << FileIdToString(a.fileId) << "\",";
        out << "\"" << replaceType << "\"\n";
    }
    else if (fmt == OutputFormat::JSON) {
        out << "    {\n";
        out << "      \"name\": \"" << to_utf8(a.name) << "\",\n";
        out << "      \"directory\": \"" << to_utf8(a.directory) << "\",\n";
        out << "      \"fileId\": \"" << FileIdToString(a.fileId) << "\",\n";
        out << "      \"replace\": \"" << replaceType << "\"\n";
        out << "    }";
        if (!isLast) out << ",";
        out << "\n";
    }
}

void USNJournalReader::WriteExplorerReplaceEntry(std::ostream& out, OutputFormat fmt, const std::vector<USNEntry>& allEntries, size_t startIndex, bool isLast) {
    const auto& lastEvent = allEntries[startIndex + 3];
    if (fmt == OutputFormat::TXT) {
        out << "Name: " << to_utf8(lastEvent.name) << "\n";
        out << "Directory: " << to_utf8(lastEvent.directory) << "\n";
        out << "Replace: Explorer\n";
        out << "Events:\n";
        for (size_t j = 0; j < 4; ++j) {
            const auto& e = allEntries[startIndex + j];
            out << "  Date: " << formatFileTime(e.date) << " | Reason: " << e.reason
                << " | Directory: " << to_utf8(e.directory) << "\n";
        }
        out << "---\n";
    }
    else if (fmt == OutputFormat::CSV) {
        out << "\"Explorer\",";
        out << "\"" << to_utf8(lastEvent.name) << "\",";
        out << "\"" << to_utf8(lastEvent.directory) << "\",";
        out << "\"\",";
        out << "\"Explorer\"\n";
    }
    else if (fmt == OutputFormat::JSON) {
        out << "    {\n";
        out << "      \"name\": \"" << to_utf8(lastEvent.name) << "\",\n";
        out << "      \"directory\": \"" << to_utf8(lastEvent.directory) << "\",\n";
        out << "      \"replace\": \"Explorer\"\n";
        out << "    }";
        if (!isLast) out << ",";
        out << "\n";
    }
}