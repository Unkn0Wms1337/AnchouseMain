#pragma once

#include "base_gcmessages.pb.h"

#include <cstdint>
#include <vector>

namespace ItemGrant
{
    bool Grant(std::uint32_t Def);
    bool Grant(const std::vector<std::uint32_t>& Defs);

    // Collects every equippable item definition that is neither owned for real
    // nor already present in the fake inventory. Does not store or send anything.
    std::vector<CSOEconItem> MissingWearables();
}
