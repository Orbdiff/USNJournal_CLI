# Journal_CLI

**Journal_CLI** was created primarily to help other tools use usnjrnl easily. It can detect **replacements (type, copy, explorer)** and **filtering after logon time**, etc.

## Use

```cpp
Journal_CLI.exe <VOLUME> [OPTIONS]

Options:

"Volume : <C:> Scan the entire USN Journal of volume C:"

-h : help with examples uses
-L : Show entries after current user logon
-A <DATE> : Show entries after date (YYYY-MM-DD HH:MM:SS)
-n <names> : Filter by file name(s)   (test.exe;cmd.dll)
-r <reasons> : Filter by USN reason(s)  (File Create;Overwrite)
-i <ids>   :   Filter by File ID(s)
-p <paths> : Filter by path(s)
-R : Recursive path filtering
-x <types> : Detect replace patterns: (copy;type;explorer;all)
--only-replace : Show ONLY replace results (no full journal)
-f <formats> : Output format(s): txt;csv;json
-o <files> : Output file name(s)
-c : Print results to console