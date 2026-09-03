// Written by Richard Christopher, Copyright 2026 NeoTec Digital
//
// One log line is one buffer, one fwrite, one flush, and one checked result.
//
// It used to be three unsynchronised operations - a std::cout prefix, a C
// vprintf body, a std::endl - with no result checked anywhere. That emitted a
// line as a single write() only because sync_with_stdio happens to be at its
// default and because the line happened to fit inside stdout's block; past that
// the body flushed mid-line and an interleaving writer could split it. Worse,
// a failed write dropped the line in silence and left the process exiting 0,
// which is the shape of a log that lies about what ran.
//
// Failure policy: count it, say so ONCE on stderr, keep going. A logger that
// throws or aborts turns a full disk into a crash, which is strictly worse than
// a short log; loggerDroppedLines() is how a caller finds out.

#include "logger.h"
#include "./debug_level.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

/**
 * Bytes one rendered line may occupy - prefix, body, marker and '\n' included.
 *
 * 2 KiB, for two reasons that both have to hold:
 *
 *  - It is ~15x the longest line this tree has been observed to emit. Measured
 *    at LOG_LEVEL=VERBOSE across a full `vazio` headless run and every probe:
 *    134 bytes, "SpatialPresentLoop - Presentation path: ...".
 *  - It stays well inside the 4096-byte block glibc gives stdout for a pipe, a
 *    regular file or a tmpfs file. A line that fits in stdio's buffer is copied
 *    there whole and leaves as ONE write() on the following flush; a line that
 *    exceeds it is split by stdio itself, and no amount of care here would put
 *    it back together. The cap is what makes "one line, one write" a property
 *    rather than a coincidence.
 *
 * It lives on the stack of each report() call, so there is no shared buffer to
 * serialise and two threads logging at once cannot overwrite each other.
 */
constexpr size_t LOG_LINE_MAX = 2048;

// Appended in place of the bytes that did not fit. Visible on purpose: a line
// that was cut has to say so, or the reader silently believes a truncated fact.
constexpr char TRUNCATION_MARKER[] = " ..[log line truncated]";
constexpr size_t TRUNCATION_MARKER_LEN = sizeof(TRUNCATION_MARKER) - 1;

// Substituted when vsnprintf itself fails - an output encoding error is the
// only way it can. The level and the fact of the failure still reach the log.
constexpr char FORMAT_FAILURE_BODY[] = "<unformattable log message>";
constexpr size_t FORMAT_FAILURE_BODY_LEN = sizeof(FORMAT_FAILURE_BODY) - 1;

/**
 * Lines the stream refused, and whether the single notice has gone out.
 *
 * Atomic because report() is callable from any thread and neither counter may
 * be torn. Relaxed ordering throughout: these are a tally and a one-shot latch,
 * and nothing is published through them.
 */
std::atomic<unsigned long> g_dropped_lines{0};
std::atomic<bool> g_drop_notice_sent{false};

// The severity prefixes, byte for byte what report() has always emitted. The
// *LINE variants are continuation lines and carry the indent instead.
const char* levelPrefix(LOGGER level)
{
    switch (level) {
        case LOGGER::ILINE:
        case LOGGER::DLINE:
        case LOGGER::VLINE:
            return " \t ";
        case LOGGER::ERROR:
            return " [ERROR]: ";
        case LOGGER::WARN:
            return " [WARN]: ";
        case LOGGER::INFO:
            return " [INFO]: ";
        case LOGGER::DEBUG:
            return " [DEBUG]: ";
        case LOGGER::VERBOSE:
            return " [VERBOSE]: ";
        default:
            return "";
    }
}

bool letsGo(LOGGER level)
{
    return level <= LOG_LEVEL;
}

/**
 * Render one complete line - prefix, body, newline - into `line`.
 *
 * Returns its length, always >= 1 and always ending in '\n'. The body's room is
 * computed with the marker and the newline already reserved, so truncation can
 * never cost the line its terminator and nothing is ever written past
 * LOG_LINE_MAX. `line` must have LOG_LINE_MAX bytes.
 */
size_t buildLine(char* line, LOGGER level, const char* format, std::va_list args)
{
    const char* prefix = levelPrefix(level);
    const size_t prefix_len = std::strlen(prefix);
    std::memcpy(line, prefix, prefix_len);

    // -1 for the '\n'; vsnprintf's own NUL is the +1 on the size it is given.
    const size_t body_room = LOG_LINE_MAX - prefix_len - TRUNCATION_MARKER_LEN - 1;
    const int wanted = std::vsnprintf(line + prefix_len, body_room + 1, format, args);

    size_t length = prefix_len;
    if (wanted < 0) {
        std::memcpy(line + length, FORMAT_FAILURE_BODY, FORMAT_FAILURE_BODY_LEN);
        length += FORMAT_FAILURE_BODY_LEN;
    } else if (static_cast<size_t>(wanted) > body_room) {
        std::memcpy(line + length + body_room, TRUNCATION_MARKER, TRUNCATION_MARKER_LEN);
        length += body_room + TRUNCATION_MARKER_LEN;
    } else {
        length += static_cast<size_t>(wanted);
    }

    line[length++] = '\n';

    return length;
}

/**
 * Record a line the stream would not take, and announce the condition once.
 *
 * No throw, no abort, no retry loop: the caller is mid-report and has no way to
 * handle any of the three. The notice goes to stderr because stdout is the
 * thing that just failed, and stderr is unbuffered, so it is one write with no
 * flush to check. Its own failure is unreportable by construction.
 */
void noteDroppedLine(std::FILE* stream)
{
    g_dropped_lines.fetch_add(1, std::memory_order_relaxed);

    // stdio's error flag is sticky. Cleared so a transient ENOSPC that later
    // resolves does not condemn every remaining line of the run.
    std::clearerr(stream);

    if (g_drop_notice_sent.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    static constexpr char NOTICE[] =
        " [ERROR]: logger: the log stream refused a write; lines are being dropped\n";
    const size_t written = std::fwrite(NOTICE, 1, sizeof(NOTICE) - 1, stderr);
    (void)written;   // nowhere left to report a failure to
}

/**
 * The whole line, in one operation, with the result actually looked at.
 *
 * The flush is what makes the write happen now rather than at the next 4 KiB
 * boundary - the previous code got it from std::endl, and the strace signature
 * of one write per line depends on it. ferror() is checked as well as the two
 * return values because a partial write can be reported through the flag alone.
 */
void emitLine(const char* line, size_t length)
{
    std::FILE* stream = stdout;

    if (std::fwrite(line, 1, length, stream) != length ||
        std::fflush(stream) != 0 ||
        std::ferror(stream) != 0) {
        noteDroppedLine(stream);
    }
}

LOGGER getLogLevel(const char* dbg_lvl)
{
    switch (getDebugLevel(dbg_lvl)) {
        case DEBUG_LEVEL::SILENT:
            return LOGGER::OFF;
        case DEBUG_LEVEL::RELEASE:
            return LOGGER::ERROR;
        case DEBUG_LEVEL::STAGING:
            return LOGGER::INFO;
        case DEBUG_LEVEL::DEV:
            return LOGGER::DEBUG;
        case DEBUG_LEVEL::LOUD:
            return LOGGER::VERBOSE;
        default:
            return LOGGER::OFF;
    }
}

DEBUG_LEVEL getDebugLevel(LOGGER log_level)
{
    switch (log_level) {
        case LOGGER::OFF:
            return DEBUG_LEVEL::SILENT;
        case LOGGER::ERROR:
            return DEBUG_LEVEL::RELEASE;
        case LOGGER::WARN:
            // Lowest configured level at which a WARN message is emitted.
            return DEBUG_LEVEL::STAGING;
        case LOGGER::INFO:
            return DEBUG_LEVEL::STAGING;
        case LOGGER::DEBUG:
            return DEBUG_LEVEL::DEV;
        case LOGGER::VERBOSE:
            return DEBUG_LEVEL::LOUD;
        default:
            return DEBUG_LEVEL::INVALID;
    }
}

} // namespace

void setLogLevel(const char* debug_level)
{
    LOG_LEVEL = getLogLevel(debug_level);
    report(LOGGER::INFO, "Logger - Debug Level set to %s ..", debugString(getDebugLevel(LOG_LEVEL)));
}

unsigned long loggerDroppedLines()
{
    return g_dropped_lines.load(std::memory_order_relaxed);
}

void report(LOGGER log_level, const char* format, ...)
{
    if (!letsGo(log_level)) {
        return;
    }

    char line[LOG_LINE_MAX];

    std::va_list args;
    va_start(args, format);
    const size_t length = buildLine(line, log_level, format, args);
    va_end(args);

    emitLine(line, length);
}
