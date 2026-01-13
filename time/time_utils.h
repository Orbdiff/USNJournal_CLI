#pragma once

#include <windows.h>
#include <ntsecapi.h>
#include <ctime>
#include <iostream>
#include <iomanip>

inline time_t FileTimeUtcToLocalTimeT(const FILETIME& utcFt)
{
    FILETIME localFt{};
    SYSTEMTIME stLocal{};

    if (!FileTimeToLocalFileTime(&utcFt, &localFt))
        return 0;

    if (!FileTimeToSystemTime(&localFt, &stLocal))
        return 0;

    tm t{};
    t.tm_year = stLocal.wYear - 1900;
    t.tm_mon = stLocal.wMonth - 1;
    t.tm_mday = stLocal.wDay;
    t.tm_hour = stLocal.wHour;
    t.tm_min = stLocal.wMinute;
    t.tm_sec = stLocal.wSecond;
    t.tm_isdst = -1;

    return mktime(&t);
}

inline time_t LocalFileTimeToTimeT(const FILETIME& localFt)
{
    SYSTEMTIME stLocal{};
    if (!FileTimeToSystemTime(&localFt, &stLocal))
        return 0;

    tm t{};
    t.tm_year = stLocal.wYear - 1900;
    t.tm_mon = stLocal.wMonth - 1;
    t.tm_mday = stLocal.wDay;
    t.tm_hour = stLocal.wHour;
    t.tm_min = stLocal.wMinute;
    t.tm_sec = stLocal.wSecond;
    t.tm_isdst = -1;

    return mktime(&t); 
}

inline time_t GetCurrentUserLogonTime()
{
    wchar_t username[256];
    DWORD size = ARRAYSIZE(username);

    if (!GetUserNameW(username, &size))
        return 0;

    ULONG count = 0;
    PLUID sessions = nullptr;

    if (LsaEnumerateLogonSessions(&count, &sessions) != 0)
        return 0;

    time_t result = 0;

    for (ULONG i = 0; i < count; i++)
    {
        PSECURITY_LOGON_SESSION_DATA data = nullptr;

        if (LsaGetLogonSessionData(&sessions[i], &data) == 0 && data)
        {
            if (data->UserName.Buffer &&
                data->LogonType == Interactive &&
                _wcsicmp(data->UserName.Buffer, username) == 0)
            {
                FILETIME utcFt{
                    static_cast<DWORD>(data->LogonTime.LowPart),
                    static_cast<DWORD>(data->LogonTime.HighPart)
                };

                result = FileTimeUtcToLocalTimeT(utcFt);

                LsaFreeReturnBuffer(data);
                break;
            }
            LsaFreeReturnBuffer(data);
        }
    }

    if (sessions)
        LsaFreeReturnBuffer(sessions);

    return result;
}

inline void print_time(time_t t)
{
    if (t == 0) return;

    tm timeinfo{};
    localtime_s(&timeinfo, &t);

    std::cout << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "\n";
}