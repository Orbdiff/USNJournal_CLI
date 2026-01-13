#include "usn_reader.h"
#include "usn_utils.h"
#include "time_utils.h"
#include "privilege.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <sstream>

int main(int argc, char* argv[]) {

    if (!EnableDebugPrivilege()) {
        std::wcerr << L"[!] Failed to enable SeDebugPrivilege (might require admin)\n";
    }

    if (argc < 2) {
        std::cout <<
            "Usage:\n"
            "  " << argv[0] << " <VOLUME> [OPTIONS]\n\n"

            "Volume:\n"
            "  C:            Scan the entire USN Journal of volume C:\n\n"

            "Time filters:\n"
            "  -L            Show entries after current user logon\n"
            "  -A <DATE>     Show entries after date (YYYY-MM-DD HH:MM:SS)\n\n"

            "File filters:\n"
            "  -n <names>    Filter by file name(s)   (e.g. test.exe;cmd.dll)\n"
            "  -r <reasons>  Filter by USN reason(s)  (e.g. File Create;Overwrite)\n"
            "  -i <ids>      Filter by File ID(s)\n"
            "  -p <paths>    Filter by path(s)\n"
            "  -R            Recursive path filtering\n\n"

            "Replace detection:\n"
            "  -x <types>    Detect replace patterns: (copy;type;explorer;all)\n"
            "  --only-replace  Show ONLY replace results (no full journal)\n\n"

            "Output:\n"
            "  -f <formats>  Output format(s): txt;csv;json\n"
            "  -o <files>    Output file name(s)\n"
            "  -c            Print results to console\n\n"

            "Other:\n"
            "  -h            Show this help\n\n"

            "Examples:\n"
            "  Detect all replaces only:\n"
            "    " << argv[0] << " C: -L -x all --only-replace\n\n"
            "  Show entries after logon, filtered by file names, and export to CSV:\n"
            "    " << argv[0] << " C: -L -n test.exe; cmd.dll -f csv -o results.csv\n\n"
            "  Filter by path recursively and output to JSON:\n"
            "    " << argv[0] << " C: -p C:\Users -R -f json -o journal.json\n\n";

        return 0;
    }

    std::string volStr(argv[1]);
    std::wstring volume(volStr.begin(), volStr.end());
    USNJournalReader reader(volume);

    std::vector<std::string> outputFiles = { "usnjrnl.txt" };
    bool consoleOutput = false;

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-L") {
            time_t logonTime = GetCurrentUserLogonTime();
            if (logonTime) {
                std::cout << "[+] User logon time: ";
                print_time(logonTime);
                reader.EnableAfterLogonFilter(logonTime);
            }
            else {
                std::cerr << "[-] Failed to get logon time\n";
            }
        }
        else if (arg == "-A" && i + 1 < argc) {
            time_t date = parseDateTime(argv[++i]);
            if (!date) {
                std::cerr << "[-] Invalid date format\n";
                return 1;
            }
            reader.filterAfterDate_ = true;
            reader.filterDate_ = date;
        }
        else if (arg == "-n" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';'))
                reader.filterNames_.push_back(tok);
        }
        else if (arg == "-r" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';'))
                reader.filterReasons_.push_back(tok);
        }
        else if (arg == "-i" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';'))
                reader.filterIds_.push_back(tok);
        }
        else if (arg == "-p" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';'))
                reader.filterPaths_.push_back(tok);
        }
        else if (arg == "-R") {
            reader.filterPathRecursive_ = true;
        }
        else if (arg == "-x" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';')) {
                if (tok == "copy") reader.detectReplaces_.push_back(ReplaceType::COPY);
                else if (tok == "type") reader.detectReplaces_.push_back(ReplaceType::TYPE);
                else if (tok == "explorer") reader.detectReplaces_.push_back(ReplaceType::EXPLORER);
                else if (tok == "all") reader.detectReplaces_.push_back(ReplaceType::ALL);
                else {
                    std::cerr << "[-] Invalid replace type: " << tok << "\n";
                    return 1;
                }
            }
        }
        else if (arg == "--only-replace") {
            reader.onlyReplace_ = true;
        }
        else if (arg == "-f" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';')) {
                if (tok == "txt") reader.outputFormats_.push_back(OutputFormat::TXT);
                else if (tok == "csv") reader.outputFormats_.push_back(OutputFormat::CSV);
                else if (tok == "json") reader.outputFormats_.push_back(OutputFormat::JSON);
                else {
                    std::cerr << "[-] Invalid output format: " << tok << "\n";
                    return 1;
                }
            }
        }
        else if (arg == "-o" && i + 1 < argc) {
            outputFiles.clear();
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ';'))
                outputFiles.push_back(tok);
        }
        else if (arg == "-c") {
            consoleOutput = true;
        }
        else if (arg == "-h") {
            argc = 1;
            return main(argc, argv);
        }
        else {
            std::cerr << "[-] Unknown option: " << arg << "\n";
            return 1;
        }
    }

    reader.outputFiles_ = outputFiles;
    reader.consoleOutput_ = consoleOutput;

    reader.Run();
    return 0;
}