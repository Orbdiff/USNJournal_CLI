#pragma once

#include <string>
#include <Windows.h>
#include <ctime>
#include <sstream>
#include <iomanip>

std::string to_utf8(const std::wstring& wstr);
std::string formatFileTime(const FILETIME& ft);
time_t parseDateTime(const std::string& datetimeStr);