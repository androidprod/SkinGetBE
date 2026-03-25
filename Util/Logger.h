#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>
#include <ctime>
#include <iomanip>
#include <deque>
#include <mutex>
#include <vector>

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
    // When true, Logger will not write directly to stdout but instead stash recent
    // log lines into an internal buffer for the terminal UI to render.
    static bool useUI;
    enum Level { LVL_ERROR = 0, LVL_INFO = 1, LVL_VERBOSE = 2, LVL_DEBUG = 3 };
    static Level level;

    // Retrieve a snapshot of recent logs (thread-safe).
    static std::vector<std::string> getRecentLogs();

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
        if (level >= LVL_INFO) log("\033[96mINFO\033[0m", msg);
    }

    static void warn(const std::string& msg) {
        if (level >= LVL_INFO) log("\033[93mWARN\033[0m", msg);
    }

    static void error(const std::string& msg) {
        log("\033[91mERROR\033[0m", msg);
    }

    static void debug(const std::string& msg) {
        if (debugEnabled || level == LVL_DEBUG) log("\033[90mDEBUG\033[0m", msg);
    }

    static void success(const std::string& msg) {
        if (level >= LVL_INFO) log("\033[92mSUCCESS\033[0m", msg);
    }

    static void setLevel(Level l) { level = l; }

private:
    static std::deque<std::string> recentLogs;
    static std::mutex recentLogsMtx;
    static std::mutex consoleMtx;
    static size_t maxLogs;

    static void log(const std::string& level, const std::string& msg) {
        auto now = std::time(nullptr);
        auto* tm = std::localtime(&now);
        char timebuf[16] = "??:??:??";
        if (tm) std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);

        std::string line = std::string("[") + timebuf + "] [" + level + "] " + msg;

        // Always keep recent logs in buffer for UI consumers
        {
            std::lock_guard<std::mutex> lk(recentLogsMtx);
            recentLogs.push_back(line);
            while (recentLogs.size() > maxLogs) recentLogs.pop_front();
        }

        // If UI is active, avoid printing directly to stdout (UI will render)
        if (useUI) return;

        std::lock_guard<std::mutex> coutLock(consoleMtx);
        std::cout << "\033[90m[" << timebuf << "]\033[0m [" << level << "] " << msg << std::endl;
    }
};

#endif
