#pragma once

#include "base_gcmessages.pb.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace FakeItems
{
    constexpr std::uint64_t IdBase = 0x7000000000000000ull;

    bool IsFake(std::uint64_t ItemId);
    void Store(const CSOEconItem& Item);
    void Store(const std::vector<CSOEconItem>& Items);
    std::optional<CSOEconItem> Find(std::uint64_t ItemId);
    bool Has(std::uint64_t ItemId);
    std::uint64_t Equipped(std::uint32_t Class, std::uint32_t Slot);
    std::vector<CSOEconItem> EquippedAll();
    std::vector<CSOEconItem> All();
    std::vector<std::uint64_t> Ids();
    void ForEach(const std::function<void(CSOEconItem&)>& Visit);
}
