// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/**
 * The assertion vocabulary every in-repo integration harness shares.
 *
 * Deliberately NOT <cassert>: these binaries are compiled in whatever
 * configuration the tree is configured in, and a RelWithDebInfo build defines
 * NDEBUG, which would delete every assert() and leave a test that passes by
 * being empty. A test that cannot fail is worse than no test.
 *
 * Every check prints one line, counts itself, and never throws: a harness that
 * aborts on its first failure reports one defect per run instead of all of them.
 * The process exit code is the gate - non-zero whenever any check failed - which
 * is what CTest reads.
 */
namespace VazioTest {

class CheckLog {
public:
    explicit CheckLog(std::string label) : label_(std::move(label)) {}

    bool check(bool ok, const char* fmt, ...) __attribute__((format(printf, 3, 4))) {
        char message[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);

        ++checks_;
        if (!ok) ++failures_;
        // stderr for both, unbuffered by default: mixing an ok stream on stdout
        // with a failure stream on stderr reorders the log the moment it is
        // redirected, and the crash point then reads several checks early.
        fprintf(stderr, "[%s] %s: %s\n", ok ? " ok " : "FAIL", label_.c_str(), message);
        fflush(stderr);
        return ok;
    }

    void note(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char message[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(message, sizeof(message), fmt, args);
        va_end(args);
        fprintf(stderr, "[note] %s: %s\n", label_.c_str(), message);
        fflush(stderr);
    }

    int report() const {
        fprintf(stderr, "\n=== %s: %d/%d checks passed, %d failures ===\n",
                label_.c_str(), checks_ - failures_, checks_, failures_);
        fflush(stderr);
        return failures_ == 0 ? 0 : 1;
    }

    int checks() const { return checks_; }
    int failures() const { return failures_; }
    void absorb(int checks, int failures) { checks_ += checks; failures_ += failures; }

private:
    std::string label_;
    int checks_ = 0;
    int failures_ = 0;
};

/**
 * Negative-control selector, read once from the environment.
 *
 * Each harness documents its own modes. A mode deliberately breaks one input to
 * the system under test so the run MUST fail; running the harness under a mode
 * and seeing it stay green means the assertions are decorative. This is the only
 * way an in-repo test proves it can detect anything at all.
 */
inline bool negativeControl(const char* mode) {
    const char* selected = getenv("VAZIO_TEST_NEG");
    return selected != nullptr && strcmp(selected, mode) == 0;
}

inline const char* negativeControlName() {
    const char* selected = getenv("VAZIO_TEST_NEG");
    return selected != nullptr ? selected : "none";
}

}  // namespace VazioTest
