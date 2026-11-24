// C_TemporalContracts (TemporalContractMgr, static 0x10D3C1F0 via FUN_10339160), transcribed as the
// decompile has it.
//
// Manager: +0x04 an unordered_map, +0x24 vector of contract types (100 bytes each), +0x30..+0x40 a
// deque of pending activations. Type: str Name +0, u32 RemainingCompletions +0x18, MoneyCost +0x1C,
// TokenCost +0x20, vector<u32> quest groups +0x24, MinimumLevel +0x30, str display name +0x34, str
// content type +0x4C. The code that fills the type vector sits in the VM-protected part of the DLL
// and is not recoverable, so the list stays empty here and every reader returns its empty shape.
//
// Module init FUN_10339120: the library, glue reset FUN_10337120 (types and queue cleared),
// TEMPORAL_CONTRACTS_UPDATED, enter-world FUN_1033b820 (a 50 ms timer, 0x403370, that sends one
// queued activation per tick through FUN_10339320) and glue-Lua init FUN_103392f0 (cancel it).
//   CMSG 0x55B: u32 length, the type name's bytes (no NUL), u8 flag, u32 count.
//
// Quest records are the client quest cache (0x67DE90 on 0xC5DA48): +0x10 ZoneOrSort, +0x34 reward
// money, +0x44 honor (scaled by RealmInfo +0x10), +0x54 reward items x4, +0x64 amounts x4, +0x24A0
// arena points. Quest groups come from the DLL quest store (AscQuestStore, Quest.dbc).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscQuestAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace AscScript;

namespace AscCallboard
{
    bool ForEachCompletion(const std::string* contentType, void (*fn)(const std::string& type, void* ctx), void* ctx);
}
namespace AscQuestStore
{
    std::vector<const uint8_t*> InGroup(uint32_t group);
}

namespace
{
#include <Ascension/AscQuestCategories.generated.inc>

    struct ContractType
    {
        std::string name;                                            // +0x00
        uint32_t remaining = 0, moneyCost = 0, tokenCost = 0;        // +0x18 / +0x1C / +0x20
        std::vector<uint32_t> groups;                                // +0x24
        uint32_t minimumLevel = 0;                                   // +0x30
        std::string displayName;                                     // +0x34
        std::string contentType;                                     // +0x4C
    };
    struct Activation { std::string type; uint8_t flag; uint32_t count; };   // 0x20 bytes

    std::vector<ContractType> g_types;   // +0x24
    std::deque<Activation> g_queue;      // +0x30
    uint32_t g_timer = 0;                // 0x10D3C1DC

    const uint8_t* QuestRecord(uint32_t id)   // 0x67DE90 on the quest cache 0xC5DA48
    {
        return static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67DE90)(
            reinterpret_cast<void*>(0xC5DA48), id, nullptr, nullptr, nullptr, 0));
    }
    uint32_t U32(const uint8_t* p, uint32_t off)
    {
        uint32_t v;
        memcpy(&v, p + off, 4);
        return v;
    }

    // FUN_10212c10: the first quest of the group the client has in its quest cache.
    const uint8_t* FirstCachedQuest(uint32_t group)
    {
        for (const uint8_t* r : AscQuestStore::InGroup(group))
            if (QuestRecord(U32(r, 0)))
                return r;
        return nullptr;
    }

    // FUN_103376a0: the first non-zero group of a type with a cached quest.
    const uint8_t* FirstCachedQuest(const ContractType& t)
    {
        for (uint32_t g : t.groups)
            if (g != 0)
                if (const uint8_t* r = FirstCachedQuest(g))
                    return r;
        return nullptr;
    }

    const ContractType* FindByName(const std::string& name)   // FUN_10338f20
    {
        for (const ContractType& t : g_types)
            if (t.name == name)
                return &t;
        return nullptr;
    }

    // FUN_10338fe0: the priced types (MoneyCost and TokenCost both set) of a content type.
    std::vector<const ContractType*> ByContentType(const std::string& contentType)
    {
        std::vector<const ContractType*> out;
        for (const ContractType& t : g_types)
            if (t.moneyCost != 0 && t.tokenCost != 0 && t.contentType == contentType)
                out.push_back(&t);
        return out;
    }

    // FUN_10337620
    void Enqueue(const std::string& type, bool flag, uint32_t count)
    {
        g_queue.push_back({type, static_cast<uint8_t>(flag), count});
    }

    // FUN_10339320: one activation per tick.
    void SendNext()
    {
        if (g_queue.empty())
            return;
        const Activation a = g_queue.front();
        g_queue.pop_front();
        Packet(0x55B).U32(static_cast<uint32_t>(a.type.size())).Data(a.type.data(), static_cast<uint32_t>(a.type.size())).U8(a.flag).U32(a.count).Send();
    }

    int __cdecl OnTick(void*)   // 0x1033B840
    {
        SendNext();
        g_timer = AscRuntime::Schedule(0x32, OnTick, nullptr);
        return 0;
    }

    // FUN_1033b820. EnterWorld repeats on /reload and far teleports: cancel the running chain
    // before re-arming so world entries do not stack ticks.
    void OnEnterWorld()
    {
        if (g_timer)
        {
            AscRuntime::Cancel(g_timer, OnTick, nullptr);
            g_timer = 0;
        }
        g_timer = AscRuntime::Schedule(0x32, OnTick, nullptr);
    }

    void OnGlue()   // FUN_10337120 + FUN_103392f0
    {
        g_types.clear();
        g_queue.clear();
        if (g_timer)
        {
            AscRuntime::Cancel(g_timer, OnTick, nullptr);
            g_timer = 0;
        }
    }

    // ---- Lua writers ----------------------------------------------------------------------------------
    void SetInt(lua_State* L, const char* k, int32_t v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushinteger(L, v);
        AscLua::lua_settable(L, -3);
    }
    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushstring(L, v.c_str());
        AscLua::lua_settable(L, -3);
    }
    // The quests of every non-zero group, one array numbered across groups.
    void PushQuests(lua_State* L, const ContractType* t)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        if (!t)
            return;
        int n = 1;
        for (uint32_t g : t->groups)
            if (g != 0)
                for (const uint8_t* r : AscQuestStore::InGroup(g))
                {
                    AscLua::lua_pushinteger(L, n++);
                    AscLua::lua_pushinteger(L, static_cast<int32_t>(U32(r, 0)));
                    AscLua::lua_settable(L, -3);
                }
    }
    int32_t ScaledHonor(uint32_t raw)   // cvt u32 -> float * RealmInfo +0x10, truncated (0x10ae6220)
    {
        return static_cast<int32_t>(static_cast<float>(static_cast<double>(raw)) * RealmInfoSvc::Get().rate1);
    }
    std::string ArgString(lua_State* L, int idx)   // FUN_100bf440
    {
        const char* s = AscLua::lua_tolstring(L, idx, nullptr);
        return s ? s : "";
    }
    bool IsBoolArg(lua_State* L, int idx)   // nil, boolean or number
    {
        const int t = AscLua::lua_type(L, idx);
        return t == 0 || t == 1 || t == 3;
    }

    struct Totals
    {
        uint32_t money = 0, arena = 0, honor = 0, cachePoints = 0;
        std::vector<std::pair<uint32_t, uint32_t>> items;   // FUN_10336aa0: the item map in list order
        void AddItem(uint32_t item, uint32_t amount, bool accumulate)
        {
            for (auto& e : items)
                if (e.first == item)
                {
                    e.second = accumulate ? e.second + amount : amount;
                    return;
                }
            items.emplace_back(item, amount);
        }
    };
    // Adds one cached quest's rewards, each times `times`.
    void AddQuest(Totals& t, const uint8_t* questRow, uint32_t times, bool accumulate)
    {
        const uint8_t* rec = QuestRecord(U32(questRow, 0));
        if (!rec)
            return;
        t.arena += U32(rec, 0x24A0) * times;
        t.money += U32(rec, 0x34) * times;
        t.honor += U32(rec, 0x44) * times;
        for (uint32_t i = 0; i < 4; ++i)
        {
            const uint32_t item = U32(rec, 0x54 + i * 4);
            const uint32_t amount = U32(rec, 0x64 + i * 4) * times;
            if (item != 0 && amount != 0)
                t.AddItem(item, amount, accumulate);
        }
        if (const AscQuestAddon::Record* a = AscQuestAddon::Get(U32(questRow, 0)))
            t.cachePoints += a->points * times;
    }
    void PushTotals(lua_State* L, const Totals& t)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "RewardMoney", static_cast<int32_t>(t.money));
        SetInt(L, "RewardArenaPoints", static_cast<int32_t>(t.arena));
        SetInt(L, "RewardHonor", ScaledHonor(t.honor));
        AscLua::lua_pushstring(L, "RewardItems");
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < t.items.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int32_t>(t.items[i].first));
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "RewardAmount");
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < t.items.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int32_t>(t.items[i].second));
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "CallboardCachePoints");
        AscLua::lua_pushnumber(L, static_cast<double>(t.cachePoints));
        AscLua::lua_settable(L, -3);
    }

    // ---- bindings -------------------------------------------------------------------------------------
    // FUN_10339ff0: {[Name] = {remainingCompletions, moneyCost, tokenCost, minimumLevel, name,
    // quests}} for every type with completions left and both costs set.
    int GetCompletableTemporalContracts(lua_State* L)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const ContractType& t : g_types)
        {
            if (t.remaining == 0 || t.moneyCost == 0 || t.tokenCost == 0)
                continue;
            AscLua::lua_pushstring(L, t.name.c_str());
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetInt(L, "remainingCompletions", static_cast<int32_t>(t.remaining));
            SetInt(L, "moneyCost", static_cast<int32_t>(t.moneyCost));
            SetInt(L, "tokenCost", static_cast<int32_t>(t.tokenCost));
            SetInt(L, "minimumLevel", static_cast<int32_t>(t.minimumLevel));
            SetStr(L, "name", t.displayName);
            AscLua::lua_pushstring(L, "quests");
            PushQuests(L, &t);
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_103396a0: (type, flag [, count]) -- queued only while nothing else is pending.
    int ActivateTemporalContract(lua_State* L)
    {
        if (AscLua::lua_gettop(L) > 1 && AscLua::lua_isstring(L, 1) && IsBoolArg(L, 2) && g_queue.empty())
        {
            const std::string type = ArgString(L, 1);
            const bool flag = AscLua::lua_toboolean(L, 2) != 0;
            uint32_t count = 0;
            if (AscLua::lua_gettop(L) >= 3 && AscLua::lua_isnumber(L, 3))
                count = static_cast<uint32_t>(ToInt(CheckNumber(L, 3)));
            Enqueue(type, flag, count);
            PushBool(L, true);
            return 1;
        }
        PushBool(L, false);
        return 1;
    }

    // FUN_1033a780: (type) -> {remainingCompletions, moneyCost, tokenCost, quests}, zeros when unknown.
    int GetTemporalContractInfo(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1 || !AscLua::lua_isstring(L, 1))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const ContractType* t = FindByName(ArgString(L, 1));
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "remainingCompletions", t ? static_cast<int32_t>(t->remaining) : 0);
        SetInt(L, "moneyCost", t ? static_cast<int32_t>(t->moneyCost) : 0);
        SetInt(L, "tokenCost", t ? static_cast<int32_t>(t->tokenCost) : 0);
        AscLua::lua_pushstring(L, "quests");
        PushQuests(L, t);
        AscLua::lua_settable(L, -3);
        return 1;
    }

    // FUN_10339d90: (type) -> the type's quest ids, or nil.
    int GetCompletableQuestsByCategory(lua_State* L)
    {
        const ContractType* t = nullptr;
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isstring(L, 1))
            t = FindByName(ArgString(L, 1));
        if (!t)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushQuests(L, t);
        return 1;
    }

    // FUN_103376d0: the built-in id-range table; the second name applies from expansion 1 on.
    const char* BuiltinCategory(uint32_t quest)
    {
        for (const auto& c : kQuestCategories)
            if (quest >= c.first && quest <= c.last)
                return RealmInfoSvc::Get().ruleset >= 1 ? c.tbc : c.name;
        return nullptr;
    }

    // handler_GetQuestCategory (0x1033a280): (quest) -> the built-in category, else the first type
    // owning one of the quest's groups, else nil.
    int GetQuestCategory(lua_State* L)
    {
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isnumber(L, 1))
        {
            const uint32_t quest = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 1)));
            if (const char* c = BuiltinCategory(quest))
            {
                AscLua::lua_pushstring(L, c);
                return 1;
            }
            if (const uint8_t* row = AscBridgeStore::QuestById(quest))   // FUN_101dc700
                for (const ContractType& t : g_types)
                    for (uint32_t g : t.groups)
                        for (uint32_t off : {8u, 12u, 16u})
                            if (U32(row, off) == g)
                            {
                                AscLua::lua_pushstring(L, t.name.c_str());
                                return 1;
                            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_1033aa80: {[content type] = {type names...}}.
    int GetTemporalContractTypes(lua_State* L)
    {
        std::vector<std::pair<std::string, std::vector<std::string>>> byContent;
        for (const ContractType& t : g_types)
        {
            auto it = byContent.begin();
            while (it != byContent.end() && it->first != t.contentType)
                ++it;
            if (it == byContent.end())
            {
                byContent.emplace_back(t.contentType, std::vector<std::string>());
                it = byContent.end() - 1;
            }
            it->second.push_back(t.name);
        }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const auto& e : byContent)
        {
            AscLua::lua_pushstring(L, e.first.c_str());
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            for (size_t i = 0; i < e.second.size(); ++i)
            {
                AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
                AscLua::lua_pushstring(L, e.second[i].c_str());
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    void EnqueueCompletion(const std::string& type, void* flag)
    {
        Enqueue(type, *static_cast<bool*>(flag), 0);
    }

    // handler_ActivateAllTemporalContracts (0x10339510): (flag) over the whole Callboard view.
    int ActivateAllTemporalContracts(lua_State* L)
    {
        if (AscLua::lua_gettop(L) == 1 && IsBoolArg(L, 1) && g_queue.empty())
        {
            bool flag = AscLua::lua_toboolean(L, 1) != 0;
            PushBool(L, AscCallboard::ForEachCompletion(nullptr, EnqueueCompletion, &flag));
            return 1;
        }
        PushBool(L, false);
        return 1;
    }

    // FUN_103395a0: (content type, flag).
    int ActivateAllTemporalContractsByContentType(lua_State* L)
    {
        if (AscLua::lua_gettop(L) == 2 && AscLua::lua_isstring(L, 1) && IsBoolArg(L, 2) && g_queue.empty())
        {
            const std::string contentType = ArgString(L, 1);
            bool flag = AscLua::lua_toboolean(L, 2) != 0;
            PushBool(L, AscCallboard::ForEachCompletion(&contentType, EnqueueCompletion, &flag));
            return 1;
        }
        PushBool(L, false);
        return 1;
    }

    // FUN_10339800: (type) -> {rewards (the first cached quest's, unscaled by completions),
    // remainingCompletions, category, moneyCost, tokenCost, minimumLevel, name}, or nil.
    int GetCategoryData(lua_State* L)
    {
        const ContractType* t = nullptr;
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isstring(L, 1))
            t = FindByName(ArgString(L, 1));
        if (!t)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "rewards");
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t g : t->groups)
        {
            const uint8_t* row = g ? FirstCachedQuest(g) : nullptr;
            const uint8_t* rec = row ? QuestRecord(U32(row, 0)) : nullptr;
            if (!rec)
                continue;
            SetInt(L, "RewardMoney", static_cast<int32_t>(U32(rec, 0x34)));
            SetInt(L, "RewardArenaPoints", static_cast<int32_t>(U32(rec, 0x24A0)));
            SetInt(L, "RewardHonor", ScaledHonor(U32(rec, 0x44)));
            for (const char* key : {"RewardItems", "RewardAmount"})
            {
                const uint32_t base = key[6] == 'I' ? 0x54 : 0x64;
                AscLua::lua_pushstring(L, key);
                AscLua::lua_createtable(L, 0, 0);
                AscLua::lua_checkstack(L, 2);
                for (uint32_t i = 0; i < 4; ++i)
                {
                    AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
                    AscLua::lua_pushinteger(L, static_cast<int32_t>(U32(rec, base + i * 4)));
                    AscLua::lua_settable(L, -3);
                }
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_pushstring(L, "CallboardCachePoints");
            const AscQuestAddon::Record* a = AscQuestAddon::Get(U32(row, 0));
            AscLua::lua_pushnumber(L, a ? static_cast<double>(a->points) : 0.0);
            AscLua::lua_settable(L, -3);
            break;
        }
        AscLua::lua_settable(L, -3);
        SetInt(L, "remainingCompletions", static_cast<int32_t>(t->remaining));
        SetStr(L, "category", t->contentType);
        SetInt(L, "moneyCost", static_cast<int32_t>(t->moneyCost));
        SetInt(L, "tokenCost", static_cast<int32_t>(t->tokenCost));
        SetInt(L, "minimumLevel", static_cast<int32_t>(t->minimumLevel));
        SetStr(L, "name", t->displayName);
        return 1;
    }

    // FUN_10339cb0: (type) -> its display name, or nil.
    int GetCategoryName(lua_State* L)
    {
        const ContractType* t = nullptr;
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isstring(L, 1))
            t = FindByName(ArgString(L, 1));
        if (!t)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscLua::lua_pushstring(L, t->displayName.c_str());
        return 1;
    }

    // FUN_1033ae00: (content type) -> the rewards of every remaining completion of its priced types,
    // one cached quest per group.
    int GetTotalContentTypeRewards(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1 || !AscLua::lua_isstring(L, 1))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        Totals total;
        for (const ContractType* t : ByContentType(ArgString(L, 1)))
            if (t->remaining != 0)
                for (uint32_t g : t->groups)
                    if (g != 0)
                        if (const uint8_t* row = FirstCachedQuest(g))
                            AddQuest(total, row, t->remaining, true);
        PushTotals(L, total);
        return 1;
    }

    // FUN_1033b410: (type) -> its first cached quest's rewards times the remaining completions, or nil.
    int GetTotalContractTypeRewards(lua_State* L)
    {
        const ContractType* t = nullptr;
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isstring(L, 1))
            t = FindByName(ArgString(L, 1));
        if (!t)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        Totals total;
        const uint8_t* row = FirstCachedQuest(*t);
        if (row && t->moneyCost != 0 && t->tokenCost != 0)
            AddQuest(total, row, t->remaining, false);
        PushTotals(L, total);
        return 1;
    }

    // FUN_1033ac60: (content type) -> {moneyCost, tokenCost} over its priced types' completions.
    int GetTotalContentTypeCost(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1 || !AscLua::lua_isstring(L, 1))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        uint32_t money = 0, token = 0;
        for (const ContractType* t : ByContentType(ArgString(L, 1)))
            if (t->remaining != 0)
            {
                money += t->moneyCost * t->remaining;
                token += t->tokenCost * t->remaining;
            }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "moneyCost", static_cast<int32_t>(money));
        SetInt(L, "tokenCost", static_cast<int32_t>(token));
        return 1;
    }

    // FUN_1033b2e0: (type) -> {moneyCost, tokenCost} for its remaining completions, zeros when unknown.
    int GetTotalContractTypeCost(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1 || !AscLua::lua_isstring(L, 1))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        uint32_t money = 0, token = 0;
        const ContractType* t = FindByName(ArgString(L, 1));
        if (t && t->moneyCost != 0 && t->tokenCost != 0)
        {
            money = t->moneyCost * t->remaining;
            token = t->tokenCost * t->remaining;
        }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "moneyCost", static_cast<int32_t>(money));
        SetInt(L, "tokenCost", static_cast<int32_t>(token));
        return 1;
    }

    // FUN_1033a3b0: (quest, includeZone) -> [the cached quest's ZoneOrSort when asked and set,] then
    // the Quest.dbc row's non-zero sort ids (+0x68..+0x70). Nothing while either record is missing.
    int GetQuestSortIDs(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, AscScript::BOOLEAN}))
            return 0;
        const uint32_t quest = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const bool includeZone = AscLua::lua_toboolean(L, 2) != 0;
        const uint8_t* rec = QuestRecord(quest);
        const uint8_t* row = rec ? AscBridgeStore::QuestById(quest) : nullptr;
        if (!row)
            return 0;
        std::vector<uint32_t> ids;
        if (includeZone && U32(rec, 0x10) != 0)
            ids.push_back(U32(rec, 0x10));
        for (uint32_t off = 0x68; off != 0x74; off += 4)
            if (U32(row, off) != 0)
                ids.push_back(U32(row, off));
        AscLua::lua_createtable(L, 0, static_cast<int>(ids.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int32_t>(ids[i]));   // FUN_1009b2a0
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_1033a5f0: (sort) -> AreaTable name (+0x2C) for a positive id, QuestSort name (+4) for a
    // negative one, else "Unknown".
    int GetQuestSortText(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const int32_t sort = ToInt(CheckNumber(L, 1));
        const char* text = "Unknown";
        auto row = [](uint32_t rows, uint32_t minId, uint32_t maxId, uint32_t id) -> const uint8_t* {
            const uint8_t* base = *reinterpret_cast<const uint8_t* const*>(rows);
            if (!base || id < *reinterpret_cast<const uint32_t*>(minId) || id > *reinterpret_cast<const uint32_t*>(maxId))
                return nullptr;
            return reinterpret_cast<const uint8_t* const*>(base)[id - *reinterpret_cast<const uint32_t*>(minId)];
        };
        if (sort >= 1)
        {
            if (const uint8_t* r = row(0xAD3154, 0xAD3144, 0xAD3140, static_cast<uint32_t>(sort)))   // FUN_100b1240
                text = *reinterpret_cast<const char* const*>(r + 0x2C);
        }
        else if (sort < 0)
        {
            if (const uint8_t* r = row(0xAD4450, 0xAD4440, 0xAD443C, static_cast<uint32_t>(-sort)))   // FUN_100b1ea0
                text = *reinterpret_cast<const char* const*>(r + 4);
        }
        AscLua::lua_pushstring(L, text);
        return 1;
    }

    void Init()   // FUN_10339120
    {
        AscRuntime::OnGlueScreen(OnGlue);
        AscRuntime::OnEnterWorld(OnEnterWorld);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_TemporalContracts", "GetCompletableTemporalContracts", GetCompletableTemporalContracts},
        {"C_TemporalContracts", "ActivateTemporalContract", ActivateTemporalContract},
        {"C_TemporalContracts", "GetTemporalContractInfo", GetTemporalContractInfo},
        {"C_TemporalContracts", "GetCompletableQuestsByCategory", GetCompletableQuestsByCategory},
        {"C_TemporalContracts", "GetQuestCategory", GetQuestCategory},
        {"C_TemporalContracts", "GetTemporalContractTypes", GetTemporalContractTypes},
        {"C_TemporalContracts", "ActivateAllTemporalContracts", ActivateAllTemporalContracts},
        {"C_TemporalContracts", "ActivateAllTemporalContractsByContentType", ActivateAllTemporalContractsByContentType},
        {"C_TemporalContracts", "GetCategoryData", GetCategoryData},
        {"C_TemporalContracts", "GetCategoryName", GetCategoryName},
        {"C_TemporalContracts", "GetTotalContentTypeRewards", GetTotalContentTypeRewards},
        {"C_TemporalContracts", "GetTotalContractTypeRewards", GetTotalContractTypeRewards},
        {"C_TemporalContracts", "GetTotalContentTypeCost", GetTotalContentTypeCost},
        {"C_TemporalContracts", "GetTotalContractTypeCost", GetTotalContractTypeCost},
        {"C_TemporalContracts", "GetQuestSortIDs", GetQuestSortIDs},
        {"C_TemporalContracts", "GetQuestSortText", GetQuestSortText},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}
