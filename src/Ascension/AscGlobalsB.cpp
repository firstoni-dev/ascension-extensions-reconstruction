// Small GLOBAL bindings over the client's caches, DBC containers and descriptors, plus three packets
// that feed them. Each is transcribed from its decompile (ghidra-ext/out/bindings/<Name>.c).
//   SMSG 0x768 HONORABLE_COMBAT_ZONES (FUN_100d3b30): u32 n x u32 zone -> HONORABLE_COMBAT_ZONES_PAYLOAD
//   SMSG 0x76F CHARACTER_SELECTION_SORT_ORDER (FUN_100d3a00): cstring
//   SMSG 0x66D LFG_UPDATE_CUSTOM (FUN_100d3f80): u32 dungeon -> CURRENT_LFG_DUNGEON_ID_CHANGED on change
// A null cache record returns NO values (the null in eax is the return count); a missing DBC row
// pushes nil.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    const uint32_t kMapDbc = 0xAD4160, kAreaDbc = 0xAD3134, kAchievementDbc = 0xAD305C;
    const uint32_t kSkillLineAbilityDbc = 0xAD4598, kSkillLineDbc = 0xAD45E0;

    std::vector<uint32_t> g_honorableZones;   // DAT_10bdb5f0
    bool g_honorableZonesSet = false;         // DAT_10bdb5ee
    std::string g_sortOrder;                  // DAT_10bc94ac
    bool g_sortOrderSet = false;              // DAT_10bdb5fe
    uint32_t g_lfgDungeon = 0;                // DAT_10bdb640
    uint8_t g_castCount = 0;                  // DAT_10bde438 (.bss)

    const uint8_t* ItemRecord(uint32_t id)    // 0x67CA30 on the item cache 0xC5D828
    {
        return static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67CA30)(
            reinterpret_cast<void*>(0xC5D828), id, nullptr, nullptr, nullptr, 0));
    }
    const uint8_t* QuestRecord(uint32_t id)   // 0x67DE90 on the quest cache 0xC5DA48
    {
        return static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67DE90)(
            reinterpret_cast<void*>(0xC5DA48), id, nullptr, nullptr, nullptr, 0));
    }
    uint8_t* PlayerDescriptors()
    {
        uint8_t* p = ActivePlayer();
        return p ? *reinterpret_cast<uint8_t**>(p + 8) : nullptr;
    }
    int32_t ArgInt(lua_State* L, int idx) { return ToInt(CheckNumber(L, idx)); }
    void PushGuidString(lua_State* L, uint32_t lo, uint32_t hi)   // 0x74D0D0
    {
        char buf[32] = {};
        reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, char*)>(0x74D0D0)(lo, hi, buf);
        AscLua::lua_pushstring(L, buf);
    }
    void PushRowStr(lua_State* L, const uint8_t* row, uint32_t off)
    {
        const char* s = *reinterpret_cast<const char* const*>(row + off);
        PushStr(L, s);
    }

    int LoadWDBCache(lua_State*) { reinterpret_cast<void(__cdecl*)()>(0x635520)(); return 0; }
    int SaveWDBCache(lua_State*) { reinterpret_cast<void(__cdecl*)()>(0x635540)(); return 0; }

    int GetItemSetID(lua_State* L)
    {
        const uint8_t* rec = ItemRecord(static_cast<uint32_t>(ArgInt(L, 1)));
        AscLua::lua_pushinteger(L, rec ? *reinterpret_cast<const int32_t*>(rec + 0x1A8) : 0);
        return 1;
    }

    int GetItemTemplateBonding(lua_State* L)
    {
        const uint8_t* rec = ItemRecord(static_cast<uint32_t>(ArgInt(L, 1)));
        if (!rec)
            return 0;
        AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(rec + 0x178));
        return 1;
    }

    int GetItemWeaponSpeed(lua_State* L)
    {
        double v = 0.0;
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isnumber(L, 1))
            if (const uint8_t* rec = ItemRecord(static_cast<uint32_t>(ArgInt(L, 1))))
                v = static_cast<double>(*reinterpret_cast<const int32_t*>(rec + 0xF4));
        AscLua::lua_pushnumber(L, v);
        return 1;
    }

    int GetQuestScaling(lua_State* L)
    {
        const uint8_t* rec = QuestRecord(static_cast<uint32_t>(ArgInt(L, 1)));
        if (!rec)
            return 0;
        PushBool(L, ((*reinterpret_cast<const uint32_t*>(rec + 0x50) >> 24) & 1) != 0);
        return 1;
    }

    int GetTitleForQuestID(lua_State* L)
    {
        const char* s = reinterpret_cast<const char*(__cdecl*)(uint32_t)>(0x5DEC70)(static_cast<uint32_t>(ArgInt(L, 1)));
        PushStr(L, s);
        return 1;
    }

    int GetRealmMaintenance(lua_State* L)   // realm-info object +0x40 then +0x18
    {
        const RealmInfo& r = RealmInfoSvc::Get();
        PushBool(L, r.gates[0] != 0 && r.flag14 != 0);
        return 1;
    }

    int CanShowAchievementToast(lua_State* L)
    {
        const uint8_t* row = ClientDbcRow(kAchievementDbc, static_cast<uint32_t>(ArgInt(L, 1)));
        PushBool(L, row && (*reinterpret_cast<const uint32_t*>(row + 0x24) & 0x400) == 0);
        return 1;
    }

    int Ascension_GetArenaBracketCutoffs(lua_State* L)   // FUN_10005da0's static list
    {
        static const int32_t kCutoffs[6] = {0, 1400, 1700, 2000, 2200, 2400};
        for (int32_t c : kCutoffs)
            AscLua::lua_pushinteger(L, c);
        return 6;
    }

    int ItemDbcField(lua_State* L, uint32_t off)
    {
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isnumber(L, 1))
            if (const uint8_t* row = ClientDbcRow(kItemDbc, static_cast<uint32_t>(ArgInt(L, 1))))
            {
                AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row + off));
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }
    int GetItemClassID(lua_State* L) { return ItemDbcField(L, 4); }
    int GetItemSubClassID(lua_State* L) { return ItemDbcField(L, 8); }

    int GetItemInventoryType(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = ClientDbcRow(kItemDbc, id);
        if (row) AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row + 0x18)); else AscLua::lua_pushnil(L);
        return 1;
    }

    int GetActiveMapName(lua_State* L)
    {
        const uint8_t* row = ClientDbcRow(kMapDbc, *reinterpret_cast<const uint32_t*>(0xBD088C));
        if (row) PushRowStr(L, row, 0x14); else AscLua::lua_pushnil(L);
        return 1;
    }

    int GetMapName(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = ClientDbcRow(kMapDbc, id);
        if (row) PushRowStr(L, row, 0x14); else AscLua::lua_pushnil(L);
        return 1;
    }

    int GetAreaName(lua_State* L)
    {
        const uint8_t* row = ClientDbcRow(kAreaDbc, static_cast<uint32_t>(ArgInt(L, 1)));
        if (row) PushRowStr(L, row, 0x2C); else AscLua::lua_pushnil(L);
        return 1;
    }

    int GetKeyringItemGUID(lua_State* L)   // player descriptor +0x7B8 + slot * 8, slots 1..31
    {
        uint8_t* d = PlayerDescriptors();
        if (d)
        {
            const int32_t slot = ArgInt(L, 1);
            if (static_cast<uint32_t>(slot - 1) < 0x1F)
            {
                const uint32_t lo = *reinterpret_cast<uint32_t*>(d + 0x7B8 + slot * 8);
                const uint32_t hi = *reinterpret_cast<uint32_t*>(d + 0x7BC + slot * 8);
                if (lo || hi)
                {
                    PushGuidString(L, lo, hi);
                    return 1;
                }
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // Quest log slots at player descriptor +0x278, 0x14 each (ASCENSION_QUEST_LOG_64: the 64 slots, 25..63 from
    // SMSG 0x8F7); the index comes from 0x5DEEB0.
    int GetQuestLogIndexByID(lua_State* L)
    {
        const int32_t quest = ArgInt(L, 1);
        uint8_t* d = PlayerDescriptors();
        if (!d)
            return 0;
#if ASCENSION_QUEST_LOG_64
        if (AscQuestLog64::QuestSlot(d, static_cast<uint32_t>(quest)) >= 0)
        {
            AscLua::lua_pushinteger(L, reinterpret_cast<int(__cdecl*)(int32_t)>(0x5DEEB0)(quest));
            return 1;
        }
#else
        for (uint32_t i = 0; i < 25; ++i)
        {
            const int32_t q = *reinterpret_cast<int32_t*>(d + 0x278 + i * 0x14);
            if (q != 0 && q == quest)
            {
                AscLua::lua_pushinteger(L, reinterpret_cast<int(__cdecl*)(int32_t)>(0x5DEEB0)(quest));
                return 1;
            }
        }
#endif
        AscLua::lua_pushinteger(L, 0);
        return 1;
    }

    // |1 - UNIT_MOD_CAST_SPEED| x 100, rounded up, never negative.
    int GetSpellHaste(lua_State* L)
    {
        double v = 0.0;
        if (uint8_t* d = PlayerDescriptors())
        {
            const float mod = *reinterpret_cast<float*>(d + 0x140);
            const double c = std::ceil(static_cast<double>(std::fabs(1.0f - mod) * 100.0f));
            const int32_t i = static_cast<int32_t>(c);
            v = static_cast<double>(i < 0 ? 0 : i);
        }
        AscLua::lua_pushnumber(L, v);
        return 1;
    }

    int IsPassiveSpellID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        uint8_t rec[0x2A8];
        const bool found = FetchSpell(id, rec);
        PushBool(L, found && ((*reinterpret_cast<uint32_t*>(rec + 0x10) >> 6) & 1) != 0);
        return 1;
    }

    int GetHonorableCombatZones(lua_State* L)
    {
        if (!g_honorableZonesSet)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscLua::lua_createtable(L, static_cast<int>(g_honorableZones.size()), 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < g_honorableZones.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(g_honorableZones[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int GetCharacterSelectionSortOrder(lua_State* L)
    {
        if (!g_sortOrderSet) AscLua::lua_pushnil(L); else PushStr(L, g_sortOrder.c_str());
        return 1;
    }

    // The first DungeonEncounterExtra row whose +0xC is the current LFG dungeon.
    int GetLFGBoss(lua_State* L)
    {
        if (g_lfgDungeon != 0)
        {
            AscDbc::Table& t = AscDbc::Get("DBFilesClient\\DungeonEncounterExtra.dbc");
            for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
            {
                const uint8_t* row = t.Row(id);
                if (row && AscDbc::Table::U32(row, 0xC) == g_lfgDungeon)
                {
                    AscLua::lua_pushinteger(L, static_cast<int>(AscDbc::Table::U32(row, 0)));
                    return 1;
                }
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // The frame's C object: rawget(frame, 0) -> userdata (0x84E670 / 0x84E1C0).
    void* FrameObject(lua_State* L)
    {
        reinterpret_cast<void(__cdecl*)(lua_State*, int, int)>(0x84E670)(L, 1, 0);
        void* obj = reinterpret_cast<void*(__cdecl*)(lua_State*, int)>(0x84E1C0)(L, -1);
        AscLua::lua_settop(L, -2);
        return obj;
    }

    // MovieFrame:SetVolume(v) (FUN_101aeca0): v clamped to [0, 1] (0x879710, thiscall on the frame object).
    int SetVolume(lua_State* L)
    {
        void* obj = FrameObject(L);
        float v = static_cast<float>(CheckNumber(L, 2));
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        reinterpret_cast<void(__thiscall*)(void*, float)>(0x879710)(obj, v);
        return 0;
    }

    // CastSpecialSpell(spell): a profession spell (an effect 0xA2, or a SkillLineAbility row whose
    // SkillLine category is 11) is cast as CMSG_CAST_SPELL 0x12E with the DLL's own cast counter and
    // the DLL-global target guid DAT_10bdb230 (never written: empty).
    int CastSpecialSpell(lua_State* L)
    {
        const uint32_t spell = static_cast<uint32_t>(ArgInt(L, 1));
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return 0;
        bool special = false;
        for (uint32_t i = 0; i < 3 && !special; ++i)
            special = *reinterpret_cast<uint32_t*>(rec + 0x11C + i * 4) == 0xA2;   // Effect[3], field 71
        if (!special)
        {
            const uint32_t minId = *reinterpret_cast<const uint32_t*>(kSkillLineAbilityDbc + 0x10);
            const uint32_t maxId = *reinterpret_cast<const uint32_t*>(kSkillLineAbilityDbc + 0x0C);
            for (uint32_t id = minId; id <= maxId && !special; ++id)
            {
                const uint8_t* sla = ClientDbcRow(kSkillLineAbilityDbc, id);
                if (!sla || *reinterpret_cast<const uint32_t*>(sla + 8) != spell)
                    continue;
                const uint8_t* line = ClientDbcRow(kSkillLineDbc, *reinterpret_cast<const uint32_t*>(sla + 4));
                special = line && *reinterpret_cast<const uint32_t*>(line + 4) == 0xB;
            }
        }
        if (!special)
            return 0;
        Packet p(0x12E);
        p.U8(g_castCount).U32(spell).U8(0).U32(0);
        p.U8(0);   // packed guid mask of the empty target guid
        p.Send();
        g_castCount = static_cast<uint8_t>(g_castCount % 0xFF + 1);
        return 0;
    }

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    void __cdecl OnHonorableZones(void*, uint32_t, uint32_t, CDataStore* p)   // 0x768
    {
        g_honorableZones.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_honorableZones.push_back(Read<uint32_t>(p));
        g_honorableZonesSet = true;
        AscRuntime::Signal("HONORABLE_COMBAT_ZONES_PAYLOAD");
    }

    void __cdecl OnSortOrder(void*, uint32_t, uint32_t, CDataStore* p)   // 0x76F
    {
        g_sortOrder = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        p->m_read += static_cast<int32_t>(g_sortOrder.size() + 1);
        g_sortOrderSet = true;
    }

    void __cdecl OnLfgUpdateCustom(void*, uint32_t, uint32_t, CDataStore* p)   // 0x66D
    {
        const uint32_t id = Read<uint32_t>(p);
        const bool changed = id != g_lfgDungeon;
        g_lfgDungeon = id;
        if (changed)
            AscRuntime::Signal("CURRENT_LFG_DUNGEON_ID_CHANGED");
    }

    void Init()
    {
        sDC.AddPacketHandler(0x768, CNetClientCustomPacket((void*)&OnHonorableZones, nullptr));
        sDC.AddPacketHandler(0x76F, CNetClientCustomPacket((void*)&OnSortOrder, nullptr));
        sDC.AddPacketHandler(0x66D, CNetClientCustomPacket((void*)&OnLfgUpdateCustom, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "Ascension_GetArenaBracketCutoffs", Ascension_GetArenaBracketCutoffs},
        {nullptr, "CanShowAchievementToast", CanShowAchievementToast},
        {nullptr, "CastSpecialSpell", CastSpecialSpell},
        {nullptr, "GetActiveMapName", GetActiveMapName},
        {nullptr, "GetAreaName", GetAreaName},
        {nullptr, "GetCharacterSelectionSortOrder", GetCharacterSelectionSortOrder},
        {nullptr, "GetHonorableCombatZones", GetHonorableCombatZones},
        {nullptr, "GetItemClassID", GetItemClassID},
        {nullptr, "GetItemInventoryType", GetItemInventoryType},
        {nullptr, "GetItemSetID", GetItemSetID},
        {nullptr, "GetItemSubClassID", GetItemSubClassID},
        {nullptr, "GetItemTemplateBonding", GetItemTemplateBonding},
        {nullptr, "GetItemWeaponSpeed", GetItemWeaponSpeed},
        {nullptr, "GetKeyringItemGUID", GetKeyringItemGUID},
        {nullptr, "GetLFGBoss", GetLFGBoss},
        {nullptr, "GetMapName", GetMapName},
        {nullptr, "GetQuestLogIndexByID", GetQuestLogIndexByID},
        {nullptr, "GetQuestScaling", GetQuestScaling},
        {nullptr, "GetRealmMaintenance", GetRealmMaintenance},
        {nullptr, "GetSpellHaste", GetSpellHaste},
        {nullptr, "GetTitleForQuestID", GetTitleForQuestID},
        {nullptr, "IsPassiveSpellID", IsPassiveSpellID},
        {nullptr, "LoadWDBCache", LoadWDBCache},
        {nullptr, "SaveWDBCache", SaveWDBCache},
        {"#MovieFrame", "SetVolume", SetVolume},   // a MovieFrame method (FUN_101aeca0 via the registrar hook on 0x970610)
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
