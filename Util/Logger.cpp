#include "Logger.h"

bool Logger::debugEnabled = false;
bool Logger::useUI = false;
std::deque<std::string> Logger::recentLogs;
std::mutex Logger::recentLogsMtx;
std::mutex Logger::consoleMtx;
size_t Logger::maxLogs = 200;
Logger::Level Logger::level = Logger::LVL_INFO;

std::vector<std::string> Logger::getRecentLogs() {
	std::lock_guard<std::mutex> lk(recentLogsMtx);
	return std::vector<std::string>(recentLogs.begin(), recentLogs.end());
}
