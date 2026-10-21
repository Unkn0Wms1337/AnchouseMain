#pragma once

#include <cstdint>
#include <string>

namespace InventorySync
{
    enum class Action
    {
        Pass,
        Changed,
        Drop,
    };

    void Update();
    void Refresh();
    Action Rewrite(std::uint32_t EMsg, std::string& Body);
}
