#pragma once
#include <cstdio>
#include <string>
#include <unordered_map>

// Everything defined here is `inline`. These are definitions in a header, so
// without it a second translation unit including this file is a duplicate-symbol
// link error -- it links today only because exactly one TU (logger.cpp) includes
// it, which is a property of the current file layout and not of this header.


enum DEBUG_LEVEL {
    SILENT,             // No output
    RELEASE,            // Would only be Critical User-facing errors
    STAGING,            // Would typically be status level updates for other testers
    DEV,                // Would be standard debug level information meant for other developers
    LOUD,               // Maximum debug level information for personal debugging and benchmarking
    INVALID             // Invalid debug level
};

inline const std::unordered_map<std::string, DEBUG_LEVEL> DEBUG_MAP = {
        { "none", DEBUG_LEVEL::SILENT },
        { "release", DEBUG_LEVEL::RELEASE },
        { "staging", DEBUG_LEVEL::STAGING },
        { "development", DEBUG_LEVEL::DEV },
        { "debug", DEBUG_LEVEL::LOUD }
};

// Adds a safety check to ensure that the debug level is set to a valid value.
inline DEBUG_LEVEL getDebugLevel (const char* dbg_lvl) {
    // Terminated and flushed: without the newline this line had no end of its
    // own and glued itself to the front of whatever the logger printed next,
    // and without the flush it sat in stdout's buffer until something else
    // happened to flush it - which, at SILENT, is process exit.
    printf("Logger - Debug Level set to %s ..\n", dbg_lvl);
    fflush(stdout);
    if (DEBUG_MAP.find(dbg_lvl) != DEBUG_MAP.end()) 
        { return DEBUG_MAP.at(dbg_lvl); } 
    else 
        { return DEBUG_LEVEL::INVALID; }
};

// This is a helper function to convert the DEBUG_LEVEL enum to a string for output.
inline const char* debugString (DEBUG_LEVEL dbg_lvl) {
    switch (dbg_lvl) {
        case DEBUG_LEVEL::SILENT:
            return "none";
        case DEBUG_LEVEL::RELEASE:
            return "release";
        case DEBUG_LEVEL::STAGING:
            return "staging";
        case DEBUG_LEVEL::DEV:
            return "development";
        case DEBUG_LEVEL::LOUD:
            return "debug";
        default:
            return "invalid";
    }
};