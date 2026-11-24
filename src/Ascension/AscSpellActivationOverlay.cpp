// C_SpellActivationOverlay -- transcribed from the original's subsystem at 0x1031f580..0x1031fcc0.
//
// Same shape as ExtraActionButton: world entry (0x1031f940) arms a 100 ms timer (0x1031fb60) whose
// tick (0x1031fb90) walks SpellActivationOverlays.dbc, evaluates each row (0x1031f640) and keeps the
// list of shown overlays (0x10be43d4), firing SHOW_ / HIDE_SPELL_ACTIVATION_OVERLAY ("%u", id).
// The glue screen (0x1031f860) clears it and cancels the timer. HIDE_ALL_SPELL_ACTIVATION_OVERLAYS
// is registered, never fired. Note that, unlike ExtraActionButton, additions are NOT sorted: each id
// is appended as the row walk reaches it.
//
// SpellActivationOverlays.dbc row: +0x04 spell, +0x08 texture str, +0x0C..+0x18 floats, +0x1C spell
// whose cooldown gates the overlay (0 = +0x04), +0x20 int, +0x24 trigger type NAME, +0x28 int,
// +0x2C stack/charge value, +0x30 comparison.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscSpellRank.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace AscScript;

namespace
{
    AscDbc::Table& Overlays() { return AscDbc::Get("DBFilesClient/SpellActivationOverlays.dbc"); }

    std::vector<uint32_t> g_shown;   // 0x10be43d4
    uint32_t g_timer = 0;            // 0x10be43c4

    // PTR_s_TRIGGER_TYPE_NONE_10b3cb94, parsed by FUN_10215070
    const char* const kTriggerTypes[] = {
        "TRIGGER_TYPE_NONE", "TRIGGER_TYPE_STACKS", "TRIGGER_TYPE_AURA_EXISTS",
        "TRIGGER_TYPE_AURA_DOES_NOT_EXIST", "TRIGGER_TYPE_COOLDOWN_READY", "TRIGGER_TYPE_CHARGES",
        "TRIGGER_TYPE_MAX",
    };

    uint32_t TriggerType(const char* name)
    {
        for (uint32_t i = 0; i < 7; ++i)
            if (strcmp(name, kTriggerTypes[i]) == 0)
                return i;
        AscLog::Printf("Unexpected Value: %s", name);
        return 0;
    }

    // FUN_10215370
    bool Compare(uint32_t op, int32_t have, int32_t want)
    {
        switch (op)
        {
        case 0: return have == want;
        case 1: return want < have;
        case 2: return have < want;
        case 3: return want <= have;
        case 4: return have <= want;
        default: return false;
        }
    }

    struct Cooldown { uint32_t start = 0, duration = 0; uint8_t enabled = 0; };
    Cooldown SpellCooldown(uint32_t spell)
    {
        Cooldown c;
        reinterpret_cast<void(__cdecl*)(uint32_t, int, uint32_t*, uint32_t*, uint8_t*)>(0x809000)(
            spell, 0, &c.start, &c.duration, &c.enabled);
        return c;
    }
    uint32_t Now() { return reinterpret_cast<uint32_t(__cdecl*)()>(0x86AE20)(); }

    // 0x1031f640
    bool Shown(uint32_t id)
    {
        AscDbc::Table& t = Overlays();
        const uint8_t* row = t.Row(id);
        uint8_t* player = row ? ActivePlayer() : nullptr;
        if (!player)
            return false;

        // A row whose gating spell is on a real cooldown (longer than its GCD + 50 ms) is hidden.
        uint32_t gate = t.U32(row, 0x1C);
        if (!gate)
            gate = t.U32(row, 4);
        if (gate)
        {
            const Cooldown c = SpellCooldown(gate);
            if (c.enabled && static_cast<int32_t>(c.start) >= 1 && static_cast<int32_t>(c.duration) >= 1
                && c.start + c.duration > Now())
            {
                uint8_t rec[0x2A8] = {};
                if (!FetchSpell(gate, rec))
                    return false;
                const uint32_t shortest = c.start < c.duration ? c.start : c.duration;
                if (shortest > *reinterpret_cast<const uint32_t*>(rec + 0x238) + 0x32)   // StartRecoveryTime
                    return false;
            }
        }

        const uint32_t spell = t.U32(row, 4);
        switch (TriggerType(t.Str(row, 0x24)))
        {
        case 1:   // STACKS
        case 5:   // CHARGES
        {
            typedef uint32_t(__thiscall* AuraIndex_t)(void*, uint32_t);
            typedef const uint8_t*(__thiscall* AuraAt_t)(void*, uint32_t);
            const uint8_t* aura = reinterpret_cast<AuraAt_t>(0x556E10)(
                player, reinterpret_cast<AuraIndex_t>(0x7289C0)(player, spell));
            if (!aura)
                return false;
            return Compare(t.U32(row, 0x30), *reinterpret_cast<const uint16_t*>(aura + 0xE), t.I32(row, 0x2C));
        }
        case 2: return UnitHasAuraSpell(player, spell);
        case 3: return !UnitHasAuraSpell(player, spell);
        case 4:
        {
            const Cooldown c = SpellCooldown(spell);
            return c.duration + c.start <= Now();
        }
        default:
            return false;
        }
    }

    bool Contains(uint32_t id) { return std::find(g_shown.begin(), g_shown.end(), id) != g_shown.end(); }

    // 0x1031fb90
    void Tick()
    {
        AscDbc::Table& t = Overlays();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const bool on = Shown(id);
            if (!Contains(id))
            {
                if (on)
                {
                    g_shown.push_back(id);
                    AscRuntime::Signal("SHOW_SPELL_ACTIVATION_OVERLAY", "%u", id);
                }
            }
            else if (!on)
            {
                g_shown.erase(std::remove(g_shown.begin(), g_shown.end(), id), g_shown.end());
                AscRuntime::Signal("HIDE_SPELL_ACTIVATION_OVERLAY", "%u", id);
            }
        }
    }

    // 0x1031fb60
    int __cdecl OnTimer(void*)
    {
        Tick();
        // Keep the re-armed handle so Stop/Start can cancel the live timer, not only the first one.
        g_timer = AscRuntime::Schedule(100, OnTimer, nullptr);
        return 0;
    }

    void CancelTimer()
    {
        if (g_timer)
        {
            AscRuntime::Cancel(g_timer, OnTimer, nullptr);
            g_timer = 0;
        }
    }

    // 0x1031f940. EnterWorld fires again on every /reload and far teleport without a glue screen
    // in between: cancel the running chain first or each world entry stacks another 100 ms tick.
    void Start()
    {
        CancelTimer();
        g_timer = AscRuntime::Schedule(100, OnTimer, nullptr);
    }

    void Stop()                                                                // 0x1031f860
    {
        g_shown.clear();
        CancelTimer();
    }

    // handler_GetSpellActivationOverlayInfo: 8 values, the last nil when the dword at +0x28 is 0.
    int GetSpellActivationOverlayInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscDbc::Table& t = Overlays();
        const uint8_t* row = t.Row(id);
        if (!row)
            return PushNils(L, 8);
        PushInt(L, t.I32(row, 4));
        PushStr(L, t.Str(row, 8));
        PushInt(L, t.I32(row, 0x20));
        PushNum(L, t.F32(row, 0x18));
        PushNum(L, t.F32(row, 0xC));
        PushNum(L, t.F32(row, 0x10));
        PushNum(L, t.F32(row, 0x14));
        if (t.I32(row, 0x28))
            PushInt(L, t.I32(row, 0x28));
        else
            AscLua::lua_pushnil(L);
        return 8;
    }

    // handler_IsSpellOverlayed: is any shown overlay's spell (+0x1C) the same first rank as arg 1.
    int IsSpellOverlayed(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return 0;
        const uint32_t base = AscSpellRank::FirstRank(spell);
        AscDbc::Table& t = Overlays();
        for (uint32_t id : g_shown)
        {
            const uint8_t* row = t.Row(id);
            if (row && AscSpellRank::FirstRank(t.U32(row, 0x1C)) == base)
            {
                PushBool(L, true);
                return 1;
            }
        }
        PushBool(L, false);
        return 1;
    }

    // 0x1031f8a0
    void Init()
    {
        AscRuntime::OnEnterWorld(Start);
        AscRuntime::OnGlueScreen(Stop);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_SpellActivationOverlay", "GetSpellActivationOverlayInfo", GetSpellActivationOverlayInfo},
        {"C_SpellActivationOverlay", "IsSpellOverlayed", IsSpellOverlayed},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
