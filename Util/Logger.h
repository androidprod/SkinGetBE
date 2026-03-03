#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>
#include <ctime>
#include <iomanip>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#endif

class Logger {
public:
    static bool debugEnabled;

    static void init() {
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
#endif
    }

    static void info(const std::string& msg) {
        log("\033[96mINFO\033[0m", msg);
    }

    static void warn(const std::string& msg) {
        log("\033[93mWARN\033[0m", msg);
    }

    static void error(const std::string& msg) {
        log("\033[91mERROR\033[0m", msg);
    }

    static void debug(const std::string& msg) {
        if (debugEnabled) log("\033[90mDEBUG\033[0m", msg);
    }

    static void success(const std::string& msg) {
        log("\033[92mSUCCESS\033[0m", msg);
    }

private:
    static void log(const std::string& level, const std::string& msg) {
        auto now = std::time(nullptr);
        auto* tm = std::localtime(&now);
        
        std::cout << "\033[90m[";
        if (tm) {
            std::cout << std::put_time(tm, "%H:%M:%S");
        } else {
            std::cout << "??:??:??";
        }
        std::cout << "]\033[0m [" << level << "] " << msg << std::endl;
    }
};

#endif
