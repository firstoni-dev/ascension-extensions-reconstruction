// Gossip and quest-log hooks: the three installed by the misc packet module FUN_100d4b90 (whose packet
// handlers and events live in AscMisc.cpp and elsewhere) and the two of FUN_100e0cc0.
//
//   0x58A9E0 -> FUN_100d9b40   GetGossipOptions replaced: extended icon types 0x15..0x28 get names
//   0x58AF10 -> FUN_100de060   SelectGossipOption, then GOSSIP_OPTION_SELECTED
//   0x53BA40 -> sub_100e0300   the client's by-name comparator replaced: record 120 sorts first
//   0x6D1110 -> FUN_100e0ab0   QUEST_TURNED_IN(quest, a, b)
//   0x6D4F80 -> FUN_100e0ba0   QUEST_REMOVED(quest)
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscLua.hpp>
#include <cstring>
#include <cstdint>

namespace
{
    // ---- GetGossipOptions (FUN_100d9b40, replaces 0x58A9E0) ---------------------------------------
    // The client's 32 gossip options at 0xC00BF0 (0x18 bytes: +0 text, +8 index (< 0 = unused), +0x14
    // icon type). Each used option pushes its text and icon name: types below 0x15 from the client's
    // table 0xACEF20, the Ascension ones by name, anything else "". Returns 2 values per option.
    const char* ExtendedIcon(uint32_t type)
    {
        switch (type)
        {
        case 0x15: return "ActiveQuestIcon";
        case 0x16: return "AvailableQuestIcon";
        case 0x17: return "DailyActiveQuestIcon";
        case 0x18: return "DailyQuestIcon";
        case 0x19: return "activelegendaryquesticon";
        case 0x1A: return "availablelegendaryquesticon";
        case 0x1B: return "transmogrifygossipicon";
        case 0x1C: return "workordergossipicon";
        case 0x1D: return "campaignactivequesticon";
        case 0x1E: return "campaignavailablequesticon";
        case 0x1F: return "campaignincompletequesticon";
        case 0x20: return "campaignactivedailyquesticon";
        case 0x21: return "campaignavailabledailyquesticon";
        case 0x22: return "invisiblegossipicon";
        case 0x23: return "GreyTowergossipicon";
        case 0x24: return "SilverSkullgossipicon";
        case 0x25: return "SilverSwordsgossipicon";
        case 0x26: return "SilverChestgossipicon";
        case 0x27: return "AzzarFaireFriendsgossipicon";
        case 0x28: return "AzzarFairePlayergossipicon";
        default: return "";
        }
    }
    int __cdecl GetGossipOptions(lua_State* L)
    {
        int n = 0;
        for (const uint8_t* o = reinterpret_cast<const uint8_t*>(0xC00BF0); reinterpret_cast<uint32_t>(o) <= 0xC00EEF; o += 0x18)
        {
            if (*reinterpret_cast<const int32_t*>(o + 8) < 0)
                continue;
            AscLua::lua_checkstack(L, 2);
            const char* text = *reinterpret_cast<const char* const*>(o);
            AscLua::lua_pushstring(L, text ? text : "");
            const uint32_t type = *reinterpret_cast<const uint32_t*>(o + 0x14);
            if (type < 0x15)
            {
                const char* name = *reinterpret_cast<const char* const*>(0xACEF20 + type * 4);
                AscLua::lua_pushstring(L, name ? name : "");
            }
            else
                AscLua::lua_pushstring(L, ExtendedIcon(type));
            ++n;
        }
        return n * 2;
    }

    // ---- SelectGossipOption (FUN_100de060 over 0x58AF10, __cdecl(index, a, b)) ---------------------
    typedef int(__cdecl* Select_t)(int32_t, uint32_t, uint32_t);
    Select_t g_select = nullptr;
    int __cdecl SelectDetour(int32_t index, uint32_t a, uint32_t b)
    {
        const int r = g_select(index, a, b);
        AscRuntime::Signal("GOSSIP_OPTION_SELECTED", "%d%d", *reinterpret_cast<const int32_t*>(0xC016F8), index + 1);
        return r;
    }

    // ---- by-name comparator (sub_100e0300 in place of 0x53BA40) -------------------------------------
    // a / b point at a pointer to a row id of the client table 0xAD45E0 (FUN_100b2020). A null id sorts
    // first (-1 / 1); a missing row compares equal; row id 120 sorts before everything else; otherwise
    // _stricmp of the names (+0xC) where the client used its own compare (0x76EC80).
    const uint8_t* Row(uint32_t id)
    {
        const uint8_t* const* index = *reinterpret_cast<const uint8_t* const* const*>(0xAD4600);
        const uint32_t lo = *reinterpret_cast<const uint32_t*>(0xAD45F0), hi = *reinterpret_cast<const uint32_t*>(0xAD45EC);
        if (!index || id < lo || id > hi)
            return nullptr;
        return index[id - lo];
    }
    int __cdecl CompareByName(const uint32_t* const* a, const uint32_t* const* b)
    {
        const uint32_t ia = **a;
        if (ia == 0)
            return -1;
        const uint32_t ib = **b;
        if (ib == 0)
            return 1;
        const uint8_t* ra = Row(ia);
        const uint8_t* rb = Row(ib);
        if (!ra || !rb)
            return 0;
        const uint32_t ta = *reinterpret_cast<const uint32_t*>(ra), tb = *reinterpret_cast<const uint32_t*>(rb);
        if (ta == 0x78)
        {
            if (tb != 0x78)
                return INT32_MIN;
        }
        else if (tb == 0x78)
            return INT32_MAX;
        return _stricmp(*reinterpret_cast<const char* const*>(ra + 0xC), *reinterpret_cast<const char* const*>(rb + 0xC));
    }

    // ---- quest events ------------------------------------------------------------------------------
    // FUN_100e0ab0 over 0x6D1110 (__stdcall(p)): after the original, QUEST_TURNED_IN with the three
    // dwords at *(p +4).
    typedef int(__stdcall* TurnIn_t)(uint8_t*);
    TurnIn_t g_turnIn = nullptr;
    int __stdcall TurnInDetour(uint8_t* p)
    {
        const int r = g_turnIn(p);
        const uint32_t* d = *reinterpret_cast<const uint32_t* const*>(p + 4);
        AscRuntime::Signal("QUEST_TURNED_IN", "%u%u%u", d[0], d[1], d[2]);
        return r;
    }
    // FUN_100e0ba0 over 0x6D4F80 (__thiscall(player, slot), ret 4): after the original, QUEST_REMOVED
    // with the quest id the log slot holds (descriptor +0x278 + slot * 0x14) -- read after the removal,
    // as the original does.
    // ASCENSION_QUEST_LOG_64: the byte slot of the 64 (25..63 from SMSG 0x8F7, still holding the quest until the
    // server's next packet, as the descriptor does), 0 past 63.
    typedef void(__fastcall* Remove_t)(uint8_t*, void*, int32_t);
    Remove_t g_remove = nullptr;
    void __fastcall RemoveDetour(uint8_t* player, void* edx, int32_t slot)
    {
        g_remove(player, edx, slot);
        const uint8_t* d = *reinterpret_cast<uint8_t* const*>(player + 8);
#if ASCENSION_QUEST_LOG_64
        const uint8_t* e = AscQuestLog64::SlotEntry(d, static_cast<uint8_t>(slot));
        AscRuntime::Signal("QUEST_REMOVED", "%u", e ? *reinterpret_cast<const uint32_t*>(e) : 0u);
#else
        AscRuntime::Signal("QUEST_REMOVED", "%u", *reinterpret_cast<const uint32_t*>(d + 0x278 + slot * 0x14));
#endif
    }

    void Init()
    {
        AscRuntime::ReplaceFunction(0x58A9E0, reinterpret_cast<void*>(&GetGossipOptions));
        g_select = reinterpret_cast<Select_t>(AscRuntime::Detour(0x58AF10, 6, reinterpret_cast<void*>(&SelectDetour)));
        AscRuntime::ReplaceFunction(0x53BA40, reinterpret_cast<void*>(&CompareByName));
        g_turnIn = reinterpret_cast<TurnIn_t>(AscRuntime::Detour(0x6D1110, 6, reinterpret_cast<void*>(&TurnInDetour)));
        g_remove = reinterpret_cast<Remove_t>(AscRuntime::Detour(0x6D4F80, 6, reinterpret_cast<void*>(&RemoveDetour)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
