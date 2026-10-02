// C_Wildcard, part 1: WildcardModeMgr (FUN_10a315d0, static 0x10D3D0D8) -- spent-roll repurchasing and
// the roll / reset / starting-choice gates. The desired/undesired filters and rapid rolling (the
// WildcardMgr, FUN_10a349d0) are still AscWildcard.cpp's older inferred code; see HANDOFF.
//
// WildcardModeMgr, per spec (index FUN_1016f580, up to 20):
//   +0x08 / +0x58   two server counters (0x6EE / 0x6EF)
//   +0xA8 / +0xF8 / +0x148   repurchasable rolls / ability rolls / talent rolls
//   +0x198 / +0x19C          learned-entry records still being revealed (0x2C each, SMSG 0x621)
// SMSG 0x6EE (FUN_10a30820): u32 n (<= 20) x {u32 x5} -> spec i. SMSG 0x6EF (handler_0x06ef): 5 u32 for
// the active spec; WILDCARD_REPURCHASABLE_ROLLS_CHANGED / _ABILITY_ / _TALENT_ "%u%u" (old, new) on change.
// SMSG 0x6F4 (FUN_10a3a0d0): str -> WILDCARD_REPURCHASE_ROLLS_RESULT.
// SMSG 0x646 / 0x64C: str -> WILDCARD_RESET_ABILITIES_RESULT / WILDCARD_REROLL_UNLOCKED_STARTING_ABILITIES_RESULT.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellRank.hpp>
#include <Ascension/AscWildcardRapid.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <windows.h>

using namespace AscScript;

namespace
{
    const char* const kRepurchase[11] = {"REPURCHASE_WILDCARD_ROLL_OK", "REPURCHASE_WILDCARD_ROLL_UNKNOWN",
        "REPURCHASE_WILDCARD_ROLL_NOT_WILDCARD", "REPURCHASE_WILDCARD_ROLL_NO_COUNT",
        "REPURCHASE_WILDCARD_ROLL_NOT_MAX_LEVEL", "REPURCHASE_WILDCARD_ROLL_NOT_ENABLED",
        "REPURCHASE_WILDCARD_ROLL_TOO_LOW_COUNT", "REPURCHASE_WILDCARD_ROLL_TOO_HIGH_COUNT",
        "REPURCHASE_WILDCARD_ROLL_NOT_ENOUGH_COUNT", "REPURCHASE_WILDCARD_ROLL_NO_TOKENS",
        "REPURCHASE_WILDCARD_ROLL_NO_MONEY"};   // 0x10B1EEE8

    struct
    {
        uint32_t a[20] = {}, b[20] = {}, rolls[20] = {}, abilityRolls[20] = {}, talentRolls[20] = {};
    } g;

    uint32_t Spec()
    {
        const int s = AscGameMode::ActiveSpecIndex();
        return (s >= 0 && s < 20) ? static_cast<uint32_t>(s) : 0;
    }

    // Kinds of repurchase: 0 rolls (+0xA8), 1 ability rolls (+0xF8), 2 talent rolls (+0x148).
    uint32_t Available(int kind)   // FUN_10a2fea0 / FUN_10a2fe80 / FUN_10a2fec0
    {
        return kind == 0 ? g.rolls[Spec()] : kind == 1 ? g.abilityRolls[Spec()] : g.talentRolls[Spec()];
    }

    // FUN_1008e380: at the server max level (or, on a development realm, level 60 / 70 / 80).
    bool AtMaxLevel()
    {
        uint8_t* p = ActivePlayer();
        if (!p)
            return false;
        const uint32_t lvl = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(p + 8) + 0xD8);
        if (lvl >= AscGameEvents::ServerMaxLevel())
            return true;
        return RealmInfoSvc::Get().Dev() && (lvl == 0x3C || lvl == 0x46 || lvl == 0x50);
    }

    // FUN_10308570: stacks of `entry` in the player's slots 0x76..0x95.
    uint32_t CountItem(uint32_t entry)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return 0;
        const uint8_t* desc = *reinterpret_cast<uint8_t* const*>(player + 8);
        uint32_t total = 0;
        for (uint32_t i = 0x76; i < 0x96; ++i)
            if (const uint8_t* item = static_cast<const uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(desc + 0x510 + i * 8), 2)))
            {
                const uint8_t* idesc = *reinterpret_cast<uint8_t* const*>(item + 8);
                if (*reinterpret_cast<const uint32_t*>(idesc + 0xC) == entry)
                    total += *reinterpret_cast<const uint32_t*>(idesc + 0x38);
            }
        return total;
    }

    // FUN_10a39e70 via FUN_10a3a050 / 39e30 / 3a090: count 1..limit; gold = price x count in copper
    // (player +0x1248), else price x count Marks of Ascension (item 0x5B9D2).
    uint32_t Validate(int kind, uint32_t count, bool gold)
    {
        const uint32_t limit = kind == 2 ? 0x37EC : 0x29F1;
        const uint32_t price = kind == 2 ? (gold ? 150000u : 0x21u) : (gold ? 200000u : 0x32u);
        if (!((AscGameMode::Mode() >> 6) & 1))
            return 2;
        const uint32_t available = Available(kind);
        if (available == 0)
            return 3;
        if (!ActivePlayer() || !AtMaxLevel())
            return 4;
        const bool* enabled = AscConfig::Bool("CONFIG_WILDCARD_ROLL_REPURCHASING_ENABLED");
        if (!(enabled && *enabled))
            return 5;
        if (count < 1)
            return 6;
        if (limit < count)
            return 7;
        if (available < count)
            return 8;
        uint8_t* player = ActivePlayer();
        if (!gold)   // the token price: Marks of Ascension
            return CountItem(0x5B9D2) < count * price ? 9 : 0;
        if (!player)
            return 10;
        const uint32_t money = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(player + 8) + 0x1248);
        return money < count * price ? 10 : 0;
    }

    void PushResult(lua_State* L, uint32_t code)   // FUN_10a29a30
    {
        if (code < 11)
            PushStr(L, kRepurchase[code]);
        else
            PushStr(L, ("UNEXPECTED_ENUM_VALUE_" + std::to_string(code)).c_str());
    }

    int CanRepurchase(lua_State* L, int kind)   // FUN_10a2b6d0 / 2b560 / 2b750
    {
        uint32_t count;
        bool gold;
        if (!ValidateInput(L, {NUMBER, AscScript::BOOLEAN}))
            return 0;
        count = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        gold = AscLua::lua_toboolean(L, 2) != 0;
        const uint32_t code = Validate(kind, count, gold);
        PushBool(L, code == 0);
        if (code)
            PushResult(L, code);
        else
            AscLua::lua_pushnil(L);
        return 2;
    }
    int CanRepurchaseRolls(lua_State* L) { return CanRepurchase(L, 0); }
    int CanRepurchaseAbilityRolls(lua_State* L) { return CanRepurchase(L, 1); }
    int CanRepurchaseTalentRolls(lua_State* L) { return CanRepurchase(L, 2); }

    // FUN_10a2d7a0 / 2d660 / 2d8e0: (count, gold) -> CMSG 0x6F1 / 0x6F2 / 0x6F3 {u32 count, u8 gold}.
    int Repurchase(lua_State* L, int kind)
    {
        if (!ValidateInput(L, {NUMBER, AscScript::BOOLEAN}))
            return 0;
        const uint32_t count = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const bool gold = AscLua::lua_toboolean(L, 2) != 0;
        if (Validate(kind, count, gold) != 0)
        {
            PushBool(L, false);
            return 1;
        }
        Packet(kind == 0 ? 0x6F1 : kind == 1 ? 0x6F2 : 0x6F3).U32(count).U8(gold ? 1 : 0).Send();
        PushBool(L, true);
        return 1;
    }
    int RepurchaseRolls(lua_State* L) { return Repurchase(L, 0); }
    int RepurchaseAbilityRolls(lua_State* L) { return Repurchase(L, 1); }
    int RepurchaseTalentRolls(lua_State* L) { return Repurchase(L, 2); }

    int GetNumRepurchasableRolls(lua_State* L) { AscLua::lua_pushinteger(L, static_cast<int>(Available(0))); return 1; }
    int GetNumRepurchasableAbilityRolls(lua_State* L) { AscLua::lua_pushinteger(L, static_cast<int>(Available(1))); return 1; }
    int GetNumRepurchasableTalentRolls(lua_State* L) { AscLua::lua_pushinteger(L, static_cast<int>(Available(2))); return 1; }

    // handler_GetMaxRepurchasable*: (gold) -> the largest count <= available that validates, else 0.
    int GetMax(lua_State* L, int kind)
    {
        bool gold;
        if (!ReadBool(L, gold))
            return 0;
        uint32_t n = Available(kind);
        while (n != 0 && Validate(kind, n, gold) != 0)
            --n;
        AscLua::lua_pushinteger(L, static_cast<int>(n));
        return 1;
    }
    int GetMaxRepurchasableRolls(lua_State* L) { return GetMax(L, 0); }
    int GetMaxRepurchasableAbilityRolls(lua_State* L) { return GetMax(L, 1); }
    int GetMaxRepurchasableTalentRolls(lua_State* L) { return GetMax(L, 2); }

    // FUN_10a2c4a0 (rolls and ability rolls) / FUN_10a2c520 (talent rolls): (count, gold) -> price x
    // count, or -1 past the count limit.
    int RollCost(lua_State* L, bool talent)
    {
        if (!ValidateInput(L, {NUMBER, AscScript::BOOLEAN}))
            return 0;
        const uint32_t count = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const bool gold = AscLua::lua_toboolean(L, 2) != 0;
        if (count > (talent ? 0x37EBu : 0x29F0u))
        {
            AscLua::lua_pushinteger(L, -1);
            return 1;
        }
        const uint32_t price = talent ? (gold ? 150000u : 0x21u) : (gold ? 200000u : 0x32u);
        AscLua::lua_pushinteger(L, static_cast<int>(price * count));
        return 1;
    }
    int GetRepurchaseRollCost(lua_State* L) { return RollCost(L, false); }
    int GetRepurchaseTalentRollCost(lua_State* L) { return RollCost(L, true); }

    // FUN_10a2b5e0: (gold) -> any kind whose single-roll check fails only for a count/money reason
    // (not NOT_WILDCARD / NO_COUNT / NOT_MAX_LEVEL / NOT_ENABLED).
    int CanRepurchaseAnyRolls(lua_State* L)
    {
        bool gold;
        if (!ReadBool(L, gold))
            return 0;
        bool any = false;
        for (int kind : {0, 1, 2})
        {
            const uint32_t code = Validate(kind, 1, gold);
            any = any || !(code >= 2 && code <= 5);
        }
        PushBool(L, any);
        return 1;
    }

    // ---- roll / reset / starting-choice gates ---------------------------------------------------
    // SMSG 0x621's reveal records (+0x198, 0x2C each): the entry, its first-rank spell, the ranks
    // gained, the rank before, the stop code and the 1.5 s reveal timer. Spellbook adds of a spell
    // being revealed wait in +0x1A4 (keyed by entry) until its timer fires.
    struct Reveal { uint32_t entry, spell, numRanks, preRoll; std::string stop; uint32_t timer; };
    std::vector<Reveal> g_reveals;
    struct DeferredAdd { uint32_t spell, a, b; };
    std::map<uint32_t, std::vector<DeferredAdd>> g_deferred;
    bool RevealsPending() { return !g_reveals.empty(); }

    uint32_t StartingCount()   // FUN_10154f50
    {
        const int32_t* v = AscConfig::Int("CONFIG_WILDCARD_STARTING_ABILITY_COUNT");
        return static_cast<uint32_t>(v ? *v : 4);
    }

    int CanResetAbilities(lua_State* L)   // handler_CanResetAbilities -> FUN_10155680
    {
        const AscCA::Build* b = AscCA::ActiveBuild();
        PushBool(L, b && b->wildcard && AscCA::AbilityCount(*b) <= StartingCount());
        return 1;
    }

    int ResetAbilities(lua_State* L)   // FUN_10a2db10: CMSG 0x645, true
    {
        Packet(0x645).Send();
        PushBool(L, true);
        return 1;
    }

    bool CanShowStartingChoice()   // FUN_10a2f920
    {
        const AscCA::Build* b = AscCA::ActiveBuild();
        return !RevealsPending() && b && b->wildcard && AscCA::AbilityCount(*b) <= StartingCount();
    }
    int CanShowStartingChoice(lua_State* L)
    {
        PushBool(L, CanShowStartingChoice());
        return 1;
    }

    int RerollUnlockedStartingAbilities(lua_State* L)   // FUN_10a2da20: CMSG 0x64B when nothing is revealing
    {
        if (RevealsPending())
        {
            PushBool(L, false);
            return 1;
        }
        Packet(0x64B).Send();
        PushBool(L, true);
        return 1;
    }

    int WillRollStartingAbilities(lua_State* L)   // FUN_10159210
    {
        const AscCA::Build* b = AscCA::ActiveBuild();
        PushBool(L, b && b->wildcard && AscCA::AbilityCount(*b) < StartingCount());
        return 1;
    }

    int WillRollFirstNonStartingAbility(lua_State* L)   // FUN_101591f0
    {
        const AscCA::Build* b = AscCA::ActiveBuild();
        PushBool(L, b && b->wildcard && AscCA::AbilityCount(*b) == StartingCount());
        return 1;
    }

    // FUN_10155120 / FUN_10155160: the unspent essence (budget above spent) covers one roll.
    bool CanAffordRoll(const AscCA::Build& b, bool talent)
    {
        const int32_t* costCfg = AscConfig::Int(talent ? "CONFIG_WILDCARD_TALENT_ROLL_COST" : "CONFIG_WILDCARD_ABILITY_ROLL_COST");
        const uint32_t cost = static_cast<uint32_t>(costCfg ? *costCfg : (talent ? 1 : 2));
        const uint32_t spent = talent ? b.GlobalTE(0) : b.GlobalAE(0);
        const uint32_t budget = talent ? b.TEBudget(b.level) : b.AEBudget(b.level);
        return (spent < budget ? budget - spent : 0) >= cost;
    }

    int CanRollAbilities(lua_State* L)   // handler_CanRollAbilities
    {
        const AscCA::Build* b = RevealsPending() ? nullptr : AscCA::ActiveBuild();
        PushBool(L, b && (CanAffordRoll(*b, false) || CanAffordRoll(*b, true)));
        return 1;
    }

    // ---- SMSG ------------------------------------------------------------------------------------
    uint32_t U32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    std::string Str(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    void __cdecl OnMiscList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6EE
    {
        uint32_t n = U32(p);
        if (n > 20)
            n = 20;
        for (uint32_t i = 0; i < n; ++i)
        {
            g.a[i] = U32(p);
            g.b[i] = U32(p);
            g.rolls[i] = U32(p);
            g.abilityRolls[i] = U32(p);
            g.talentRolls[i] = U32(p);
        }
    }

    void __cdecl OnMiscUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6EF
    {
        const uint32_t s = Spec();
        g.a[s] = U32(p);
        g.b[s] = U32(p);
        const uint32_t oldRolls = g.rolls[s], oldAbility = g.abilityRolls[s], oldTalent = g.talentRolls[s];
        g.rolls[s] = U32(p);
        g.abilityRolls[s] = U32(p);
        g.talentRolls[s] = U32(p);
        if (oldRolls != g.rolls[s])
            AscRuntime::Signal("WILDCARD_REPURCHASABLE_ROLLS_CHANGED", "%u%u", oldRolls, g.rolls[s]);
        if (oldAbility != g.abilityRolls[s])
            AscRuntime::Signal("WILDCARD_REPURCHASABLE_ABILITY_ROLLS_CHANGED", "%u%u", oldAbility, g.abilityRolls[s]);
        if (oldTalent != g.talentRolls[s])
            AscRuntime::Signal("WILDCARD_REPURCHASABLE_TALENT_ROLLS_CHANGED", "%u%u", oldTalent, g.talentRolls[s]);
    }

    void __cdecl OnRepurchaseResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6F4
    {
        const std::string r = Str(p);
        AscRuntime::Signal("WILDCARD_REPURCHASE_ROLLS_RESULT", "%s", r.c_str());
    }
    void __cdecl OnResetResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x646
    {
        const std::string r = Str(p);
        AscRuntime::Signal("WILDCARD_RESET_ABILITIES_RESULT", "%s", r.c_str());
    }
    void __cdecl OnRerollResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x64C
    {
        const std::string r = Str(p);
        AscRuntime::Signal("WILDCARD_REROLL_UNLOCKED_STARTING_ABILITIES_RESULT", "%s", r.c_str());
    }

    // ---- the roll-ready poll (FUN_10a31760 / LAB_10a31a00) ---------------------------------------
    uint32_t g_pollTimer = 0;      // DAT_10d3d0a4
    bool g_rollReady = false;      // DAT_10d3d0a8
    uint32_t g_pollUntil = 0;      // DAT_10d3d0ac: 50 ms polls until this tick, then 1 s

    int __cdecl Poll(void*)
    {
        const AscCA::Build* b = AscCA::ActiveBuild();
        const bool ready = b && (CanAffordRoll(*b, false) || CanAffordRoll(*b, true));
        if (ready && !g_rollReady)
            AscRuntime::Signal("WILDCARD_ROLL_READY");
        g_rollReady = ready;
        const uint32_t delay = static_cast<int32_t>(GetTickCount() - g_pollUntil) < 0 ? 50 : 1000;
        g_pollTimer = AscRuntime::Schedule(delay, Poll, nullptr);
        return 0;
    }

    void ArmPoll()
    {
        g_pollUntil = GetTickCount() + 1000;
        if (g_pollTimer)
        {
            AscRuntime::Cancel(g_pollTimer, Poll, nullptr);
            g_pollTimer = 0;
        }
        g_pollTimer = AscRuntime::Schedule(0x32, Poll, nullptr);
    }

    // ---- the spellbook add (0x542030) and its deferral while a reveal runs -------------------------
    typedef void(__cdecl* AddSpell_t)(uint32_t, uint32_t, uint32_t);
    AddSpell_t g_addSpell = nullptr;

    // FUN_102781e0: the add, with the learn sounds muted on a wildcard realm, then list 0x10be2aec
    // (AscRuntime::RunAfterAddSpell; the alternate-power spell watch FUN_10322270).
    void AddSpell(uint32_t spell, uint32_t a, uint32_t b)
    {
        const bool wildcard = (AscGameMode::Mode() >> 6) & 1;
        if (wildcard)
            AscCA::SuppressLearnSounds(true);
        g_addSpell(spell, a, b);
        if (wildcard)
            AscCA::SuppressLearnSounds(false);
        AscRuntime::RunAfterAddSpell(spell);
    }

    // FUN_10a31b00: an add of a spell whose first rank is being revealed waits for the reveal.
    bool Defer(uint32_t spell, uint32_t a, uint32_t b)
    {
        if (g_reveals.empty())
            return false;
        const uint32_t root = AscSpellRank::FirstRank(spell);
        for (const Reveal& r : g_reveals)
            if (r.spell && r.spell == root)
            {
                g_deferred[r.entry].push_back({spell, a, b});
                return true;
            }
        return false;
    }

    void __cdecl AddSpellDetour(uint32_t spell, uint32_t a, uint32_t b)   // FUN_10275ed0
    {
        if (!Defer(spell, a, b))
            AddSpell(spell, a, b);
    }

    // FUN_10a2fa70 (the 1.5 s timer LAB_10a31ae0): the reveal ends, its deferred adds run, and the
    // rapid-rolling session moves on. 0x403370 calls its callback with (x, param); the thunk
    // LAB_10a31ae0 hands FUN_10a2fa70 the second argument ([esp+8]), the entry id.
    int __cdecl RevealDoneThunk(void*, void* param);
    const AscRuntime::TimerFn RevealDone = reinterpret_cast<AscRuntime::TimerFn>(&RevealDoneThunk);

    int __cdecl RevealDoneThunk(void*, void* param)
    {
        const uint32_t entry = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(param));
        auto it = std::find_if(g_reveals.begin(), g_reveals.end(), [&](const Reveal& r) { return r.entry == entry; });
        if (it == g_reveals.end())
            return 0;
        g_reveals.erase(it);
        auto d = g_deferred.find(entry);
        if (d != g_deferred.end())
        {
            const std::vector<DeferredAdd> adds = d->second;
            g_deferred.erase(d);
            for (const DeferredAdd& x : adds)
                AddSpell(x.spell, x.a, x.b);
        }
        AscWildcardRapid::OnRevealDone(entry);
        return 0;
    }

    // SMSG 0x621 (FUN_10a30260): u32 entry, u32 ranks gained, str stop code.
    void __cdecl OnEntryLearned(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t entry = U32(p);
        const uint32_t numRanks = U32(p);
        const std::string stop = Str(p);
        uint32_t spell = 0;
        if (AscCA::Row r = AscCA::FindRow(entry))
            spell = AscSpellRank::FirstRank(AscCA::RowU32(r, 0x14));   // FUN_101c62e0(1) -> FUN_102163f0
        const AscCA::Build* b = AscCA::ActiveBuild();
        const uint32_t preRoll = b ? b->RankOf(entry) : 0;
        AscLog::Printf("WildcardModeMgr::HandleWildcardEntryLearnedOpcode: entry: %u, numRanks: %u, preRollRank: %u, newRank: %u, stopRapidRollingCode: %s",
            entry, numRanks, preRoll, preRoll + numRanks, stop.c_str());
        Reveal rec{entry, spell, numRanks, preRoll, stop, 0};
        rec.timer = AscRuntime::Schedule(0x5DC, RevealDone, reinterpret_cast<void*>(static_cast<uintptr_t>(entry)));
        g_reveals.push_back(rec);
        AscWildcardRapid::OnEntryLearned(entry, preRoll + numRanks, preRoll, stop);
        // FUN_10a2f5e0: STOP_RAPID_ROLLING_NONE is index 0 -- a plain learn.
        if (stop == "STOP_RAPID_ROLLING_NONE")
            AscRuntime::Signal("WILDCARD_ENTRY_LEARNED", "%u%u%u", entry, preRoll + numRanks, preRoll);
        else
            AscRuntime::Signal("WILDCARD_RAPID_ROLL_LEARNED", "%u%u%u%s", entry, preRoll + numRanks, preRoll, stop.c_str());
    }

    // SMSG 0x61E (FUN_10a30dd0): str result. Outside a rapid-rolling session a successful unlearn
    // this client asked for is followed by the roll itself (CMSG 0x643 with nothing desired).
    void __cdecl OnUnlearnResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = Str(p);
        AscRuntime::Signal("WILDCARD_UNLEARN_ABILITY_RESULT", "%s", result.c_str());
        const bool consumed = AscWildcardRapid::OnUnlearnResult(result);
        const bool pending = AscWildcardRapid::TakeRerollPending();
        if (!consumed && pending && result == "CA_UNLEARN_OK")
            Packet(0x643).U32(0).U32(0).U32(0).U8(0).Send();
    }

    // SMSG 0x644 (FUN_10a30bb0): str result; a CAN_START_RAPID_ROLLING_* answer is a rapid-roll result.
    void __cdecl OnRollResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = Str(p);
        AscWildcardRapid::OnRollResult(result);
        const bool rapid = result.find("CAN_START_RAPID_ROLLING_") != std::string::npos;
        AscRuntime::Signal(rapid ? "WILDCARD_RAPID_ROLL_RESULT" : "WILDCARD_ROLL_ABILITIES_RESULT", "%s", result.c_str());
    }

    void OnGlueScreen()   // FUN_10a2f950
    {
        g = {};
        if (g_pollTimer)
        {
            AscRuntime::Cancel(g_pollTimer, Poll, nullptr);
            g_pollTimer = 0;
        }
        g_rollReady = false;
        g_pollUntil = 0;
        AscWildcardRapid::TakeRerollPending();
        for (const Reveal& r : g_reveals)
            AscRuntime::Cancel(r.timer, RevealDone, reinterpret_cast<void*>(static_cast<uintptr_t>(r.entry)));
        g_reveals.clear();
        g_deferred.clear();   // FUN_10a31e10
    }

    // FUN_10a318b0 (every world entry): the reveal timers cancelled ("WildcardReveal") and the reveal list
    // (+0x198) and deferred adds (FUN_10a31e10) dropped, the rapid-rolling session reset (FUN_10a33b10),
    // the roll-ready flag cleared, and the roll poll restarted at 1000 ms.
    void OnEnterWorld()
    {
        for (const Reveal& r : g_reveals)
            AscRuntime::Cancel(r.timer, RevealDone, reinterpret_cast<void*>(static_cast<uintptr_t>(r.entry)));
        g_reveals.clear();
        g_deferred.clear();
        AscWildcardRapid::ResetSession();
        g_rollReady = false;
        if (g_pollTimer)
        {
            AscRuntime::Cancel(g_pollTimer, Poll, nullptr);
            g_pollTimer = 0;
        }
        g_pollTimer = AscRuntime::Schedule(1000, Poll, nullptr);
    }

    void Init()
    {
        AscRuntime::OnEnterWorld(OnEnterWorld);
        sDC.AddPacketHandler(0x61E, CNetClientCustomPacket((void*)&OnUnlearnResult, nullptr));
        sDC.AddPacketHandler(0x644, CNetClientCustomPacket((void*)&OnRollResult, nullptr));
        sDC.AddPacketHandler(0x621, CNetClientCustomPacket((void*)&OnEntryLearned, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
        // 0x542030 begins push ebp / mov ebp,esp / sub esp,0x580: nine bytes.
        g_addSpell = reinterpret_cast<AddSpell_t>(AscRuntime::Detour(0x542030, 9, reinterpret_cast<void*>(&AddSpellDetour)));
        sDC.AddPacketHandler(0x6EE, CNetClientCustomPacket((void*)&OnMiscList, nullptr));
        sDC.AddPacketHandler(0x6EF, CNetClientCustomPacket((void*)&OnMiscUpdate, nullptr));
        sDC.AddPacketHandler(0x6F4, CNetClientCustomPacket((void*)&OnRepurchaseResult, nullptr));
        sDC.AddPacketHandler(0x646, CNetClientCustomPacket((void*)&OnResetResult, nullptr));
        sDC.AddPacketHandler(0x64C, CNetClientCustomPacket((void*)&OnRerollResult, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Wildcard", "CanRepurchaseRolls", CanRepurchaseRolls},
        {"C_Wildcard", "CanRepurchaseAbilityRolls", CanRepurchaseAbilityRolls},
        {"C_Wildcard", "CanRepurchaseTalentRolls", CanRepurchaseTalentRolls},
        {"C_Wildcard", "RepurchaseRolls", RepurchaseRolls},
        {"C_Wildcard", "RepurchaseAbilityRolls", RepurchaseAbilityRolls},
        {"C_Wildcard", "RepurchaseTalentRolls", RepurchaseTalentRolls},
        {"C_Wildcard", "GetNumRepurchasableRolls", GetNumRepurchasableRolls},
        {"C_Wildcard", "GetNumRepurchasableAbilityRolls", GetNumRepurchasableAbilityRolls},
        {"C_Wildcard", "GetNumRepurchasableTalentRolls", GetNumRepurchasableTalentRolls},
        {"C_Wildcard", "GetMaxRepurchasableRolls", GetMaxRepurchasableRolls},
        {"C_Wildcard", "GetMaxRepurchasableAbilityRolls", GetMaxRepurchasableAbilityRolls},
        {"C_Wildcard", "GetMaxRepurchasableTalentRolls", GetMaxRepurchasableTalentRolls},
        {"C_Wildcard", "GetRepurchaseRollCost", GetRepurchaseRollCost},
        {"C_Wildcard", "GetRepurchaseAbilityRollCost", GetRepurchaseRollCost},
        {"C_Wildcard", "GetRepurchaseTalentRollCost", GetRepurchaseTalentRollCost},
        {"C_Wildcard", "CanRepurchaseAnyRolls", CanRepurchaseAnyRolls},
        {"C_Wildcard", "CanResetAbilities", CanResetAbilities},
        {"C_Wildcard", "ResetAbilities", ResetAbilities},
        {"C_Wildcard", "CanShowStartingChoice", CanShowStartingChoice},
        {"C_Wildcard", "RerollUnlockedStartingAbilities", RerollUnlockedStartingAbilities},
        {"C_Wildcard", "WillRollStartingAbilities", WillRollStartingAbilities},
        {"C_Wildcard", "WillRollFirstNonStartingAbility", WillRollFirstNonStartingAbility},
        {"C_Wildcard", "CanRollAbilities", CanRollAbilities},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscWildcardRolls
{
    uint32_t SpecCounter(int which) { return which == 0 ? g.a[Spec()] : g.b[Spec()]; }   // FUN_10a2fe60 / FUN_10a2fe70
    bool RevealsPending() { return ::RevealsPending(); }
    bool AtMaxLevel() { return ::AtMaxLevel(); }
    void ArmPoll() { ::ArmPoll(); }
    uint32_t StartingCount() { return ::StartingCount(); }
    bool CanShowStartingChoice() { return ::CanShowStartingChoice(); }
    bool CanAffordRoll(bool talent)   // FUN_1016f390 then FUN_10155120 / FUN_10155160
    {
        const AscCA::Build* b = AscCA::ActiveBuild();
        return b && ::CanAffordRoll(*b, talent);
    }
}
