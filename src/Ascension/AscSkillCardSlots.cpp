// C_SkillCard (0x102F5290..0x102F79A0): the skill cards slotted per specialization.
//
// Slot manager (FUN_102f5140 -> 0x10BE3AC4): spec -> type (SKILL_CARD_* 0..8) -> slots of 9 bytes
// {u32 card id, u32 rank, u8 blocked}. Module init 0x102F50C0: SMSG 0x623 SET_SKILL_CARD_RESULT (str),
// 0x624 the whole list, 0x625 one slot; glue reset 0x102F41C0. CMSG 0x622 {u32 spec, str type,
// u32 slot, u32 card} sets (card 0 = remove) a slot.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSkillCard.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;
using AscSkillCard::Card;

namespace AscWildcardRolls { uint32_t StartingCount(); }   // FUN_10154f50

namespace
{
    struct Slot { uint32_t cardId = 0, rank = 0; uint8_t blocked = 0; };
    std::vector<std::vector<std::vector<Slot>>> g_specs;   // 0x10BE3AC4

    const uint32_t kTypeCount = 9;               // FUN_102f41f0 / 0x10B5752C: types 0..8
    const uint32_t kLevels[3] = {0, 25, 40};     // 0x10B57550: the level each default slot opens at

    uint32_t Spec() { const int s = AscGameMode::ActiveSpecIndex(); return s < 0 ? 0xFFFFFFFF : static_cast<uint32_t>(s); }

    const Slot* SlotAt(uint32_t spec, uint32_t type, uint32_t i)   // FUN_102f43c0
    {
        if (spec >= g_specs.size() || type >= g_specs[spec].size() || i >= g_specs[spec][type].size())
            return nullptr;
        return &g_specs[spec][type][i];
    }

    uint32_t Count(uint32_t spec, uint32_t type)   // FUN_102f4440
    {
        return (spec < g_specs.size() && type < g_specs[spec].size()) ? static_cast<uint32_t>(g_specs[spec][type].size()) : 0;
    }

    // FUN_102f51a0: a slot is blocked when its starter-ness disagrees with the build's starting phase,
    // when the server blocked it, or (default slots 1-3) below their level.
    bool Blocked(uint32_t spec, uint32_t type, uint32_t i)
    {
        const bool starter = type == 2 || type == 3;
        bool phase = false;
        if (const AscCA::Build* b = AscCA::ActiveBuild())
        {
            if (b->wildcard)
                phase = AscCA::AbilityCount(*b) < AscWildcardRolls::StartingCount();   // FUN_10159210
            else if (b->draft)
                phase = b->GlobalAE(0) < (((AscGameMode::Mode() >> 7) & 1) ? 0x12u : 8u);
        }
        const Slot* s = SlotAt(spec, type, i);
        if (starter != phase || (s && s->blocked))
            return true;
        if ((type == 0 || type == 1) && i < 3 && kLevels[i] != 0)
        {
            uint8_t* p = ActivePlayer();
            if (!p)
                return true;
            if (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(p + 8) + 0xD8) < kLevels[i])
                return true;
        }
        return false;
    }

    int32_t ConfigInt(const char* name, int32_t fallback)
    {
        const int32_t* v = AscConfig::Int(name);
        return v ? *v : fallback;
    }

    // (type name, 1-based slot) -- FUN_100e96d0 then FUN_102f3f50.
    bool ReadTypeSlot(lua_State* L, int& type, uint32_t& slot)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return false;
        type = AscSkillCard::TypeIndex(CheckString(L, 1));
        slot = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        return type >= 0 && slot != 0;
    }

    int GetCardCount(lua_State* L)   // FUN_102f53e0
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        const int t = AscSkillCard::TypeIndex(s);
        if (t < 0)
            return 0;
        AscLua::lua_pushinteger(L, static_cast<int>(Count(Spec(), static_cast<uint32_t>(t))));
        return 1;
    }

    int GetCardAtIndex(lua_State* L)   // FUN_102f5290: card id, rank
    {
        int t;
        uint32_t i;
        if (!ReadTypeSlot(L, t, i))
            return 0;
        const Slot* s = SlotAt(Spec(), static_cast<uint32_t>(t), i - 1);
        AscLua::lua_pushinteger(L, static_cast<int>(s ? s->cardId : 0));
        AscLua::lua_pushinteger(L, static_cast<int>(s ? s->rank : 0));
        return 2;
    }

    int GetSkillCardInfoAtIndex(lua_State* L)   // FUN_102f6970
    {
        int t;
        uint32_t i;
        if (!ReadTypeSlot(L, t, i))
            return 0;
        const Slot* s = SlotAt(Spec(), static_cast<uint32_t>(t), i - 1);
        const Card* c = AscSkillCard::ByCardRank(s ? s->cardId : 0, s ? s->rank : 0);
        if (c)
            AscSkillCard::PushCard(L, *c);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetCardRankAtIndex(lua_State* L)   // FUN_102f5550
    {
        int t;
        uint32_t i;
        if (!ReadTypeSlot(L, t, i))
            return 0;
        const Slot* s = SlotAt(Spec(), static_cast<uint32_t>(t), i - 1);
        AscLua::lua_pushinteger(L, static_cast<int>(s ? s->rank : 0));
        return 1;
    }

    int IsCardAtIndexActive(lua_State* L)   // FUN_102f6b90: slotted and not blocked
    {
        int t;
        uint32_t i;
        if (!ReadTypeSlot(L, t, i))
            return 0;
        const Slot* s = SlotAt(Spec(), static_cast<uint32_t>(t), i - 1);
        PushBool(L, s && s->cardId != 0 && !Blocked(Spec(), static_cast<uint32_t>(t), i - 1));
        return 1;
    }

    int IsCardAtIndexBlocked(lua_State* L)   // FUN_102f6cd0: the server's flag only
    {
        int t;
        uint32_t i;
        if (!ReadTypeSlot(L, t, i))
            return 0;
        const Slot* s = SlotAt(Spec(), static_cast<uint32_t>(t), i - 1);
        PushBool(L, s && s->blocked);
        return 1;
    }

    // FUN_102f5c80: the realm's slot count for the type (felforged mode: mode bit 7).
    int GetMaxCardCount(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        const int t = AscSkillCard::TypeIndex(s);
        if (t < 0)
            return 0;
        const bool fel = (AscGameMode::Mode() >> 7) & 1;
        int32_t n = 0;
        switch (t)
        {
        case 0: case 1:
            n = fel ? ConfigInt("CONFIG_RANDOM_MODE_MAX_DEFAULT_FELFORGED_SKILL_CARDS", 3)
                    : ConfigInt("CONFIG_RANDOM_MODE_MAX_DEFAULT_NORMAL_SKILL_CARDS", 2);
            break;
        case 2: case 3:
            n = 2;
            break;
        case 4: case 5:
            n = fel ? ConfigInt("CONFIG_RANDOM_MODE_MAX_LUCKY_FELFORGED_SKILL_CARDS", 3)
                    : ConfigInt("CONFIG_RANDOM_MODE_MAX_LUCKY_NORMAL_SKILL_CARDS", 2);
            break;
        case 6: case 7:
            n = fel ? ConfigInt("CONFIG_RANDOM_MODE_MAX_TALENT_FELFORGED_SKILL_CARDS", 3)
                    : ConfigInt("CONFIG_RANDOM_MODE_MAX_TALENT_NORMAL_SKILL_CARDS", 2);
            break;
        default:
            n = 0;
        }
        AscLua::lua_pushinteger(L, n);
        return 1;
    }

    void SendSet(const char* type, uint32_t slot, uint32_t card)   // CMSG 0x622
    {
        Packet(0x622).U32(Spec()).Str(type).U32(slot).U32(card).Send();
    }

    int SetCardAtIndex(lua_State* L)   // FUN_102f79a0: (type, slot, card)
    {
        if (!ValidateInput(L, {STRING, NUMBER, NUMBER}))
            return 0;
        const std::string type = CheckString(L, 1);
        const int32_t slot = ToInt(CheckNumber(L, 2));
        const int32_t card = ToInt(CheckNumber(L, 3));
        if (slot == 0)
            return 0;
        SendSet(type.c_str(), static_cast<uint32_t>(slot - 1), static_cast<uint32_t>(card));
        PushBool(L, true);
        return 1;
    }

    int RemoveCardAtIndex(lua_State* L)   // FUN_102f77f0: (type, slot)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string type = CheckString(L, 1);
        const int32_t slot = ToInt(CheckNumber(L, 2));
        if (slot == 0)
            return 0;
        SendSet(type.c_str(), static_cast<uint32_t>(slot - 1), 0);
        PushBool(L, true);
        return 1;
    }

    // (card [, rank]) with rank 1 by default -- FUN_100b88c0, else FUN_1008ba00.
    bool ReadCardRank(lua_State* L, uint32_t& card, uint32_t& rank)
    {
        int32_t a, b;
        if (ReadInt2(L, a, b))
        {
            card = static_cast<uint32_t>(a);
            rank = static_cast<uint32_t>(b);
            return true;
        }
        rank = 1;
        return ReadNumber(L, card);
    }

    int GetSkillCardInfo(lua_State* L)   // FUN_102f68f0
    {
        uint32_t card, rank;
        if (!ReadCardRank(L, card, rank))
            return 0;
        const Card* c = AscSkillCard::ByCardRank(card, rank);
        if (!c)
            return 0;
        AscSkillCard::PushCard(L, *c);
        return 1;
    }

    int GetSkillCardQuality(lua_State* L)   // FUN_102f6af0
    {
        uint32_t card, rank;
        if (!ReadCardRank(L, card, rank))
            return 0;
        const Card* c = AscSkillCard::ByCardRank(card, rank);
        if (c)
            PushStr(L, c->quality.c_str());
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_102f6e00 / FUN_102f7300: an unblocked slot of the active spec whose card matches.
    // Returns (found, the slot's type name -- SKILL_CARD_MAX when none, found).
    template <class Match>
    int FindCarded(lua_State* L, Match match)
    {
        const uint32_t spec = Spec();
        bool found = false;
        uint32_t type = 8;
        for (uint32_t t = 0; t < 8 && !found; ++t)   // FUN_102f41f0: types 0..7
            for (uint32_t i = 0; i < Count(spec, t); ++i)
            {
                const Slot* s = SlotAt(spec, t, i);
                const Card* c = s ? AscSkillCard::ByCardRank(s->cardId, s->rank) : nullptr;
                if (Blocked(spec, t, i) || !c || !match(*c))
                    continue;
                found = true;
                type = t;
                break;
            }
        PushBool(L, found);
        const char* name = AscSkillCard::TypeName(type);
        PushStr(L, name ? name : "");
        PushBool(L, found);
        return 3;
    }

    int IsCardedID(lua_State* L)   // an entry id (the CharacterAdvancement row listing the card's spell)
    {
        uint32_t entry;
        if (!ReadNumber(L, entry))
            return 0;
        return FindCarded(L, [entry](const Card& c) {
            AscCA::Row r = AscCA::RowBySpell(c.spell);   // FUN_101498d0
            return r && AscCA::RowU32(r, 0) == entry;
        });
    }

    int IsCardedSpellID(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return 0;
        return FindCarded(L, [spell](const Card& c) { return c.spell == spell; });
    }

    int GetCardID(lua_State* L)   // handler_GetCardID: (item) -> card id, rank
    {
        uint32_t item;
        if (!ReadNumber(L, item))
            return 0;
        const Card* c = AscSkillCard::ByItem(item);
        if (!c)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 2;
        }
        AscLua::lua_pushinteger(L, static_cast<int>(c->cardId));
        AscLua::lua_pushinteger(L, static_cast<int>(c->rank));
        return 2;
    }

    int GetCardSpellID(lua_State* L)   // FUN_102f5670: (card, rank)
    {
        int32_t card, rank;
        if (!ReadInt2(L, card, rank))
            return 0;
        const Card* c = AscSkillCard::ByCardRank(static_cast<uint32_t>(card), static_cast<uint32_t>(rank));
        if (!c)
            AscLua::lua_pushnil(L);
        else
            AscLua::lua_pushinteger(L, static_cast<int>(c->spell));
        return 1;
    }

    // ---- Hand of Fate / Scroll of Fortune quests ------------------------------------------------------
    // The DLL's quest store (FUN_10212ad0 -> FUN_102123e0): Quest.dbc in the MemoryBridge store, grouped
    // by byte offsets 8 / 12 / 16 -- see AscBridgeStore.
    std::vector<const uint8_t*> QuestsInGroup(uint32_t group) { return AscBridgeStore::QuestsInGroup(group); }

    void PushIntList(lua_State* L, const char* key, const uint8_t* r, uint32_t off, uint32_t n)   // FUN_100b9d20 + array
    {
        AscLua::lua_pushstring(L, key);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < n; ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(AscDbc::Table::U32(r, off + i * 4)));
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    // One array of every quest of each spec's group (group(spec)), each {ID, MinLevel,
    // RequiredItemCount, RequiredItemId, RewardAmount, RewardItem, Specialization (0-based), Leveling:
    // one of the quest's groups is in `leveling`}.
    template <class GroupOf>
    int PushQuestGroups(lua_State* L, GroupOf groupOf, uint32_t levelingFirst)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        int n = 0;
        for (uint32_t spec = 0; spec < 20; ++spec)   // DAT_10be3a64: 20 groups
        {
            uint32_t group;
            if (!groupOf(spec, group))
                continue;
            for (const uint8_t* r : QuestsInGroup(group))
            {
                AscLua::lua_pushinteger(L, ++n);
                AscLua::lua_createtable(L, 0, 0);
                auto num = [L](const char* k, uint32_t v) { AscLua::lua_pushstring(L, k); AscLua::lua_pushinteger(L, static_cast<int>(v)); AscLua::lua_settable(L, -3); };
                num("ID", AscDbc::Table::U32(r, 0));
                num("MinLevel", AscDbc::Table::U32(r, 0x14));
                PushIntList(L, "RequiredItemCount", r, 0x50, 6);
                PushIntList(L, "RequiredItemId", r, 0x38, 6);
                PushIntList(L, "RewardAmount", r, 0x28, 4);
                PushIntList(L, "RewardItem", r, 0x18, 4);
                num("Specialization", spec);
                bool leveling = false;
                for (uint32_t off : {8u, 12u, 16u})
                {
                    const uint32_t g = AscDbc::Table::U32(r, off);
                    leveling = leveling || (g != 0 && g >= levelingFirst && g < levelingFirst + 20);
                }
                AscLua::lua_pushstring(L, "Leveling");
                PushBool(L, leveling);
                AscLua::lua_settable(L, -3);
                AscLua::lua_settable(L, -3);
            }
        }
        return 1;
    }

    // 0x102F56F0: groups 0x65 + spec (FUN_1007a0b0), leveling groups 0xC9.. (FUN_1007a2b0).
    int GetHandOfFateQuests(lua_State* L)
    {
        return PushQuestGroups(L, [](uint32_t spec, uint32_t& g) { g = 0x65 + spec; return true; }, 0xC9);
    }

    // 0x102F6360: groups 0x191 + spec (FUN_1007a1b0), leveling groups 0x1F5.. (FUN_1007a3b0).
    int GetScrollOfFortuneQuests(lua_State* L)
    {
        return PushQuestGroups(L, [](uint32_t spec, uint32_t& g) { g = 0x191 + spec; return true; }, 0x1F5);
    }

    // ---- SMSG -------------------------------------------------------------------------------------
    uint32_t U32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    std::string Str(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    void __cdecl OnSetResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x623
    {
        const std::string r = Str(p);
        AscRuntime::Signal("SET_SKILL_CARD_RESULT", "%s", r.c_str());
    }

    // 0x624 (FUN_102f4ab0): u32 specs, each 9 types x {u32 n, n x {u32 card, u32 rank, u8 blocked}}.
    void __cdecl OnList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_specs.clear();
        const uint32_t specs = U32(p);
        g_specs.resize(specs);
        for (uint32_t s = 0; s < specs; ++s)
        {
            g_specs[s].resize(kTypeCount);
            for (uint32_t t = 0; t < kTypeCount; ++t)
            {
                const uint32_t n = U32(p);
                g_specs[s][t].resize(n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    Slot& e = g_specs[s][t][i];
                    e.cardId = U32(p);
                    e.rank = U32(p);
                    e.blocked = static_cast<uint8_t>(p->m_buffer[p->m_read]);
                    p->m_read += 1;
                }
            }
        }
    }

    // 0x625 (FUN_102f4660): u32 spec, str type, u32 slot, u32 card, u32 rank, u8 blocked.
    void __cdecl OnUpdate(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t spec = U32(p);
        const std::string typeName = Str(p);
        const uint32_t slot = U32(p);
        const uint32_t card = U32(p);
        const uint32_t rank = U32(p);
        const uint8_t blocked = static_cast<uint8_t>(p->m_buffer[p->m_read]);
        p->m_read += 1;
        const int t = AscSkillCard::TypeIndex(typeName);
        if (t < 0)
            return;
        if (g_specs.size() <= spec)
            g_specs.resize(spec + 1);
        if (g_specs[spec].size() <= static_cast<uint32_t>(t))
            g_specs[spec].resize(static_cast<uint32_t>(t) + 1);
        std::vector<Slot>& v = g_specs[spec][static_cast<uint32_t>(t)];
        if (v.size() <= slot)
            v.resize(slot + 1);
        const Slot old = v[slot];
        v[slot] = {card, rank, blocked};
        if (old.cardId != card)
            AscRuntime::Signal("SKILL_CARD_UPDATED", "%u%s%u%u%u%u%u", spec + 1, typeName.c_str(), slot + 1, old.cardId,
                old.rank, card, rank);
        if (old.blocked != blocked)
            AscRuntime::Signal(blocked ? "SKILL_CARD_BLOCKED" : "SKILL_CARD_UNBLOCKED", "%u%s%u", spec + 1, typeName.c_str(),
                slot + 1);
    }

    void OnGlueScreen() { g_specs.clear(); }   // 0x102F41C0

    void Init()
    {
        sDC.AddPacketHandler(0x623, CNetClientCustomPacket((void*)&OnSetResult, nullptr));
        sDC.AddPacketHandler(0x624, CNetClientCustomPacket((void*)&OnList, nullptr));
        sDC.AddPacketHandler(0x625, CNetClientCustomPacket((void*)&OnUpdate, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_SkillCard", "GetCardCount", GetCardCount},
        {"C_SkillCard", "GetCardAtIndex", GetCardAtIndex},
        {"C_SkillCard", "GetSkillCardInfoAtIndex", GetSkillCardInfoAtIndex},
        {"C_SkillCard", "GetCardRankAtIndex", GetCardRankAtIndex},
        {"C_SkillCard", "IsCardAtIndexActive", IsCardAtIndexActive},
        {"C_SkillCard", "IsCardAtIndexBlocked", IsCardAtIndexBlocked},
        {"C_SkillCard", "GetMaxCardCount", GetMaxCardCount},
        {"C_SkillCard", "SetCardAtIndex", SetCardAtIndex},
        {"C_SkillCard", "RemoveCardAtIndex", RemoveCardAtIndex},
        {"C_SkillCard", "GetSkillCardInfo", GetSkillCardInfo},
        {"C_SkillCard", "GetSkillCardQuality", GetSkillCardQuality},
        {"C_SkillCard", "IsCardedID", IsCardedID},
        {"C_SkillCard", "IsCardedSpellID", IsCardedSpellID},
        {"C_SkillCard", "GetCardID", GetCardID},
        {"C_SkillCard", "GetCardSpellID", GetCardSpellID},
        // Bare globals: FUN_102f7c30 registers these two after the C_SkillCard table.
        {nullptr, "GetHandOfFateQuests", GetHandOfFateQuests},
        {nullptr, "GetScrollOfFortuneQuests", GetScrollOfFortuneQuests},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscSkillCard
{
    // FUN_102f4f70: any slotted, unblocked card in the spec (types 0..8).
    bool AnyUnblocked(uint32_t spec)
    {
        for (uint32_t t = 0; t < kTypeCount; ++t)
            for (uint32_t i = 0; i < Count(spec, t); ++i)
            {
                const Slot* s = SlotAt(spec, t, i);
                if (s && s->cardId != 0 && !Blocked(spec, t, i))
                    return true;
            }
        return false;
    }
}

namespace AscQuestStore
{
    std::vector<const uint8_t*> InGroup(uint32_t group) { return QuestsInGroup(group); }
}
