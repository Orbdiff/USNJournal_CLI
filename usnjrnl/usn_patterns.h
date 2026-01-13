#pragma once

#include <vector>
#include <string>

static const std::vector<std::vector<std::string>> COPY_PATTERN_1 = {
    {"Data Truncation", "Security Change"},
    {"Data Extend", "Data Truncation", "Security Change"},
    {"Data Overwrite", "Data Extend", "Data Truncation", "Security Change"},
    {"Data Overwrite", "Data Extend", "Data Truncation", "Security Change", "Basic Info Change"},
    {"Data Overwrite", "Data Extend", "Data Truncation", "Security Change", "Basic Info Change", "Close"}
};

static const std::vector<std::vector<std::string>> COPY_PATTERN_2 = {
    {"Data Truncation"},
    {"Data Extend", "Data Truncation"},
    {"Data Overwrite", "Data Extend", "Data Truncation"},
    {"Data Overwrite", "Data Extend", "Data Truncation", "Basic Info Change"},
    {"Data Overwrite", "Data Extend", "Data Truncation", "Basic Info Change", "Close"}
};

static const std::vector<std::vector<std::string>> EXPLORER_PATTERN = {
    {"File Delete", "Close"},
    {"Rename Old Name"},
    {"Rename New Name"},
    {"Rename New Name", "Close"}
};

static const std::vector<std::vector<std::string>> TYPE_PATTERN_1 = {
    {"Data Extend", "Data Truncation"},
    {"Data Extend", "Data Truncation", "Close"}
};

static const std::vector<std::vector<std::string>> TYPE_PATTERN_2 = {
    {"Data Truncation"},
    {"Data Extend", "Data Truncation"}
};

// echo replace uses the same pattern as type so yeah no is needed!