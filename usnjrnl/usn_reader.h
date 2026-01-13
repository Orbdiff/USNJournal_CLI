#pragma once

#include "usn_structs.h"
#include "usn_patterns.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>

class USNJournalReader {
public:
    USNJournalReader(const std::wstring& volumeLetter);

    bool filterAfterLogon_ = false;
    time_t logonTime_ = 0;
    bool filterAfterDate_ = false;
    time_t filterDate_ = 0;
    std::vector<std::string> filterNames_;
    std::vector<std::string> filterReasons_;
    std::vector<std::string> filterIds_;
    std::vector<std::string> filterPaths_;
    bool filterPathRecursive_ = false;
    std::vector<ReplaceType> detectReplaces_;
    std::vector<OutputFormat> outputFormats_ = { OutputFormat::TXT };
    std::vector<std::string> outputFiles_ = { "usnjrnl.txt" };
    bool consoleOutput_ = false;
    bool onlyReplace_ = false;

    void Run();
    std::vector<USNEntry> GetEntriesCopy();
    std::vector<AggregatedUSNEntry> EventsFileID();
    void EnableAfterLogonFilter(time_t logonTime);

private:
    std::wstring volumeLetter_;
    HANDLE volumeHandle_ = INVALID_HANDLE_VALUE;
    std::unique_ptr<BYTE[]> buffer_;
    USN_JOURNAL_DATA_V0 journalData_{};
    std::unordered_map<FileIdVariant, std::wstring, FileIdHash, FileIdEqual> pathCache_;
    std::mutex cacheMutex_;
    std::mutex entriesMutex_;
    std::vector<USNEntry> entries_;

    std::string FileIdToString(const FileIdVariant& fid);
    bool Dump();
    bool OpenVolume();
    bool QueryJournal();
    bool AllocateBuffer();
    std::wstring GetDirectoryById(ULONGLONG fileId);
    std::wstring GetDirectoryById(const FILE_ID_128& fileId128);
    std::string ReasonToString(DWORD reason) const;
    bool CheckPatternSequential(const std::vector<std::string>& window, const std::vector<std::vector<std::string>>& pattern);
    bool IsCopyReplacement(const std::vector<FileEvent>& events);
    bool IsTypeReplacement(const std::vector<FileEvent>& events);
    bool IsExplorerReplacement(const std::vector<USNEntry>& orderedEntries, size_t startIndex);
    void PushEntry(FileIdVariant fileId, ULONGLONG usn, const std::wstring& name, const FILETIME& date, const std::string& reason, const std::wstring& dir);
    void Cleanup();

    void WriteIndividualToFile();
    void WriteIndividualToConsole();
    void WriteReplacesToFile();
    void WriteReplacesToConsole();
    std::string GetExtension(OutputFormat fmt) const;
    void WriteReplacesHeader(std::ostream& out, OutputFormat fmt, const std::string& type, size_t count);
    void WriteReplaceEntry(std::ostream& out, OutputFormat fmt, const AggregatedUSNEntry& a, const std::string& replaceType, bool isLast);
    void WriteExplorerReplaceEntry(std::ostream& out, OutputFormat fmt, const std::vector<USNEntry>& allEntries, size_t startIndex, bool isLast);
};