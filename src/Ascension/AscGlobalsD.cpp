// More GLOBAL bindings, each from its decompile (ghidra-ext/out/bindings/<Name>.c).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    constexpr uint32_t kMapDbc = 0xAD4160;
    constexpr uint32_t kAreaTableDbc = 0xAD3134;
    constexpr uint32_t kItemDbcRows = 0xAD3D4C;        // FUN_100b1870 (rows 0xAD3D6C)
    constexpr uint32_t kSpellDurationDbc = 0xAD4820;   // FUN_100b23e0 (rows 0xAD4840)

    uint32_t DbcMinId(uint32_t c) { return *reinterpret_cast<const uint32_t*>(c + 0x10); }
    uint32_t DbcMaxId(uint32_t c) { return *reinterpret_cast<const uint32_t*>(c + 0x0C); }

    void PushGuid(lua_State* L, uint32_t lo, uint32_t hi)   // FUN_10287550 -> 0x74D0D0
    {
        char buf[32] = {};
        reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, char*)>(0x74D0D0)(lo, hi, buf);
        AscLua::lua_pushstring(L, buf);
    }

    // DAT_10b1b220: {0, 2^32} -- the compiler's u32 -> double fix-up.
    double U32(uint32_t v) { return static_cast<double>(v); }

    // FUN_1020a*: exactly one numeric argument, then the item addon record (FUN_101a2690).
    const AscItemAddon::Record* ItemArg(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1 || !AscLua::lua_isnumber(L, 1))
            return nullptr;
        return AscItemAddon::Find(static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 1))));
    }

    int ItemField(lua_State* L, uint32_t field)
    {
        if (const AscItemAddon::Record* r = ItemArg(L))
            AscLua::lua_pushinteger(L, static_cast<int>(r->fields[field]));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }
    int GetItemQuality(lua_State* L)  { return ItemField(L, 0); }   // +0x10
    int GetItemPvEPower(lua_State* L) { return ItemField(L, 2); }   // +0x18
    int GetItemPvPPower(lua_State* L) { return ItemField(L, 3); }   // +0x1C

    int GetItemName(lua_State* L)   // +0x08
    {
        if (const AscItemAddon::Record* r = ItemArg(L))
            PushStr(L, r->name);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetItemFlavorText(lua_State* L)   // +0x0C
    {
        if (const AscItemAddon::Record* r = ItemArg(L))
            PushStr(L, r->description);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // Item.dbc display id (+0x14) -> ItemDisplayInfo record (0x4CFD90 on 0xAD3DDC), inventory icon +0x14.
    int GetItemIconInstant(lua_State* L)
    {
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isnumber(L, 1))
        {
            const uint32_t id = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 1)));
            if (const uint8_t* item = ClientDbcRow(kItemDbcRows, id))
            {
                uint8_t rec[100] = {};
                typedef int(__thiscall* GetRec_t)(void*, uint32_t, void*);
                if (reinterpret_cast<GetRec_t>(0x4CFD90)(reinterpret_cast<void*>(0xAD3DDC),
                                                         *reinterpret_cast<const uint32_t*>(item + 0x14), rec))
                {
                    PushStr(L, *reinterpret_cast<const char* const*>(rec + 0x14));
                    return 1;
                }
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // max(RecoveryTime +0x74, CategoryRecoveryTime +0x78), then the u32 at +0x238.
    int GetSpellBaseCooldown(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        uint8_t rec[0x2A8];
        if (!FetchSpell(id, rec))
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 2;
        }
        uint32_t cd = *reinterpret_cast<uint32_t*>(rec + 0x78);
        if (cd < *reinterpret_cast<uint32_t*>(rec + 0x74))
            cd = *reinterpret_cast<uint32_t*>(rec + 0x74);
        AscLua::lua_pushnumber(L, U32(cd));
        AscLua::lua_pushnumber(L, U32(*reinterpret_cast<uint32_t*>(rec + 0x238)));
        return 2;
    }

    // SpellDuration row (DurationIndex +0xA0), base duration +4.
    int GetSpellBaseDuration(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        uint8_t rec[0x2A8];
        if (FetchSpell(id, rec))
            if (const uint8_t* row = ClientDbcRow(kSpellDurationDbc, *reinterpret_cast<uint32_t*>(rec + 0xA0)))
            {
                AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row + 4));
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // EffectApplyAuraName[3] at +0x17C: SCHOOL_ABSORB (69) -> true + that effect's MiscValue (+0x1B8);
    // MANA_SHIELD (97) -> true.
    int IsAbsorbSpell(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        uint8_t rec[0x2A8];
        if (!FetchSpell(id, rec))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const int32_t* aura = reinterpret_cast<const int32_t*>(rec + 0x17C);
        const int32_t* misc = reinterpret_cast<const int32_t*>(rec + 0x1B8);
        for (int i = 0; i < 3; ++i)
            if (aura[i] == 0x45)
            {
                AscLua::lua_pushboolean(L, 1);
                int32_t v;
                if (aura[0] == 0x45)
                    v = misc[0];
                else if (aura[1] == 0x45)
                    v = misc[1];
                else
                    v = aura[2] == 0x45 ? misc[2] : 0;
                AscLua::lua_pushinteger(L, v);
                return 2;
            }
        for (int i = 0; i < 3; ++i)
            if (aura[i] == 0x61)
            {
                AscLua::lua_pushboolean(L, 1);
                return 1;
            }
        AscLua::lua_pushboolean(L, 0);
        return 1;
    }

    // (bag, slot): bag 0 = PLAYER_FIELD_PACK_SLOT (+0x5C8), else the bag in PLAYER_FIELD_INV_SLOT (+0x5A0)
    // and its CONTAINER_FIELD_SLOT (+0x108).
    int GetContainerItemGUID(lua_State* L)
    {
        if (uint8_t* p = ActivePlayer())
        {
            const int32_t bag = ToInt(AscLua::lua_tonumber(L, 1));
            const uint32_t slot = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 2)) - 1);
            if (slot < 0x24)
            {
                const uint8_t* d = *reinterpret_cast<uint8_t**>(p + 8);
                const uint32_t* g = nullptr;
                if (bag == 0)
                    g = reinterpret_cast<const uint32_t*>(d + 0x5C8 + slot * 8);
                else if (uint8_t* c = static_cast<uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(d + 0x5A0 + bag * 8), 4)))
                    g = reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(c + 8) + 0x108 + slot * 8);
                if (g && (g[0] || g[1]))
                {
                    PushGuid(L, g[0], g[1]);
                    return 1;
                }
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // (unit, slot 1..22): the player's PLAYER_FIELD_INV_SLOT_HEAD (+0x510); only for players (type 4).
    int GetInventoryItemGUID(lua_State* L)
    {
        const std::string token = CheckString(L, 1);
        const uint32_t slot = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 2)) - 1);
        if (slot < 0x16)
        {
            const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x60C1C0)(token.c_str());
            uint8_t* obj = guid ? static_cast<uint8_t*>(ObjectPtr(guid, 8)) : nullptr;
            if (obj && *reinterpret_cast<int32_t*>(obj + 0x14) == 4)
            {
                const uint32_t* g = reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(obj + 8) + 0x510 + slot * 8);
                if (g[0] || g[1])
                {
                    PushGuid(L, g[0], g[1]);
                    return 1;
                }
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // Quest log (descriptor +0x278, 25 x 0x14; ASCENSION_QUEST_LOG_64: the 64 slots, 25..63 from SMSG 0x8F7):
    // 0x5DEEB0 -> log index, 0x5E0EA0(index - 1, 1).
    int IsQuestComplete(lua_State* L)
    {
        if (*reinterpret_cast<const uint8_t*>(0xBD0792))
        {
            const int32_t id = ToInt(AscLua::lua_tonumber(L, 1));
            const uint8_t* d = *reinterpret_cast<uint8_t**>(ActivePlayer() + 8);
#if ASCENSION_QUEST_LOG_64
            bool logged = AscQuestLog64::QuestSlot(d, static_cast<uint32_t>(id)) >= 0;
#else
            bool logged = false;
            for (uint32_t i = 0; i < 0x19 && !logged; ++i)
            {
                const int32_t q = *reinterpret_cast<const int32_t*>(d + 0x278 + i * 0x14);
                logged = q != 0 && q == id;
            }
#endif
            if (logged)
            {
                const int idx = reinterpret_cast<int(__cdecl*)(int)>(0x5DEEB0)(id);
                const char done = reinterpret_cast<char(__cdecl*)(int, int)>(0x5E0EA0)(idx - 1, 1);
                AscLua::lua_pushboolean(L, done != 0);
                return 1;
            }
        }
        AscLua::lua_pushboolean(L, 0);
        return 1;
    }

    // Map.dbc ids 0 .. max-1 whose MapName (+0x14) equals the argument exactly.
    int GetMapID(lua_State* L)
    {
        if (!ValidateInput(L, {STRING}))
            return 0;
        const std::string name = CheckString(L, 1);
        const uint32_t max = DbcMaxId(kMapDbc);
        for (uint32_t i = 0; i < max; ++i)
            if (const uint8_t* row = ClientDbcRow(kMapDbc, i))
                if (name == *reinterpret_cast<const char* const*>(row + 0x14))
                {
                    AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row));
                    return 1;
                }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // AreaTable name (+0x2C) -> id.
    int GetAreaIdByName(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        for (uint32_t i = DbcMinId(kAreaTableDbc); i < DbcMaxId(kAreaTableDbc) + 1; ++i)
            if (const uint8_t* row = ClientDbcRow(kAreaTableDbc, i))
                if (name == *reinterpret_cast<const char* const*>(row + 0x2C))
                {
                    AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row));
                    return 1;
                }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // Names of AreaTable rows with flag 0x2000, collected once (DAT_10d3d74b / vector DAT_10d3d7fc).
    int IsAreaHidden(lua_State* L)
    {
        static bool built = false;
        static std::vector<std::string> hidden;
        if (!built)
        {
            for (uint32_t i = DbcMinId(kAreaTableDbc); i < DbcMaxId(kAreaTableDbc) + 1; ++i)
                if (const uint8_t* row = ClientDbcRow(kAreaTableDbc, i))
                    if (*reinterpret_cast<const uint32_t*>(row + 0x10) & 0x2000)
                        hidden.emplace_back(*reinterpret_cast<const char* const*>(row + 0x2C));
            built = true;
        }
        const std::string name = CheckString(L, 1);
        bool found = false;
        for (const std::string& s : hidden)
            if (s == name)
            {
                found = true;
                break;
            }
        AscLua::lua_pushboolean(L, found);
        return 1;
    }

    // Expansion cap (RealmInfo +0x08: 0 -> 60, 1 -> 70, 2 -> 80), lowered by the level-cap game events.
    int GetRealmMaxLevel(lua_State* L)
    {
        const uint32_t ruleset = RealmInfoSvc::Get().ruleset;
        uint32_t cap = ruleset == 1 ? 70 : ruleset == 2 ? 80 : 60;
        using AscGameEvents::IsActive;
        if (!IsActive(0x164))
        {
            uint32_t eventCap;
            if (IsActive(0xAD))
                eventCap = 70;
            else if (IsActive(0x1EA))
                eventCap = 60;
            else
            {
                static const struct { uint16_t ev; int level; } kFixed[] = {{0x1E9, 55}, {0x1E8, 45}, {0x1E7, 35}, {0x1E6, 25}};
                for (const auto& f : kFixed)
                    if (IsActive(f.ev))
                    {
                        AscLua::lua_pushinteger(L, f.level);
                        return 1;
                    }
                eventCap = IsActive(0x160) ? 60 : cap;
            }
            if (eventCap < cap)
                cap = eventCap;
        }
        AscLua::lua_pushinteger(L, static_cast<int>(cap));
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetAreaIdByName", GetAreaIdByName},
        {nullptr, "GetContainerItemGUID", GetContainerItemGUID},
        {nullptr, "GetInventoryItemGUID", GetInventoryItemGUID},
        {nullptr, "GetItemFlavorText", GetItemFlavorText},
        {nullptr, "GetItemIconInstant", GetItemIconInstant},
        {nullptr, "GetItemName", GetItemName},
        {nullptr, "GetItemPvEPower", GetItemPvEPower},
        {nullptr, "GetItemPvPPower", GetItemPvPPower},
        {nullptr, "GetItemQuality", GetItemQuality},
        {nullptr, "GetMapID", GetMapID},
        {nullptr, "GetRealmMaxLevel", GetRealmMaxLevel},
        {nullptr, "GetSpellBaseCooldown", GetSpellBaseCooldown},
        {nullptr, "GetSpellBaseDuration", GetSpellBaseDuration},
        {nullptr, "IsAbsorbSpell", IsAbsorbSpell},
        {nullptr, "IsAreaHidden", IsAreaHidden},
        {nullptr, "IsQuestComplete", IsQuestComplete},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
