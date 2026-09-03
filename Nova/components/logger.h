#pragma once
#include <string>

// Ordinals are severity thresholds, not identifiers: report() emits a message when
// its level is <= LOG_LEVEL, so lower ordinal == higher severity. WARN sits directly
// below ERROR, which makes it visible from STAGING upward and silent in RELEASE
// (RELEASE is defined as critical user-facing errors only -- see debug_level.h).
// The *LINE variants are continuation lines for the severity that precedes them.
enum LOGGER {
    OFF,
    ERROR,
    WARN,
    ILINE,
    INFO,
    DLINE,
    DEBUG,
    VLINE,
    VERBOSE
};

inline LOGGER LOG_LEVEL = LOGGER::VERBOSE;

void report(LOGGER log_level, const char* message, ...);
void setLogLevel(const char* debug_level);
