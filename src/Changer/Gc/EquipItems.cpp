#include "EquipItems.h"

#include "Changer/Inventory/FakeItems.h"
#include "Changer/Gc/GcEmulator.h"
#include "Changer/Inventory/InventorySync.h"
#include "Hooks/GcRecvHook.h"
#include "SDK/Logger/Logger.h"

#include "econ_gcmessages.pb.h"

#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr std::uint32_t EMsgEquipItemsResponse = 2570;
    constexpr std::uint32_t StyleUnchanged = 255;

    bool RemoveState(CSOEconItem& Item, std::uint32_t Class, std::uint32_t Slot, bool AnySlot)
    {
        bool Changed = false;
        auto* States = Item.mutable_equipped_state();
        for (int Index = States->size() - 1; Index >= 0; --Index)
        {
            const auto& State = States->Get(Index);
            if (State.new_class() == Class && (AnySlot || State.new_slot() == Slot))
            {
                States->DeleteSubrange(Index, 1);
                Changed = true;
            }
        }
        return Changed;
    }

    void Flush(const std::unordered_map<std::uint64_t, CSOEconItem>& Modified)
    {
        if (Modified.empty())
            return;
        std::vector<CSOEconItem> Items;
        Items.reserve(Modified.size());
        for (const auto& [Id, Item] : Modified)
            Items.push_back(Item);
        FakeItems::Store(Items);
        GcEmulator::SendItems({}, Items);
    }

    void ClearFakeSlot(std::uint32_t Class, std::uint32_t Slot, std::uint64_t Keep, std::unordered_map<std::uint64_t, CSOEconItem>& Modified)
    {
        for (const CSOEconItem& Original : FakeItems::EquippedAll())
        {
            CSOEconItem Item = Original;
            auto Existing = Modified.find(Item.id());
            if (Existing != Modified.end())
                Item = Existing->second;
            std::uint64_t Id = Item.id();
            if (Id != Keep && RemoveState(Item, Class, Slot, false))
                Modified[Id] = std::move(Item);
        }
    }
}

bool EquipItems::OnRequest(const void* Data, std::uint32_t Size)
{
    CMsgProtoBufHeader RequestHeader;
    CMsgClientToGCEquipItems Request;
    if (!GcEmulator::Parse(Data, Size, RequestHeader, Request) || Request.equips_size() == 0)
        return false;

    bool HasReal = false;
    for (const auto& Equip : Request.equips())
    {
        if (Equip.item_id() != 0)
        {
            HasReal |= !FakeItems::IsFake(Equip.item_id());
            continue;
        }
        bool FakeInSlot = FakeItems::Equipped(Equip.new_class(), Equip.new_slot()) != 0;
        HasReal |= !FakeInSlot;
    }

    std::unordered_map<std::uint64_t, CSOEconItem> Modified;
    if (HasReal)
    {
        for (const auto& Equip : Request.equips())
            if (Equip.item_id() != 0 && !FakeItems::IsFake(Equip.item_id()))
                ClearFakeSlot(Equip.new_class(), Equip.new_slot(), 0, Modified);
        Flush(Modified);
        return false;
    }

    for (const auto& Equip : Request.equips())
    {
        std::uint32_t Class = Equip.new_class();
        std::uint32_t Slot = Equip.new_slot();
        ClearFakeSlot(Class, Slot, Equip.item_id(), Modified);
        if (Class == 0 && Equip.item_id() != 0)
            Log("Equip: global slot %u -> %#llx", Slot, static_cast<unsigned long long>(Equip.item_id()));

        if (Equip.item_id() == 0)
            continue;

        auto Existing = Modified.find(Equip.item_id());
        if (Existing == Modified.end())
        {
            auto Found = FakeItems::Find(Equip.item_id());
            if (!Found)
            {
                LogE("Equip: unknown fake %#llx", static_cast<unsigned long long>(Equip.item_id()));
                continue;
            }
            Existing = Modified.emplace(Equip.item_id(), std::move(*Found)).first;
        }

        CSOEconItem& Item = Existing->second;
        RemoveState(Item, Class, Slot, false);
        auto* State = Item.add_equipped_state();
        State->set_new_class(Class);
        State->set_new_slot(Slot);
        if (Equip.style_index() != StyleUnchanged)
            Item.set_style(Equip.style_index());
    }

    Flush(Modified);
    InventorySync::Refresh();

    CMsgClientToGCEquipItemsResponse Response;
    Response.set_so_cache_version_id(GcRecvHook::Owner().Version);
    GcEmulator::Respond(EMsgEquipItemsResponse, RequestHeader, Response);
    return true;
}
