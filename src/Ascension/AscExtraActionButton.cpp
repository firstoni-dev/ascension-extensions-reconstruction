// C_ExtraActionButton -- transcribed from the original's subsystem at 0x10236840..0x10236f00.
//
// Not packet-driven. On world entry (FUN_10278510 -> 0x10236b00) a 100 ms client timer starts
// (0x10236ca0, which re-arms itself); each tick (FUN_10236cd0) walks every ExtraActionButtons.dbc row,
// evaluates its trigger (FUN_10236840) and keeps an ordered list of active button ids (DAT_10be24f8),
// firing EXTRA_ACTION_BUTTON_ADDED / _REMOVED ("%u", id) on each change. Back at the glue screen
// (FUN_10278760 -> 0x10236a20) the list is cleared and the timer cancelled. HIDE_ALL_EXTRA_ACTION_BUTTONS
// is registered but never fired by the original.
//
// ExtraActionButtons.dbc row: +0x04 int, +0x08 str, +0x0C str, +0x10 str, +0x14 trigger type NAME,
// +0x18 trigger argument (spell / item id), +0x24 sort order.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace AscScript;

namespace
{
    AscDbc::Table& Buttons() { return AscDbc::Get("DBFilesClient/ExtraActionButtons.dbc"); }

    std::vector<uint32_t> g_active;   // DAT_10be24f8
    uint32_t g_timer = 0;             // DAT_10be24e8

    // PTR_s_TRIGGER_TYPE_NONE_10b3c1a4
    const char* const kTriggerTypes[] = {
        "TRIGGER_TYPE_NONE", "TRIGGER_TYPE_AURA_EXISTS", "TRIGGER_TYPE_AURA_DOES_NOT_EXIST",
        "TRIGGER_TYPE_COOLDOWN_READY", "TRIGGER_TYPE_ITEM_COOLDOWN_READY", "TRIGGER_TYPE_ITEM_IN_BAGS",
        "TRIGGER_TYPE_MAX",
    };

    // FUN_1020be20: the row's trigger name mapped to its index; an unknown name logs and reads as NONE.
    uint32_t TriggerType(const char* name)
    {
        for (uint32_t i = 0; i < 7; ++i)
            if (strcmp(name, kTriggerTypes[i]) == 0)
                return i;
        AscLog::Printf("Unexpected Value: %s", name);
        return 0;
    }

    // FUN_10308270: every item object the player holds -- equipment, bag slots and backpack
    // (PLAYER_FIELD_INV_SLOT_HEAD at descriptor +0x510, slots 0..0x26), then the contents of each of
    // the four equipped bags (guids at 0xC23540).
    std::vector<uint8_t*> PlayerItems(uint8_t* player)
    {
        std::vector<uint8_t*> items;
        const uint8_t* desc = *reinterpret_cast<uint8_t**>(player + 8);
        for (int slot = 0; slot < 0x27; ++slot)
            if (uint8_t* item = static_cast<uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(desc + 0x510 + slot * 8), 2)))
                items.push_back(item);
        const uint64_t* bags = reinterpret_cast<const uint64_t*>(0xC23540);
        for (int bag = 0; bag < 4; ++bag)
        {
            uint8_t* container = static_cast<uint8_t*>(ObjectPtr(bags[bag], 4));
            if (!container)
                continue;
            typedef void*(__thiscall* Inventory_t)(void*);
            void* inv = (*reinterpret_cast<Inventory_t**>(container))[0x24 / 4](container);
            if (!inv)
                continue;
            const uint32_t slots = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(container + 8) + 0x100);
            for (uint32_t i = 0; i < slots; ++i)
                if (uint8_t* item = reinterpret_cast<uint8_t*(__thiscall*)(void*, uint32_t)>(0x754390)(inv, i))
                    items.push_back(item);
        }
        return items;
    }

    // FUN_10236840
    bool Triggered(uint32_t id)
    {
        AscDbc::Table& t = Buttons();
        const uint8_t* row = t.Row(id);
        uint8_t* player = row ? ActivePlayer() : nullptr;
        if (!player)
            return false;
        const uint32_t arg = t.U32(row, 0x18);
        switch (TriggerType(t.Str(row, 0x14)))
        {
        case 1: return UnitHasAuraSpell(player, arg);
        case 2: return !UnitHasAuraSpell(player, arg);
        case 3:
        {
            int32_t start = 0, duration = 0;
            uint32_t enabled = 0;
            reinterpret_cast<void(__cdecl*)(uint32_t, int, int32_t*, int32_t*, uint32_t*)>(0x809000)(arg, 0, &start, &duration, &enabled);
            return static_cast<uint32_t>(duration + start) <= reinterpret_cast<uint32_t(__cdecl*)()>(0x86AE20)();
        }
        case 4:
        {
            int32_t start = 0, duration = 0;
            uint32_t enabled = 0;
            reinterpret_cast<void(__cdecl*)(uint32_t, int32_t*, int32_t*, uint32_t*)>(0x809030)(arg, &start, &duration, &enabled);
            return static_cast<uint32_t>(duration + start) <= reinterpret_cast<uint32_t(__cdecl*)()>(0x86AE20)();
        }
        case 5:
            for (uint8_t* item : PlayerItems(player))
                if (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(item + 8) + 0xC) == arg)
                    return true;
            return false;
        default:
            return false;
        }
    }

    bool Contains(uint32_t id) { return std::find(g_active.begin(), g_active.end(), id) != g_active.end(); }

    // FUN_10236cd0
    void Tick()
    {
        AscDbc::Table& t = Buttons();
        std::vector<uint32_t> added;
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const bool active = Contains(id);
            const bool on = Triggered(id);
            if (!active)
            {
                if (on)
                    added.push_back(id);
            }
            else if (!on)
            {
                g_active.erase(std::remove(g_active.begin(), g_active.end(), id), g_active.end());
                AscRuntime::Signal("EXTRA_ACTION_BUTTON_REMOVED", "%u", id);
            }
        }
        // FUN_10236580: introsort by the row's +0x24 order.
        std::sort(added.begin(), added.end(), [&](uint32_t a, uint32_t b) {
            const uint8_t* ra = t.Row(a);
            const uint8_t* rb = t.Row(b);
            return (ra ? t.U32(ra, 0x24) : 0) < (rb ? t.U32(rb, 0x24) : 0);
        });
        for (uint32_t id : added)
        {
            if (Contains(id))
                continue;
            g_active.push_back(id);
            AscRuntime::Signal("EXTRA_ACTION_BUTTON_ADDED", "%u", id);
        }
    }

    // 0x10236ca0
    int __cdecl OnTimer(void*)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            AscLog::Printf("ExtraActionButton: first tick; event ids ADDED=%d REMOVED=%d PLAYER_LOGIN=%d count=%u; %u rows",
                           AscRuntime::EventId("EXTRA_ACTION_BUTTON_ADDED"),
                           AscRuntime::EventId("EXTRA_ACTION_BUTTON_REMOVED"), AscRuntime::EventId("PLAYER_LOGIN"),
                           *reinterpret_cast<const uint32_t*>(0xD3F7D4), Buttons().Count());
        }
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

    // 0x10236b00. EnterWorld repeats on /reload and far teleports: cancel the running chain
    // first so world entries do not stack 100 ms ticks.
    void Start()
    {
        CancelTimer();
        g_timer = AscRuntime::Schedule(100, OnTimer, nullptr);
    }

    // 0x10236a20
    void Stop()
    {
        g_active.clear();
        CancelTimer();
    }

    // handler_GetExtraActionButtonInfo: row -> (str +8, str +0xC, int +4, str +0x10), else four nils.
    int GetExtraActionButtonInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscDbc::Table& t = Buttons();
        const uint8_t* row = t.Row(id);
        if (!row)
            return PushNils(L, 4);
        PushStr(L, t.Str(row, 8));
        PushStr(L, t.Str(row, 0xC));
        PushInt(L, t.I32(row, 4));
        PushStr(L, t.Str(row, 0x10));
        return 4;
    }

    // handler_GetExtraActionButtonAtIndex: 1-based; nil for an out-of-range index or a zero id, and
    // nothing at all for index 0.
    int GetExtraActionButtonAtIndex(lua_State* L)
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        if (index - 1 < g_active.size() && g_active[index - 1])
            PushInt(L, static_cast<int32_t>(g_active[index - 1]));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // handler_GetNumExtraActionButtons
    int GetNumExtraActionButtons(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(g_active.size()));
        return 1;
    }

    // FUN_10236a60: the module's init.
    void Init()
    {
        AscRuntime::OnEnterWorld(Start);
        AscRuntime::OnGlueScreen(Stop);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_ExtraActionButton", "GetExtraActionButtonInfo", GetExtraActionButtonInfo},
        {"C_ExtraActionButton", "GetNumExtraActionButtons", GetNumExtraActionButtons},
        {"C_ExtraActionButton", "GetExtraActionButtonAtIndex", GetExtraActionButtonAtIndex},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
