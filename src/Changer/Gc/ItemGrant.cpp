#include "ItemGrant.h"

#include "Changer/Inventory/CosmeticCatalog.h"
#include "Changer/Inventory/FakeItems.h"
#include "Changer/Inventory/RealItems.h"
#include "Changer/Gc/GcEmulator.h"
#include "Changer/Items/ItemSockets.h"
#include "Changer/Gc/ItemStyles.h"
#include "SDK/Econ/CEconItemDefinition.h"
#include "SDK/Econ/CEconItemSchema.h"
#include "SDK/Logger/Logger.h"
#include "SDK/SteamAPI/ISteamUser.h"

#include <cstddef>
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr std::uint32_t MaxDefIndex = 0xFFFF;
    constexpr std::uint32_t InvalidLoadoutSlot = 0xFFFFFFFF;
    // Hero loadout slots are 0..31; anything above that is a sentinel or garbage.
    constexpr std::uint32_t MaxLoadoutSlot = 64;

    CSOEconItem Make(std::uint32_t Def, std::uint32_t AccountId)
    {
        CSOEconItem Item;
        Item.set_id(FakeItems::IdBase | Def);
        Item.set_account_id(AccountId);
        Item.set_def_index(Def);
        Item.set_inventory(0);
        Item.set_quantity(1);
        Item.set_level(1);
        Item.set_quality(ItemSockets::QualityUnique);
        return Item;
    }

    std::vector<CSOEconItem> Build(const std::vector<std::uint32_t>& Defs, std::uint32_t AccountId)
    {
        std::vector<CSOEconItem> Added;
        Added.reserve(Defs.size());
        for (std::uint32_t Def : Defs)
        {
            if (!Def)
                continue;
            CSOEconItem Item = Make(Def, AccountId);
            if (auto Existing = FakeItems::Find(Item.id()))
                Item = *Existing;
            ItemStyles::UnlockAll(Item);
            Added.push_back(std::move(Item));
        }
        return Added;
    }
}

bool ItemGrant::Grant(std::uint32_t Def)
{
    return Grant(std::vector<std::uint32_t>{Def});
}

bool ItemGrant::Grant(const std::vector<std::uint32_t>& Defs)
{
    ISteamUser* User = ISteamUser::Get();
    std::uint32_t AccountId = User ? User->AccountId() : 0;
    if (!AccountId || Defs.empty())
        return false;

    std::vector<CSOEconItem> Added = Build(Defs, AccountId);
    if (Added.empty())
        return false;

    FakeItems::Store(Added);
    GcEmulator::SendItems(Added, {});
    Log("Grant: %zu items on request", Added.size());
    return true;
}

std::vector<CSOEconItem> ItemGrant::MissingWearables()
{
    std::vector<CSOEconItem> Added;

    CEconItemSchema* Schema = CEconItemSchema::Get();
    ISteamUser* User = ISteamUser::Get();
    if (!Schema || !User)
        return Added;

    std::uint32_t AccountId = User->AccountId();
    if (!AccountId)
        return Added;

    std::unordered_set<std::uint64_t> Known;
    for (const CSOEconItem& Item : FakeItems::All())
        Known.insert(Item.id());

    std::unordered_set<std::uint32_t> RealDefs;
    RealItems::ForEach([&](const CSOEconItem& Item) { RealDefs.insert(Item.def_index()); });

    std::size_t Definitions = 0;
    std::unordered_set<std::uint32_t> AddedDefs;
    for (std::uint32_t Def = 1; Def <= MaxDefIndex; ++Def)
    {
        CEconItemDefinition* Definition = Schema->GetItemDefinition(Def);
        if (!Definition)
            continue;

        ++Definitions;

        std::uint32_t Slot = Definition->LoadoutSlot();
        if (Slot == InvalidLoadoutSlot || Slot >= MaxLoadoutSlot || Definition->IsBundle())
            continue;
        if (RealDefs.count(Def) || Known.count(FakeItems::IdBase | Def))
            continue;

        Added.push_back(Make(Def, AccountId));
        AddedDefs.insert(Def);
    }

    // Global loadout cosmetics, personas and gems do not expose a hero loadout
    // slot, so they are granted from the generated catalog instead.
    std::size_t Extras = 0;
    for (std::uint32_t Def : CosmeticCatalog::ExtraDefs)
    {
        if (!Def || AddedDefs.count(Def) || RealDefs.count(Def) || Known.count(FakeItems::IdBase | Def))
            continue;

        CEconItemDefinition* Definition = Schema->GetItemDefinition(Def);
        if (!Definition || Definition->IsBundle())
            continue;

        Added.push_back(Make(Def, AccountId));
        AddedDefs.insert(Def);
        ++Extras;
    }

    if (!Added.empty())
        Log("Wearables: adding %zu of %zu item definitions (%zu global/persona/gem)", Added.size(), Definitions, Extras);
    return Added;
}
