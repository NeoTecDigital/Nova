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

/**
 * Log lines the output stream refused since process start.
 *
 * report() never throws and never aborts - a full disk must not become a crash
 * - so a write that fails is counted here and announced once on stderr. Any
 * non-zero value means the log on stdout is incomplete, and a caller that cares
 * about that (a test harness grading a log, an e2e gate) can say so.
 */
unsigned long loggerDroppedLines();
