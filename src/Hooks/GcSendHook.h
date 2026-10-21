#pragma once

#include <cstdint>
#include <string>

namespace GcSendHook
{
    bool Install();
    bool Post(std::uint32_t EMsg, const std::string& Body);
}
