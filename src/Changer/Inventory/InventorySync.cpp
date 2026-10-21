#include "InventorySync.h"

#include "Changer/Inventory/FakeItems.h"
#include "Changer/Gc/GcEmulator.h"
#include "Changer/Gc/ItemGrant.h"
#include "Changer/Inventory/GemCatalog.h"
#include "Changer/Items/ItemSockets.h"
#include "Changer/Gc/ItemStyles.h"
#include "Changer/Inventory/RealItems.h"
#include "Changer/Unusual/UnusualStore.h"
#include "Hooks/GcRecvHook.h"
#include "Hooks/GcSendHook.h"
#include "SDK/Config/Config.h"
#include "SDK/Econ/CDOTAInventoryManager.h"
#include "SDK/Econ/CDOTAPlayerInventory.h"
#include "SDK/Econ/C_EconItemView.h"
#include "SDK/Econ/CEconItemSchema.h"
#include "SDK/GC/CGCClient.h"
#include "SDK/Logger/Logger.h"
#include "SDK/SteamAPI/ISteamUser.h"

#include "gcsdk_gcmessages.pb.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr int SoTypeEconItem = 1;
    constexpr std::uint32_t SoOwnerTypeUser = 1;
    constexpr std::uint32_t EMsgSoCreate = 21;
    constexpr std::uint32_t EMsgSoUpdate = 22;
    constexpr std::uint32_t EMsgSoDestroy = 23;
    constexpr std::uint32_t EMsgSoCacheSubscribed = 24;
    constexpr std::uint32_t EMsgSoUpdateMultiple = 26;
    constexpr std::uint32_t EMsgSoSubscriptionCheck = 27;
    constexpr std::uint32_t EMsgSoSubscriptionRefresh = 28;
    constexpr std::uint32_t EMsgForceSoCacheResend = 7037;
    constexpr std::size_t DefaultGrantChunk = 256;
    constexpr long long DefaultGrantWaitMs = 9000;
    constexpr long long RefreshRetryMs = 4000;
    constexpr long long RefreshFirstMs = 1500;
    constexpr int RefreshMaxAttempts = 5;
    constexpr long long ProbeTimesMs[] = {4000, 6500};
    constexpr int ProbeCount = static_cast<int>(sizeof(ProbeTimesMs) / sizeof(ProbeTimesMs[0]));
    constexpr int WatchdogSeconds = 15;

    using Clock = std::chrono::steady_clock;
    using InventorySync::Action;

    long long Elapsed(Clock::time_point Start)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - Start).count();
    }

    bool AutoGrant()
    {
        static const bool Enabled = [] {
            return Config::Get("skin.autogrant") != "0";
        }();
        return Enabled;
    }

    std::size_t GrantChunk()
    {
        static const std::size_t Chunk = [] {
            std::string Value = Config::Get("skin.grant.chunk");
            if (Value.empty())
                return DefaultGrantChunk;
            long long Parsed = std::strtoll(Value.c_str(), nullptr, 10);
            return Parsed > 0 ? static_cast<std::size_t>(Parsed) : std::size_t(0);
        }();
        return Chunk;
    }

    bool ProbesEnabled()
    {
        static const bool Enabled = [] {
            return Config::Get("skin.grant.probe") != "0";
        }();
        return Enabled;
    }

    bool SnapshotAppend()
    {
        static const bool Enabled = [] {
            return Config::Get("skin.grant.snapshot") != "0";
        }();
        return Enabled;
    }

    long long GrantWaitMs()
    {
        static const long long Wait = [] {
            std::string Value = Config::Get("skin.grant.wait");
            if (Value.empty())
                return DefaultGrantWaitMs;
            long long Parsed = std::strtoll(Value.c_str(), nullptr, 10);
            return Parsed >= 0 ? Parsed * 1000 : DefaultGrantWaitMs;
        }();
        return Wait;
    }

    std::mutex ShownMutex;
    std::unordered_map<std::uint64_t, std::string> Shown;
    bool Started = false;
    bool Subscribed = false;
    bool Prepared = false;
    bool WatchdogStarted = false;

    bool GrantArmed = false;
    bool Streaming = false;
    bool SnapshotTaken = false;
    bool ProbesDone = false;
    int ProbeStage = 0;
    long long NextRefreshMs = RefreshFirstMs;
    int RefreshAttempts = 0;
    Clock::time_point GrantStart;

    std::vector<std::uint64_t> DeliveryQueue;
    std::size_t DeliveryIndex = 0;
    std::atomic<std::uint64_t> DeliveredCount{0};
    std::atomic<std::uint64_t> DeliveryTotal{0};
    std::atomic<std::uint64_t> FrameCount{0};
    std::atomic<int> GrantMode{0};

    std::uint64_t ServedOwner = 0;
    std::uint64_t ServedVersion = 0;
    bool HaveServed = false;
    std::uint64_t ProbeId = 0;
    std::atomic<int> CacheState{0};
    std::uint64_t HeldSeenFrame = 0;
    bool SeenPresent = false;
    int MissingProbes = 0;

    bool IsLocalOwner(const CMsgSOIDOwner& Owner)
    {
        ISteamUser* User = ISteamUser::Get();
        return Owner.type() == SoOwnerTypeUser && User && Owner.id() == User->GetSteamID();
    }

    std::set<std::uint32_t> OccupiedSlots()
    {
        std::set<std::uint32_t> Occupied;
        for (const CSOEconItem& Item : FakeItems::EquippedAll())
            for (const auto& State : Item.equipped_state())
                Occupied.insert((State.new_slot() << 16) | (State.new_class() & 0xFFFF));
        return Occupied;
    }

    CSOEconItem Desired(const CSOEconItem& Pristine, const std::set<std::uint32_t>& Occupied)
    {
        CSOEconItem Out = Pristine;
        auto* States = Out.mutable_equipped_state();
        for (int Index = States->size() - 1; Index >= 0; --Index)
        {
            const auto& State = States->Get(Index);
            if (Occupied.count((State.new_slot() << 16) | (State.new_class() & 0xFFFF)))
                States->DeleteSubrange(Index, 1);
        }
        if (int Effect = UnusualStore::Get(Pristine.id()))
            ItemSockets::SetUnusual(Out, Effect, Pristine.quality());
        return Out;
    }

    bool ReplaceReal(std::string& Data, const std::set<std::uint32_t>& Occupied)
    {
        CSOEconItem Item;
        if (!Item.ParseFromString(Data) || !Item.id() || FakeItems::IsFake(Item.id()))
            return false;
        RealItems::Set(Item);
        std::string Out = Desired(Item, Occupied).SerializeAsString();
        {
            std::lock_guard<std::mutex> Lock(ShownMutex);
            Shown[Item.id()] = Out;
        }
        if (Out == Data)
            return false;
        Data = std::move(Out);
        return true;
    }

    void Forget(const std::string& Data)
    {
        CSOEconItem Item;
        if (!Item.ParseFromString(Data) || !Item.id())
            return;
        RealItems::Remove(Item.id());
        std::lock_guard<std::mutex> Lock(ShownMutex);
        Shown.erase(Item.id());
    }

    void PrepareFakes()
    {
        auto Start = Clock::now();

        GemCatalog::EnsureGranted();

        ISteamUser* User = ISteamUser::Get();
        std::uint32_t AccountId = User ? User->AccountId() : 0;

        std::vector<CSOEconItem> Added;
        if (!Prepared)
            Added = ItemGrant::MissingWearables();

        for (CSOEconItem& Item : Added)
        {
            if (AccountId)
                Item.set_account_id(AccountId);
            ItemStyles::UnlockAll(Item);
            int Effect = UnusualStore::Get(Item.id());
            if (Effect || ItemSockets::FindGem(Item, ItemSockets::UnusualGemDef))
                ItemSockets::SetUnusual(Item, Effect, ItemSockets::QualityUnique);
        }

        std::size_t New = Added.size();

        if (AccountId)
            FakeItems::ForEach([&](CSOEconItem& Item) {
                if (Item.account_id() != AccountId)
                {
                    Item.set_account_id(AccountId);
                    Added.push_back(Item);
                }
            });

        if (!Added.empty())
            FakeItems::Store(Added);

        Prepared = true;
        Log("Grant: prepared, %zu new items, %lld ms", New, Elapsed(Start));
    }

    void QueueDelivery()
    {
        DeliveryQueue = FakeItems::Ids();
        DeliveryIndex = 0;
        DeliveredCount = 0;
        DeliveryTotal = DeliveryQueue.size();
        Log("Grant: queued %zu items, chunk %zu", DeliveryQueue.size(), GrantChunk());
    }

    void StreamChunk()
    {
        if (DeliveryIndex >= DeliveryQueue.size())
            return;
        if (GcRecvHook::Queued() != 0)
            return;

        std::size_t Chunk = GrantChunk();
        std::size_t Remaining = DeliveryQueue.size() - DeliveryIndex;
        std::size_t Count = Chunk ? (std::min)(Chunk, Remaining) : Remaining;

        std::vector<CSOEconItem> Batch;
        Batch.reserve(Count);
        for (std::size_t Index = DeliveryIndex; Index < DeliveryIndex + Count; ++Index)
        {
            if (auto Item = FakeItems::Find(DeliveryQueue[Index]))
                Batch.push_back(std::move(*Item));
        }
        DeliveryIndex += Count;

        if (!Batch.empty())
            GcEmulator::SendItems(Batch, {});

        std::uint64_t Sent = DeliveredCount.fetch_add(Batch.size()) + Batch.size();
        Log("Grant: streamed %llu/%llu (%zu items, %lld ms)",
            static_cast<unsigned long long>(Sent),
            static_cast<unsigned long long>(DeliveryTotal.load()), Batch.size(), Elapsed(GrantStart));
    }

    void FireProbe(int Index)
    {
        ISteamUser* User = ISteamUser::Get();
        std::uint64_t OwnerId = User ? User->GetSteamID() : 0;

        switch (Index)
        {
        case 0:
        {
            CMsgSOCacheSubscriptionCheck Check;
            Check.set_version(~std::uint64_t(0));
            Check.mutable_owner_soid()->set_type(SoOwnerTypeUser);
            Check.mutable_owner_soid()->set_id(OwnerId);
            GcEmulator::PushMessage(EMsgSoSubscriptionCheck, Check.SerializeAsString());
            Log("Grant: probe check -> client (EMsg %u)", EMsgSoSubscriptionCheck);
            break;
        }
        case 1:
            GcEmulator::PushMessage(EMsgForceSoCacheResend, std::string());
            Log("Grant: probe resend -> client (EMsg %u)", EMsgForceSoCacheResend);
            break;
        }
    }

    void TryRefreshGc(long long ElapsedMs)
    {
        if (RefreshAttempts >= RefreshMaxAttempts || ElapsedMs < NextRefreshMs)
            return;

        ISteamUser* User = ISteamUser::Get();
        CMsgSOCacheSubscriptionRefresh Refresh;
        Refresh.mutable_owner_soid()->set_type(SoOwnerTypeUser);
        Refresh.mutable_owner_soid()->set_id(User ? User->GetSteamID() : 0);

        if (!GcSendHook::Post(EMsgSoSubscriptionRefresh, Refresh.SerializeAsString()))
            return;

        ++RefreshAttempts;
        NextRefreshMs = ElapsedMs + RefreshRetryMs;
        Log("Grant: refresh -> gc (EMsg %u, attempt %d/%d)",
            EMsgSoSubscriptionRefresh, RefreshAttempts, RefreshMaxAttempts);
    }

    void AdvanceGrant()
    {
        if (SnapshotTaken)
            return;

        if (!GrantArmed)
        {
            GrantArmed = true;
            GrantStart = Clock::now();
            QueueDelivery();
        }

        long long ElapsedMs = Elapsed(GrantStart);

        if (ProbesEnabled())
        {
            TryRefreshGc(ElapsedMs);

            if (!ProbesDone)
            {
                while (ProbeStage < ProbeCount && ElapsedMs >= ProbeTimesMs[ProbeStage])
                    FireProbe(ProbeStage++);
                ProbesDone = ProbeStage >= ProbeCount;
            }

            if (!ProbesDone && ElapsedMs < GrantWaitMs())
                return;
        }

        if (!Streaming && ElapsedMs >= GrantWaitMs())
        {
            Streaming = true;
            GrantMode.store(1);
            Log("Grant: no snapshot after %lld ms, streaming %zu items by %zu",
                ElapsedMs, DeliveryQueue.size(), GrantChunk());
        }

        if (Streaming)
            StreamChunk();
    }

    void StartWatchdog()
    {
        if (WatchdogStarted)
            return;
        WatchdogStarted = true;
        std::thread([] {
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(WatchdogSeconds));
                Log("Watchdog: frames=%llu queued=%zu delivery=%llu/%llu mode=%d",
                    static_cast<unsigned long long>(FrameCount.load()), GcRecvHook::Queued(),
                    static_cast<unsigned long long>(DeliveredCount.load()),
                    static_cast<unsigned long long>(DeliveryTotal.load()), GrantMode.load());
            }
        }).detach();
    }

    void ProbeCache()
    {
        if (!ProbeId)
        {
            std::vector<std::uint64_t> Ids = FakeItems::Ids();
            if (Ids.empty())
                return;
            ProbeId = Ids.front();
        }

        CDOTAInventoryManager* Manager = CDOTAInventoryManager::Get();
        CDOTAPlayerInventory* Local = Manager ? Manager->LocalInventory() : nullptr;
        if (!Local)
            return;

        C_EconItemView* View = Local->FindItem(ProbeId);
        const int State = View ? 1 : 2;
        const int Previous = CacheState.exchange(State);
        if (Previous != State)
            Log("Cache probe: fake %#llx %s", static_cast<unsigned long long>(ProbeId),
                View ? "present" : "missing");

        if (View)
        {
            SeenPresent = true;
            MissingProbes = 0;
        }
        else if (SeenPresent)
        {
            ++MissingProbes;
        }
    }

    void BeginStream(const char* Reason, int Mode)
    {
        SnapshotTaken = false;
        Streaming = true;
        GrantArmed = true;
        GrantStart = Clock::now();
        ProbesDone = true;
        GrantMode.store(Mode);
        QueueDelivery();
        Log("Grant: %s, streaming %zu items", Reason, DeliveryQueue.size());
    }

    void WatchHeld(std::uint64_t Frame)
    {
        if (!GcRecvHook::HeldPending())
        {
            HeldSeenFrame = 0;
            return;
        }

        if (!HeldSeenFrame)
            HeldSeenFrame = Frame;
        const std::uint64_t Waited = Frame - HeldSeenFrame;
        if (Waited < 30)
            return;

        const int State = CacheState.load();
        if (State == 1)
        {
            GcRecvHook::DiscardHeld();
            Log("Subscribe: held duplicate dropped, cache intact");
            HeldSeenFrame = 0;
        }
        else if (State == 2)
        {
            if (GcRecvHook::ReplayHeld())
            {
                Log("Subscribe: cache empty, replayed held subscribe");
                if (Prepared)
                    BeginStream("cache rebuilt", 3);
                HeldSeenFrame = 0;
            }
        }
        else if (Waited >= 180)
        {
            GcRecvHook::DiscardHeld();
            Log("Subscribe: held duplicate dropped, cache unknown");
            HeldSeenFrame = 0;
        }
    }

    void OnUnusual(std::uint64_t ItemId, int Effect)
    {
        if (!FakeItems::IsFake(ItemId))
        {
            InventorySync::Refresh();
            return;
        }
        auto Item = FakeItems::Find(ItemId);
        if (!Item)
            return;
        ItemSockets::SetUnusual(*Item, Effect, ItemSockets::QualityUnique);
        FakeItems::Store(*Item);
        GcEmulator::SendItems({}, {*Item});
    }

    void EnsureSubscribed()
    {
        if (Subscribed)
            return;
        Subscribed = true;
        UnusualStore::Subscribe(OnUnusual);
    }

    Action RewriteSubscribed(std::string& Body)
    {
        CMsgSOCacheSubscribed Message;
        if (!Message.ParseFromString(Body) || !IsLocalOwner(Message.owner_soid()))
            return Action::Pass;

        const std::uint64_t OwnerId = Message.owner_soid().id();
        const std::uint64_t Version = Message.version();
        if (HaveServed && OwnerId == ServedOwner && Version <= ServedVersion)
        {
            const int State = CacheState.load();
            // Unknown is not the same as empty: before the first probe we must
            // assume the cache we already served is still alive, otherwise the
            // client rebuilds its inventory twice in a row and keeps stale
            // econ item views around (armory/demo entry then crash).
            if (State != 2)
            {
                Log("Subscribe: version %llu <= applied %llu, cache %s, held",
                    static_cast<unsigned long long>(Version), static_cast<unsigned long long>(ServedVersion),
                    State == 1 ? "intact" : "unknown");
                return Action::Drop;
            }
            Log("Subscribe: version %llu <= applied %llu, cache empty, re-applied",
                static_cast<unsigned long long>(Version), static_cast<unsigned long long>(ServedVersion));
        }

        EnsureSubscribed();
        Started = true;
        ServedOwner = OwnerId;
        ServedVersion = Version;
        HaveServed = true;
        CacheState.store(0);
        RealItems::Clear();
        {
            std::lock_guard<std::mutex> Lock(ShownMutex);
            Shown.clear();
        }

        std::set<std::uint32_t> Occupied = OccupiedSlots();
        std::size_t Total = 0;
        std::size_t Replaced = 0;
        for (auto& Type : *Message.mutable_objects())
        {
            if (Type.type_id() != SoTypeEconItem)
                continue;
            for (std::string& Data : *Type.mutable_object_data())
            {
                ++Total;
                Replaced += ReplaceReal(Data, Occupied) ? 1 : 0;
            }
        }

        ProbesDone = true;

        std::size_t Appended = 0;
        if (AutoGrant() && SnapshotAppend())
        {
            if (!Prepared)
                PrepareFakes();

            decltype(Message.add_objects()) Group = nullptr;
            for (auto& Type : *Message.mutable_objects())
            {
                if (Type.type_id() == SoTypeEconItem)
                {
                    Group = &Type;
                    break;
                }
            }
            if (!Group)
            {
                Group = Message.add_objects();
                Group->set_type_id(SoTypeEconItem);
            }

            for (std::uint64_t Id : FakeItems::Ids())
            {
                if (auto Item = FakeItems::Find(Id))
                {
                    Group->add_object_data(Item->SerializeAsString());
                    ++Appended;
                }
            }

            if (Appended)
            {
                SnapshotTaken = true;
                Streaming = false;
                GrantMode.store(2);
                DeliveryQueue.clear();
                DeliveryIndex = 0;
                DeliveredCount.store(Appended);
                DeliveryTotal.store(Appended);
            }
        }

        if (!Replaced && !Appended)
        {
            Log("Subscribe: %zu real items, %zu replaced, ignored", Total, Replaced);
            return Action::Pass;
        }

        const std::size_t BeforeBytes = Body.size();
        Body = Message.SerializeAsString();
        Log("Subscribe: %zu real items, %zu replaced, %zu fakes added, %zu -> %zu bytes",
            Total, Replaced, Appended, BeforeBytes, Body.size());
        return Action::Changed;
    }

    bool RewriteMultiple(std::string& Body)
    {
        CMsgSOMultipleObjects Message;
        if (!Message.ParseFromString(Body) || !IsLocalOwner(Message.owner_soid()))
            return false;

        std::set<std::uint32_t> Occupied = OccupiedSlots();
        bool Changed = false;
        for (auto& Object : *Message.mutable_objects_added())
            if (Object.type_id() == SoTypeEconItem)
                Changed |= ReplaceReal(*Object.mutable_object_data(), Occupied);
        for (auto& Object : *Message.mutable_objects_modified())
            if (Object.type_id() == SoTypeEconItem)
                Changed |= ReplaceReal(*Object.mutable_object_data(), Occupied);
        for (const auto& Object : Message.objects_removed())
            if (Object.type_id() == SoTypeEconItem)
                Forget(Object.object_data());
        if (!Changed)
            return false;
        Body = Message.SerializeAsString();
        return true;
    }

    bool RewriteSingle(std::uint32_t EMsg, std::string& Body)
    {
        CMsgSOSingleObject Message;
        if (!Message.ParseFromString(Body) || !IsLocalOwner(Message.owner_soid()) || Message.type_id() != SoTypeEconItem)
            return false;
        if (EMsg == EMsgSoDestroy)
        {
            Forget(Message.object_data());
            return false;
        }
        if (!ReplaceReal(*Message.mutable_object_data(), OccupiedSlots()))
            return false;
        Body = Message.SerializeAsString();
        return true;
    }
}

void InventorySync::Update()
{
    FrameCount.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t Frame = FrameCount.load(std::memory_order_relaxed);
    StartWatchdog();

    ISteamUser* User = ISteamUser::Get();
    if (!Started)
    {
        if (!CGCClient::Get() || !User || !User->AccountId() || !CEconItemSchema::Get())
            return;
        Started = true;
        EnsureSubscribed();
        if (AutoGrant() && !Prepared)
            PrepareFakes();
        Refresh();
    }

    if (HaveServed && Prepared)
    {
        if (Frame % 30 == 0)
        {
            ProbeCache();
            const bool Busy = Streaming && DeliveryIndex < DeliveryQueue.size();
            if (SeenPresent && MissingProbes >= 2 && !Busy && AutoGrant())
            {
                SeenPresent = false;
                MissingProbes = 0;
                BeginStream("cache lost", 4);
            }
        }
        WatchHeld(Frame);
    }

    if (AutoGrant())
        AdvanceGrant();
}

void InventorySync::Refresh()
{
    std::set<std::uint32_t> Occupied = OccupiedSlots();
    std::vector<CSOEconItem> Modified;
    RealItems::ForEach([&](const CSOEconItem& Pristine) {
        CSOEconItem Item = Desired(Pristine, Occupied);
        std::string Data = Item.SerializeAsString();
        std::lock_guard<std::mutex> Lock(ShownMutex);
        std::string& Current = Shown[Pristine.id()];
        if (Current == Data)
            return;
        Current = Data;
        Modified.push_back(Item);
    });

    if (Modified.empty())
        return;
    GcEmulator::SendItems({}, Modified);
}

InventorySync::Action InventorySync::Rewrite(std::uint32_t EMsg, std::string& Body)
{
    switch (EMsg)
    {
    case EMsgSoCacheSubscribed:
        return RewriteSubscribed(Body);
    case EMsgSoUpdateMultiple:
        return RewriteMultiple(Body) ? Action::Changed : Action::Pass;
    case EMsgSoCreate:
    case EMsgSoUpdate:
    case EMsgSoDestroy:
        return RewriteSingle(EMsg, Body) ? Action::Changed : Action::Pass;
    default:
        return Action::Pass;
    }
}
