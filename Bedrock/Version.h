#pragma once

#include <string>
#include <cstdint>

namespace Bedrock {
    const std::string& resolveVersion(uint32_t protocol);
    uint16_t getLatestProtocol();
    const std::string& getLatestVersion();
    void setFallbackVersion(const std::string& v);
    void setFallbackProtocol(uint16_t p);
}
