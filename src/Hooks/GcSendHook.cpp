#include "GcSendHook.h"

#include "Changer/Gc/EquipItems.h"
#include "Changer/Gc/ItemStyles.h"
#include "Changer/Gc/Sockets.h"
#include "Misc/Patterns.h"
#include "SDK/GC/CGCClient.h"
#include "SDK/Logger/Logger.h"
#include "SDK/Memory/Memory.h"
#include "SDK/SteamAPI/ISteamUser.h"

#include "steammessages.pb.h"

#include <MinHook.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    bool (*Original)(void*, std::uint32_t, const void*, std::uint32_t) = nullptr;

    constexpr std::uint32_t ProtoFlag = 0x80000000u;
    constexpr std::uint32_t FlagMask = 0xFFF00000u;

    constexpr std::uint32_t EMsgExtractGems = 1086;
    constexpr std::uint32_t EMsgAddSocket = 1087;
    constexpr std::uint32_t EMsgAddItemToSocket = 1088;
    constexpr std::uint32_t EMsgEquipItems = 2569;
    constexpr std::uint32_t EMsgSetItemStyle = 2577;

    std::mutex SendMutex;
    void* LastClient = nullptr;
    std::uint32_t LastFlags = ProtoFlag;
    std::string LastHeader;
    std::unordered_map<std::uint32_t, int> SendCounts;
    bool FramingLogged = false;

    void NoteSend(void* Client, std::uint32_t EMsg, const void* Data, std::uint32_t Size)
    {
        std::lock_guard<std::mutex> Lock(SendMutex);

        LastClient = Client;

        if (Data && Size >= 8)
        {
            std::uint32_t Type = 0;
            std::uint32_t HeaderLen = 0;
            std::memcpy(&Type, Data, 4);
            std::memcpy(&HeaderLen, static_cast<const std::uint8_t*>(Data) + 4, 4);
            if (8 + static_cast<std::size_t>(HeaderLen) <= Size)
            {
                LastFlags = Type & FlagMask;
                LastHeader.assign(static_cast<const char*>(Data) + 8, HeaderLen);
            }
            if (!FramingLogged)
            {
                FramingLogged = true;
                Log("GC send framing: type %#x header %u size %u", Type, HeaderLen, Size);
            }
        }

        std::uint32_t Clean = EMsg & 0x7FFFFFFFu;
        int& Count = SendCounts[Clean];
        ++Count;
        bool Watched = Clean == 27 || Clean == 28 || Clean == 29 || Clean == 1005 || Clean == 7037;
        if (Count == 1 || (Watched && Count <= 20))
            Log("GC send: EMsg %u (%u bytes, #%d)", Clean, Size, Count);
    }

    bool Handle(std::uint32_t EMsg, const void* Data, std::uint32_t Size)
    {
        switch (EMsg)
        {
        case EMsgEquipItems:
            return EquipItems::OnRequest(Data, Size);
        case EMsgSetItemStyle:
            return ItemStyles::OnSetStyle(Data, Size);
        case EMsgAddSocket:
            return Sockets::OnAddSocket(Data, Size);
        case EMsgAddItemToSocket:
            return Sockets::OnAddGem(Data, Size);
        case EMsgExtractGems:
            return Sockets::OnExtractGems(Data, Size);
        default:
            return false;
        }
    }

    bool Detour(void* Client, std::uint32_t EMsg, const void* Data, std::uint32_t Size)
    {
        NoteSend(Client, EMsg, Data, Size);
        if (Handle(EMsg & 0x7FFFFFFFu, Data, Size))
            return true;
        return Original(Client, EMsg, Data, Size);
    }
}

bool GcSendHook::Post(std::uint32_t EMsg, const std::string& Body)
{
    if (!Original)
        return false;

    void* Client = nullptr;
    std::string Header;
    std::uint32_t Flags = ProtoFlag;
    {
        std::lock_guard<std::mutex> Lock(SendMutex);
        Client = LastClient;
        Header = LastHeader;
        Flags = LastFlags;
    }

    if (!Client)
        Client = CGCClient::Get();
    if (!Client)
        return false;

    CMsgProtoBufHeader Parsed;
    if (Header.empty() || !Parsed.ParseFromString(Header))
        Parsed.Clear();

    ISteamUser* User = ISteamUser::Get();
    if (User)
        Parsed.set_client_steam_id(User->GetSteamID());
    if (!Parsed.has_source_app_id())
        Parsed.set_source_app_id(570);

    Parsed.clear_job_id_source();
    Parsed.clear_job_id_target();
    Header = Parsed.SerializeAsString();

    std::uint32_t Type = (Flags & FlagMask) | EMsg;
    std::uint32_t HeaderLen = static_cast<std::uint32_t>(Header.size());
    std::vector<std::uint8_t> Out(8 + Header.size() + Body.size());
    std::memcpy(Out.data(), &Type, 4);
    std::memcpy(Out.data() + 4, &HeaderLen, 4);
    std::memcpy(Out.data() + 8, Header.data(), Header.size());
    std::memcpy(Out.data() + 8 + Header.size(), Body.data(), Body.size());

    bool Result = Original(Client, Type, Out.data(), static_cast<std::uint32_t>(Out.size()));
    Log("GC post: EMsg %u (%zu bytes) %s", EMsg, Out.size(), Result ? "sent" : "failed");
    return Result;
}

bool GcSendHook::Install()
{
    std::uintptr_t Target = Memory::FindPattern("client.dll", Patterns::GcSend);
    if (!Target)
    {
        LogE("GcSend pattern not found");
        return false;
    }

    MH_STATUS Status = MH_Initialize();
    if (Status != MH_OK && Status != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogE("MinHook init failed: %s", MH_StatusToString(Status));
        return false;
    }

    auto* TargetFn = reinterpret_cast<void*>(Target);
    Status = MH_CreateHook(TargetFn, reinterpret_cast<void*>(Detour), reinterpret_cast<void**>(&Original));
    if (Status == MH_OK)
        Status = MH_EnableHook(TargetFn);

    if (Status != MH_OK)
    {
        LogE("GcSend hook failed: %s", MH_StatusToString(Status));
        return false;
    }

    Log("GcSend %#llx", static_cast<unsigned long long>(Target));
    return true;
}
