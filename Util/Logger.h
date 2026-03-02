#ifndef LOGGER_H
#define LOGGER_H

#include <iostream>
#include <string>
#include <ctime>
#include <iomanip>

class Logger {
public:
    static bool debugEnabled;

    static void info(const std::string& msg) {
        log("INFO", msg);
    }

    static void warn(const std::string& msg) {
        log("WARN", msg);
    }

    static void error(const std::string& msg) {
        log("ERROR", msg);
    }

    static void debug(const std::string& msg) {
        if (debugEnabled) log("DEBUG", msg);
    }

private:
    static void log(const std::string& level, const std::string& msg) {
        auto now = std::time(nullptr);
        auto* tm = std::localtime(&now);
        if (tm) {
            std::cout << "[" << std::put_time(tm, "%H:%M:%S") << "] ";
        } else {
            std::cout << "[??:??:??] ";
        }
        std::cout << "[" << level << "] " << msg << std::endl;
    }
};

#endif
