// Small namespaces with no shared state: C_Sun, C_CVar, C_Chat (infractions), C_Taxi, C_Item, C_QuestLog.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Windows.h>
#include <cstring>
#include <ctime>
#include <string>

using namespace AscScript;

namespace
{
    // ---- C_Sun (FUN_1026a540 / 790 / 9e0) ----
    // FUN_1026c920 NOPs the three fstp's in 0x7EEC90's tail that recompute the sun vector each frame
    // (0x7EECA0 -> 0xD38C9C, 0x7EECA9 -> 0xD38CA0, 0x7EECB2 -> 0xD38CA4), then the binding writes the
    // component itself. Called as C_Sun:SetX(v) -- exactly two arguments, and only in world (0xBD0792).
    void WriteBytes(uint32_t at, const void* src, uint32_t n)
    {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old);
        memcpy(reinterpret_cast<void*>(at), src, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    }

    void FreezeSun()
    {
        static const uint8_t kNops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        for (uint32_t at : {0x7EECA0u, 0x7EECA9u, 0x7EECB2u})
        {
            WriteBytes(at, kNops, sizeof(kNops));
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), sizeof(kNops));
        }
    }

    int SetSun(lua_State* L, uint32_t component)
    {
        if (AscLua::lua_gettop(L) != 2 || !*reinterpret_cast<const uint8_t*>(0xBD0792))
            return 0;
        const float v = static_cast<float>(CheckNumber(L, 2));
        FreezeSun();
        WriteBytes(component, &v, sizeof(v));
        return 0;
    }
    int SetX(lua_State* L) { return SetSun(L, 0xD38C9C); }
    int SetY(lua_State* L) { return SetSun(L, 0xD38CA0); }
    int SetZ(lua_State* L) { return SetSun(L, 0xD38CA4); }

    // ---- C_CVar (FUN_10114940 / a50 / b60) ----
    // 0x767440 CVar lookup by name; 0x766860 / 0x766CA0 are thiscall on a holder of the CVar*.
    void* FindCVar(const char* name) { return reinterpret_cast<void*(__cdecl*)(const char*)>(0x767440)(name); }

    int GetCVarBitfield(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        const uint32_t index = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        void* cvar = FindCVar(name.c_str());
        if (!cvar)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const bool set = reinterpret_cast<char(__thiscall*)(void**, uint32_t)>(0x766860)(&cvar, index) != 0;
        PushBool(L, set);
        return 1;
    }

    int SetCVarBitfield(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        const uint32_t index = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const bool value = AscLua::lua_toboolean(L, 3) != 0;
        void* cvar = FindCVar(name.c_str());
        bool ok = false;
        if (cvar)
            ok = reinterpret_cast<char(__thiscall*)(void**, uint32_t, bool)>(0x766CA0)(&cvar, index, value) != 0;
        PushBool(L, ok);
        return 1;
    }

    // CVar +0x1C bit 0x80 = "do not save".
    int SetCVarSave(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        const bool save = AscLua::lua_toboolean(L, 2) != 0;
        if (uint8_t* cvar = static_cast<uint8_t*>(FindCVar(name.c_str())))
        {
            uint32_t& flags = *reinterpret_cast<uint32_t*>(cvar + 0x1C);
            flags = save ? (flags & ~0x80u) : (flags | 0x80u);
        }
        return 0;
    }

    // ---- C_Chat infractions ----
    // SMSG_CHAT_INFRACTION 0x6E5 (FUN_10114ef0): u32, str, u32; sets "has infraction" (0x10BDD1C1)
    // and fires CHAT_INFRACTION_UPDATE. AcknowledgeChatInfraction sets 0x10BDD1C0, which nothing
    // clears, and sends CMSG 0x6E4.
    struct
    {
        bool acknowledged = false, has = false;
        int32_t a = 0, b = 0;
        std::string text;
    } g_infraction;

    void __cdecl OnChatInfraction(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_infraction.a = *reinterpret_cast<const int32_t*>(p->m_buffer + p->m_read);
        p->m_read += 4;
        g_infraction.text = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        p->m_read += static_cast<int32_t>(g_infraction.text.size() + 1);
        g_infraction.b = *reinterpret_cast<const int32_t*>(p->m_buffer + p->m_read);
        p->m_read += 4;
        g_infraction.has = true;
        AscRuntime::Signal("CHAT_INFRACTION_UPDATE");
    }

    // Returns 1 with nothing pushed, as the original does.
    int AcknowledgeChatInfraction(lua_State*)
    {
        g_infraction.acknowledged = true;
        Packet(0x6E4).Send();
        return 1;
    }

    int GetChatInfraction(lua_State* L)
    {
        if (!g_infraction.has)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushInt(L, g_infraction.a);
        PushStr(L, g_infraction.text.c_str());
        PushInt(L, g_infraction.b);
        return 3;
    }

    int HasChatInfraction(lua_State* L)
    {
        PushBool(L, !g_infraction.acknowledged && g_infraction.has);
        return 1;
    }

    // ---- C_Taxi (FUN_10217ce0 / d20 / d90) ----
    // Client taxi map: current node id 0xC0D7EC; node count 0xC0D7E4; 0x30-byte slots at 0xC0DC38
    // whose first dword is the TaxiNodes row.
    int GetCurrentTaxiNodeID(lua_State* L)
    {
        PushInt(L, *reinterpret_cast<const int32_t*>(0xC0D7EC));
        return 1;
    }

    int GetTaxiNodeID(lua_State* L)
    {
        const uint32_t slot = static_cast<uint32_t>(ToInt(CheckNumber(L, 1))) - 1;
        if (slot < *reinterpret_cast<const uint32_t*>(0xC0D7E4))
            if (const int32_t* node = *reinterpret_cast<int32_t* const*>(0xC0DC38 + slot * 0x30))
            {
                PushInt(L, *node);
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // TaxiTimes.dbc: +4 from slot, +8 to slot (0-based), +0xC duration.
    int GetTaxiPathDuration(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER}))
            return 0;
        const int32_t from = ToInt(CheckNumber(L, 1)) - 1;
        const int32_t to = ToInt(CheckNumber(L, 2)) - 1;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\TaxiTimes.dbc");
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (row && AscDbc::Table::I32(row, 4) == from && AscDbc::Table::I32(row, 8) == to)
            {
                PushInt(L, AscDbc::Table::I32(row, 0xC));
                return 1;
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- C_Item (FUN_100c71c0 / FUN_100cac80) ----
    // The argument is an item GUID string (0x74D120 -> guid, ObjectPtr type 2). 0x708520 is the
    // client's "is soulbound" (item +0xD4 flags bit 0, else 0x7073E0). CanBind: not bound and the
    // ItemCache record's bonding (+0x178) is set.
    void* ItemFromGuidString(const char* s)
    {
        const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x74D120)(s);
        return ObjectPtr(guid, 2);
    }
    bool IsSoulbound(void* item) { return reinterpret_cast<char(__thiscall*)(void*)>(0x708520)(item) != 0; }

    int CanBind(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        uint8_t* item = static_cast<uint8_t*>(ItemFromGuidString(s.c_str()));
        const uint8_t* rec = nullptr;
        if (item)
        {
            const uint32_t entry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(item + 8) + 0xC);
            rec = static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67CA30)(
                reinterpret_cast<void*>(0xC5D828), entry, nullptr, nullptr, nullptr, 0));
        }
        if (!rec)
            AscLua::lua_pushnil(L);
        else
            PushBool(L, !IsSoulbound(item) && *reinterpret_cast<const int32_t*>(rec + 0x178) != 0);
        return 1;
    }

    int IsBound(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        void* item = ItemFromGuidString(s.c_str());
        if (!item)
            AscLua::lua_pushnil(L);
        else
            PushBool(L, IsSoulbound(item));
        return 1;
    }

    // FUN_100ca700: the item's addon field 0 (FUN_1027f1f0 -> FUN_102cdbb0).
    int GetScalingLevel(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        uint8_t* item = static_cast<uint8_t*>(ItemFromGuidString(s.c_str()));
        if (!item)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const uint64_t guid = **reinterpret_cast<const uint64_t* const*>(item + 8);
        PushInt(L, static_cast<int32_t>(AscObjectAddon::Item(guid)[0].value));
        return 1;
    }

    // ---- C_QuestLog ----
    // FUN_1021ae60: QuestCache (0xC5DA48, 0x67DE90) record +0x50 flags: 0x400000 -> 2, 0x800000 -> 4, else 1.
    int GetQuestType(lua_State* L)
    {
        const uint32_t questId = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint8_t* rec = static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67DE90)(
            reinterpret_cast<void*>(0xC5DA48), questId, nullptr, nullptr, nullptr, 0));
        int type = 1;
        if (rec)
        {
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(rec + 0x50);
            if (flags & 0x400000)
                type = 2;
            else if (flags & 0x800000)
                type = 4;
        }
        PushInt(L, type);
        return 1;
    }

    const uint8_t* CacheRecord(uint32_t getter, uint32_t cache, uint32_t id)
    {
        return static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(getter)(
            reinterpret_cast<void*>(cache), id, nullptr, nullptr, nullptr, 0));
    }
    const uint8_t* QuestRec(uint32_t id) { return CacheRecord(0x67DE90, 0xC5DA48, id); }      // QuestCache
    const uint8_t* CreatureRec(uint32_t id) { return CacheRecord(0x67B6A0, 0xC5D690, id); }   // CreatureCache

    // FUN_101940a0: the client's quest list at 0xC237B0 (count 0xC23AD0), 16 bytes each: +0 quest id,
    // +8 0 = still open. ASCENSION_QUEST_LOG_64: the DLL's list (128 rows) and the 64 slots, the first empty slot
    // ending the descriptor's 25 only (AscQuestLog64::FindLoggedQuest).
    template <class F> bool ForOpenLoggedQuest(uint8_t* player, F fn)
    {
        const uint8_t* desc = *reinterpret_cast<uint8_t**>(player + 8);
#if ASCENSION_QUEST_LOG_64
        uint32_t count = 0;
        const uint32_t* rows = AscQuestLog64::QuestListRows(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t* e = rows + i * 4;
            if (e[2] != 0)
                continue;
            // 32 bits, as in QuestRelevant (AscAttachFixes.cpp). The original reads the low word here too.
            if (const uint8_t* logged = AscQuestLog64::FindLoggedQuest(desc, e[0]))
                if (fn(e[0], logged))
                    return true;
        }
        return false;
#else
        const uint32_t count = *reinterpret_cast<const uint32_t*>(0xC23AD0);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t* e = reinterpret_cast<const uint32_t*>(0xC237B0 + i * 0x10);
            if (e[2] != 0)
                continue;
            for (uint32_t slot = 0; slot < 25; ++slot)   // PLAYER_QUEST_LOG, 0x14 per slot
            {
                // 32 bits, as in QuestRelevant (AscAttachFixes.cpp). The original reads the low word here too.
                const uint32_t logged = *reinterpret_cast<const uint32_t*>(desc + 0x278 + slot * 0x14);
                if (!logged)
                    break;
                if (logged == e[0])
                {
                    if (fn(e[0], desc + 0x278 + slot * 0x14))
                        return true;
                    break;
                }
            }
        }
        return false;
#endif
    }

    // FUN_1008d8c0: stack count of `item` in the backpack (inventory slots 0x17..0x26) and the four
    // bags (0xC23540).
    uint32_t CountInBags(uint8_t* player, uint32_t item)
    {
        const uint8_t* desc = *reinterpret_cast<uint8_t**>(player + 8);
        uint32_t n = 0;
        for (uint32_t slot = 0x17; slot < 0x27; ++slot)
            if (const uint8_t* it = static_cast<const uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(desc + 0x510 + slot * 8), 2)))
                if (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(it + 8) + 0xC) == item)
                    n += *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(it + 8) + 0x38);
        const uint64_t* bags = reinterpret_cast<const uint64_t*>(0xC23540);
        for (int b = 0; b < 4; ++b)
        {
            uint8_t* bag = static_cast<uint8_t*>(ObjectPtr(bags[b], 4));
            if (!bag)
                continue;
            void* inv = (*reinterpret_cast<void*(__thiscall***)(void*)>(bag))[0x24 / 4](bag);
            if (!inv)
                continue;
            const uint32_t slots = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(bag + 8) + 0x100);
            for (uint32_t i = 0; i < slots; ++i)
                if (const uint8_t* it = reinterpret_cast<uint8_t*(__thiscall*)(void*, uint32_t)>(0x754390)(inv, i))
                    if (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(it + 8) + 0xC) == item)
                        n += *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(it + 8) + 0x38);
        }
        return n;
    }

    // FUN_1008dc50: an open logged quest needs `creature` killed (ReqCreatureOrGO +0x1C24, count
    // +0x1C34 against the slot's u16 counters at +8) and the kill count is not yet reached. *quest is
    // set for every matching objective.
    bool NeedsKill(uint8_t* player, uint32_t creature, uint32_t* quest)
    {
        if (!CreatureRec(creature))
            return false;
        return ForOpenLoggedQuest(player, [&](uint32_t id, const uint8_t* slot) {
            const uint8_t* q = QuestRec(id);
            if (!q)
                return false;
            const uint64_t counts = *reinterpret_cast<const uint64_t*>(slot + 8);
            for (uint32_t i = 0; i < 4; ++i)
                if (*reinterpret_cast<const int32_t*>(q + 0x1C24 + i * 4) == static_cast<int32_t>(creature))
                {
                    const uint32_t have = static_cast<uint16_t>(counts >> (i * 16));
                    if (quest)
                        *quest = id;
                    if (*reinterpret_cast<const uint32_t*>(q + 0x1C34 + i * 4) != have)
                        return true;
                }
            return false;
        });
    }

    // FUN_1008da60: an open logged quest needs one of `creature`'s quest items (creature record +0x40,
    // six) -- ReqItem +0x1C44 / count +0x1C5C -- and the bags hold a different count.
    bool NeedsItem(uint8_t* player, uint32_t creature, uint32_t* quest)
    {
        const uint8_t* c = CreatureRec(creature);
        if (!c)
            return false;
        return ForOpenLoggedQuest(player, [&](uint32_t id, const uint8_t*) {
            const uint8_t* q = QuestRec(id);
            if (!q)
                return false;
            for (uint32_t j = 0; j < 6; ++j)
                for (uint32_t u = 0; u < 6; ++u)
                {
                    const int32_t item = *reinterpret_cast<const int32_t*>(c + 0x40 + j * 4);
                    if (item != 0 && item == *reinterpret_cast<const int32_t*>(q + 0x1C44 + u * 4))
                    {
                        if (quest)
                            *quest = id;
                        if (*reinterpret_cast<const uint32_t*>(q + 0x1C5C + u * 4) != CountInBags(player, static_cast<uint32_t>(item)))
                            return true;
                    }
                }
            return false;
        });
    }

    // FUN_1021ac30: (unit token). A unit with a quest-giver state (+0x90): its name ("unavailable" 1,
    // "trivial" 2, "incomplete" 5, "handin" 6/9/10, "pickup" 8, else ""), -1, and the string at
    // (+0x8C)->+0x2C +0x3C. Otherwise its creature record's +0x6C entry against the quest log:
    // "objective" or "collect" and the quest id. Else nil.
    int GetUnitQuestInfo(lua_State* L)
    {
        const char* s = AscLua::tostring(L, 1);
        const std::string token = s ? s : "";
        uint8_t* player = token.empty() ? nullptr : ActivePlayer();
        if (player)
        {
            const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x60C1C0)(token.c_str());
            const uint8_t* unit = guid ? static_cast<const uint8_t*>(ObjectPtr(guid, 8)) : nullptr;
            if (unit)
            {
                const uint32_t state = *reinterpret_cast<const uint32_t*>(unit + 0x90);
                if (state != 0)
                {
                    const char* name;
                    switch (state)
                    {
                    case 1: name = "unavailable"; break;
                    case 2: name = "trivial"; break;
                    case 5: name = "incomplete"; break;
                    case 6: case 9: case 10: name = "handin"; break;
                    case 8: name = "pickup"; break;
                    default: name = ""; break;
                    }
                    PushStr(L, name);
                    AscLua::lua_pushinteger(L, -1);
                    const uint8_t* info = *reinterpret_cast<uint8_t* const*>(unit + 0x8C);
                    const uint8_t* block = info ? *reinterpret_cast<uint8_t* const*>(info + 0x2C) : nullptr;
                    const char* text = block ? reinterpret_cast<const char*>(block + 0x3C) : nullptr;
                    PushStr(L, text);
                    return 3;
                }
                const uint32_t entry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0xC);
                if (const uint8_t* rec = CreatureRec(entry))
                {
                    const uint32_t credit = *reinterpret_cast<const uint32_t*>(rec + 0x6C);
                    uint32_t quest = 0;
                    const char* kind = nullptr;
                    if (NeedsKill(player, credit, &quest))
                        kind = "objective";
                    else if (NeedsItem(player, credit, &quest))
                        kind = "collect";
                    if (kind)
                    {
                        PushStr(L, kind);
                        AscLua::lua_pushinteger(L, static_cast<int>(quest));
                        return 2;
                    }
                }
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_1021af00: the player's race (UNIT_FIELD_BYTES_0 byte 0) -> ChrRaces (client 0xAD3428) +0x1C:
    // 1 -> 3, 7 -> 2, else 0. First ZoneStory row (min <= id < max -- the max id is never visited)
    // with +0xC == zone and +4 in {0, that value}: +8, +0x10.
    int GetZoneStoryInfo(lua_State* L)
    {
        if (uint8_t* player = ActivePlayer())
        {
            const int32_t zone = ToInt(CheckNumber(L, 1));
            const uint32_t type = *reinterpret_cast<const uint32_t*>(player + 0x14);
            const uint8_t race = (type == 3 || type == 4) ? *(*reinterpret_cast<uint8_t* const*>(player + 8) + 0x5C) : 0;
            int32_t side = 0;
            if (const uint8_t* chrRace = ClientDbcRow(0xAD3428, race))
            {
                const int32_t v = *reinterpret_cast<const int32_t*>(chrRace + 0x1C);
                side = v == 1 ? 3 : v == 7 ? 2 : 0;
            }
            AscDbc::Table& t = AscDbc::Get("DBFilesClient\\ZoneStory.dbc");
            for (uint32_t id = t.MinId(); t.Loaded() && id < t.MaxId(); ++id)
            {
                const uint8_t* row = t.Row(id);
                if (!row || AscDbc::Table::I32(row, 0xC) != zone)
                    continue;
                const int32_t f = AscDbc::Table::I32(row, 4);
                if (f != 0 && f != side)
                    continue;
                PushInt(L, AscDbc::Table::I32(row, 8));
                PushStr(L, t.Str(row, 0x10));
                return 2;
            }
        }
        return PushNils(L, 2);
    }

    // ---- C_Scenario (FUN_103073e0 .. handler_IsInScenario): constant in the original ----
    int GetActiveStage(lua_State* L) { PushNum(L, 0); PushStr(L, ""); return 2; }
    int GetEncounterAtIndex(lua_State* L) { AscLua::lua_createtable(L, 0, 0); AscLua::lua_checkstack(L, 2); return 1; }
    int GetNumEncounters(lua_State* L) { PushNum(L, 0); return 1; }
    int GetScenarioName(lua_State* L) { PushStr(L, ""); return 1; }
    int IsInScenario(lua_State* L) { PushBool(L, false); return 1; }

    // ---- C_LoyaltyPass (FUN_10296970 .. handler_CanRedeemLoyaltyPassReward) ----
    // SMSG 0x563 (handler_0x0563): u32 next reward time (0x10BE3074), u32 expiry (0x10BE3078). "Now"
    // is local time plus the client's server-time offset at 0xC23AE8. ClaimRewards sends CMSG 0x564.
    uint32_t g_loyaltyNext = 0, g_loyaltyExpiry = 0;

    void __cdecl OnLoyaltyPass(void*, uint32_t, uint32_t, CDataStore* p)
    {
        memcpy(&g_loyaltyNext, p->m_buffer + p->m_read, 4);
        memcpy(&g_loyaltyExpiry, p->m_buffer + p->m_read + 4, 4);
        p->m_read += 8;
    }
    uint32_t LoyaltyNow() { return static_cast<uint32_t>(static_cast<int32_t>(_time64(nullptr)) + *reinterpret_cast<const int32_t*>(0xC23AE8)); }
    bool LoyaltyActive() { return g_loyaltyNext != 0 && (g_loyaltyExpiry == 0 || LoyaltyNow() < g_loyaltyExpiry); }
    bool LoyaltyRedeemable() { return LoyaltyActive() && g_loyaltyNext <= LoyaltyNow(); }   // FUN_10296880

    int ClaimRewards(lua_State*)
    {
        if (LoyaltyActive() && LoyaltyRedeemable())
            Packet(0x564).Send();
        return 0;
    }
    int GetTimeUntilNextReward(lua_State* L)
    {
        const uint32_t now = LoyaltyNow();
        PushInt(L, (LoyaltyActive() && !LoyaltyRedeemable() && now < g_loyaltyNext) ? static_cast<int32_t>(g_loyaltyNext - now) : 0);
        return 1;
    }
    int GetTimeUntilExpiration(lua_State* L)
    {
        const uint32_t now = LoyaltyNow();
        PushInt(L, (LoyaltyActive() && now < g_loyaltyExpiry) ? static_cast<int32_t>(g_loyaltyExpiry - now) : 0);
        return 1;
    }
    int IsLoyaltyPassActive(lua_State* L) { PushBool(L, LoyaltyActive()); return 1; }
    int IsLoyaltyPassExpired(lua_State* L) { PushBool(L, g_loyaltyExpiry != 0 && g_loyaltyExpiry <= LoyaltyNow()); return 1; }
    int CanRedeemLoyaltyPassReward(lua_State* L) { PushBool(L, LoyaltyRedeemable()); return 1; }

    // ---- C_CharacterCustomizationUnlocks (handler_GetCurrentOptionInfo / ...): constant in the original ----
    int GetCurrentOptionInfo(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        CheckNumber(L, 1);
        PushBool(L, false);
        PushBool(L, true);
        PushInt(L, 0);
        return 3;
    }
    int GetFirstLockedRequirement(lua_State* L) { PushInt(L, 0); return 1; }
    int HasLockedSelections(lua_State* L) { PushBool(L, false); return 1; }

    void Init()
    {
        sDC.AddPacketHandler(0x563, CNetClientCustomPacket((void*)&OnLoyaltyPass, nullptr));
        sDC.AddPacketHandler(0x6E5, CNetClientCustomPacket((void*)&OnChatInfraction, nullptr));
    }

    // C_EquipmentSet.PlaceInBank(index) (0x10228520): luaL_checknumber; below 20, CMSG 0x55E with index - 1
    // (index 0 wraps). Returns nothing.
    int PlaceInBank(lua_State* L)
    {
        const uint32_t index = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        if (index < 0x14)
            Packet(0x55E).U32(index - 1).Send();
        return 0;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Sun", "SetX", SetX},
        {"C_Sun", "SetY", SetY},
        {"C_Sun", "SetZ", SetZ},
        {"C_CVar", "GetCVarBitfield", GetCVarBitfield},
        {"C_CVar", "SetCVarBitfield", SetCVarBitfield},
        {"C_CVar", "SetCVarSave", SetCVarSave},
        {"C_Chat", "AcknowledgeChatInfraction", AcknowledgeChatInfraction},
        {"C_Chat", "GetChatInfraction", GetChatInfraction},
        {"C_Chat", "HasChatInfraction", HasChatInfraction},
        {"C_Taxi", "GetCurrentTaxiNodeID", GetCurrentTaxiNodeID},
        {"C_Taxi", "GetTaxiNodeID", GetTaxiNodeID},
        {"C_Taxi", "GetTaxiPathDuration", GetTaxiPathDuration},
        {"C_LoyaltyPass", "ClaimRewards", ClaimRewards},
        {"C_LoyaltyPass", "GetTimeUntilNextReward", GetTimeUntilNextReward},
        {"C_LoyaltyPass", "GetTimeUntilExpiration", GetTimeUntilExpiration},
        {"C_LoyaltyPass", "IsLoyaltyPassActive", IsLoyaltyPassActive},
        {"C_LoyaltyPass", "IsLoyaltyPassExpired", IsLoyaltyPassExpired},
        {"C_LoyaltyPass", "CanRedeemLoyaltyPassReward", CanRedeemLoyaltyPassReward},
        {"C_CharacterCustomizationUnlocks", "GetCurrentOptionInfo", GetCurrentOptionInfo},
        {"C_CharacterCustomizationUnlocks", "GetFirstLockedRequirement", GetFirstLockedRequirement},
        {"C_CharacterCustomizationUnlocks", "HasLockedSelections", HasLockedSelections},
        {"C_Scenario", "GetActiveStage", GetActiveStage},
        {"C_Scenario", "GetEncounterAtIndex", GetEncounterAtIndex},
        {"C_Scenario", "GetNumEncounters", GetNumEncounters},
        {"C_Scenario", "GetScenarioName", GetScenarioName},
        {"C_Scenario", "IsInScenario", IsInScenario},
        {"C_Item", "CanBind", CanBind},
        {"C_Item", "IsBound", IsBound},
        {"C_Item", "GetScalingLevel", GetScalingLevel},
        {"C_QuestLog", "GetQuestType", GetQuestType},
        {"C_QuestLog", "GetUnitQuestInfo", GetUnitQuestInfo},
        {"C_QuestLog", "GetZoneStoryInfo", GetZoneStoryInfo},
        {"C_EquipmentSet", "PlaceInBank", PlaceInBank},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
