#include "Version.h"
#include <map>
#include <string>
#include <mutex>
#include <unordered_set>
#include <cstdio>
#include "../Util/Logger.h"

namespace Bedrock {

static std::map<uint32_t, std::string> PROTOCOL_MAP = {
    {70,  "0.14.3"},
    {82,  "0.15.6"},
    {100, "1.0.0"},
    {422, "1.16.201"},
    {428, "1.16.210"},
    {431, "1.16.220"},
    {440, "1.17.0"},
    {448, "1.17.10"},
    {465, "1.17.30"},
    {471, "1.17.40"},
    {475, "1.18.0"},
    {486, "1.18.11"},
    {503, "1.18.30"},
    {527, "1.19.1"},
    {534, "1.19.10"},
    {544, "1.19.20"},
    {545, "1.19.21"},
    {554, "1.19.30"},
    {557, "1.19.40"},
    {560, "1.19.50"},
    {567, "1.19.60"},
    {568, "1.19.63"},
    {575, "1.19.70"},
    {582, "1.19.80"},
    {589, "1.20.0"},
    {594, "1.20.10"},
    {618, "1.20.30"},
    {622, "1.20.40"},
    {630, "1.20.50"},
    {649, "1.20.61"},
    {662, "1.20.71"},
    {671, "1.20.80"},
    {685, "1.21.0"},
    {686, "1.21.2"},
    {712, "1.21.20"},
    {729, "1.21.30"},
    {748, "1.21.42"},
    {766, "1.21.50"},
    {776, "1.21.60"},
    {786, "1.21.70"},
    {800, "1.21.80"},
    {818, "1.21.90"},
    {819, "1.21.93"},
    {827, "1.21.100"},
    {844, "1.21.111"},
    {859, "1.21.120"},
    {860, "1.21.124"},
    {898, "1.21.130"},
    {924, "1.26.0"},
    {944, "1.26.10"},
};

static std::string fallbackVersion = "";
static uint16_t fallbackProtocol = 0;

const std::string& resolveVersion(uint32_t protocol) {
    auto it = PROTOCOL_MAP.find(protocol);
    if (it != PROTOCOL_MAP.end()) return it->second;

    static std::unordered_set<uint32_t> warnedUnknowns;
    static std::mutex warnedMutex;
    bool shouldWarn = false;
    {
        std::lock_guard<std::mutex> lk(warnedMutex);
        auto res = warnedUnknowns.insert(protocol);
        shouldWarn = res.second;
    }
    if (shouldWarn) {
        char hexbuf[16];
        std::snprintf(hexbuf, sizeof(hexbuf), "%X", protocol);
        Logger::warn("Unknown Bedrock protocol " + std::to_string(protocol) + " (0x" + std::string(hexbuf) + ") - falling back to " + fallbackVersion);
    }
    return fallbackVersion;
}

uint16_t getLatestProtocol() {
    if (!PROTOCOL_MAP.empty()) return (uint16_t)PROTOCOL_MAP.rbegin()->first;
    return fallbackProtocol;
}

const std::string& getLatestVersion() {
    if (!PROTOCOL_MAP.empty()) return PROTOCOL_MAP.rbegin()->second;
    return fallbackVersion;
}

void setFallbackVersion(const std::string& v) { fallbackVersion = v; }
void setFallbackProtocol(uint16_t p) { fallbackProtocol = p; }

} // namespace Bedrock
