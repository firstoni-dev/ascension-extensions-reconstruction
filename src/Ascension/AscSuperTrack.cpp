// C_SuperTrack. The tracker is the static at 0x10BCC640: +0 quest id, then the target at 0x10BCC644
// (FUN_10217xxx methods run on it; a SuperTrack.dbc row when it came from the DBC): +0 SuperTrack id,
// +4 map id (-1 = none), +8/+0xC/+0x10 x/y/z (z 0 = "the player's z + 2"), +0x14 arrival radius,
// +0x18 next SuperTrack id in the chain, +0x1C ?. Cleared value: map -1, position 0, radius 5.5.
// Setters fire SUPER_TRACKING_CHANGED.
//
// Module init (0x10334FB0): the C_SuperTrack library, the event, the "VisitedSuperTracks" CVar
// (flags 0x21, default ""), SMSG 0x740, and three lifecycle callbacks --
//   world Lua init (FUN_102787c0 list, 0x103345A0): tracker cleared, +0x24 "fresh" flag set, last quest 0;
//   world entry (FUN_10278510, 0x103362A0): three client timers, each re-arming itself --
//     1000 ms 0x10336410: re-resolve the tracked quest's target (FUN_10336440, no event);
//      100 ms 0x103362F0: target state 4 (inside its radius) -> add its SuperTrack id to the visited list;
//     1000 ms 0x10336340: when the tracked quest changed since the last tick, clear the visited list;
//   glue Lua init (FUN_102787e0 list, 0x103350C0): cancel the three timers.
// The two Lua-init lists run from the original's detour on 0x855060 (the script state's library
// setup); here they are AscBindings' world / glue "registered" lists, which run from the same detour.
//
// The visited list is the client's CVar int list (the wrapper at 0x766F70 next / 0x766720 append):
// { CVar*, index = 2, char[256], status }, values < 0xC4604, header 0x0176, at most the last 20 ids.
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Client/CVar.hpp>
#include <Misc/DataContainer.hpp>
#include <Misc/Util.hpp>
#include <cstring>
#include <vector>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <cmath>

using namespace AscScript;

namespace
{
    struct Target
    {
        uint32_t id = 0;
        int32_t map = -1;
        float x = 0, y = 0, z = 0;
        float radius = 5.5f;
        uint32_t next = 0;
        uint32_t unk1c = 0;
    };
    struct
    {
        uint32_t quest = 0;
        Target target;
        bool fresh = false;   // +0x24 (0x10BCC664)
    } g_track;
    uint32_t g_lastQuest = 0;   // 0x10D3C1A8
    CVar* g_visited = nullptr;   // 0x10D3C1A4 "VisitedSuperTracks"
    uint32_t g_timers[3] = {};  // 0x10D3C18C / 190 / 194

    int32_t CurrentMap() { return *reinterpret_cast<const int32_t*>(0xBD088C); }

    bool PlayerPosition(float out[3])
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return false;
        void** vtable = *reinterpret_cast<void***>(player);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(vtable[0x2C / 4])(player, out);
        return true;
    }

    bool Tracking() { return g_track.target.x != 0.0f || g_track.target.y != 0.0f || g_track.target.z != 0.0f; }   // FUN_10217c80

    float TargetZ()   // z, or the player's z + 2 when unset
    {
        float z = g_track.target.z;
        float p[3];
        if (z == 0.0f && PlayerPosition(p))
            z = p[2] + 2.0f;
        return z;
    }

    // FUN_10217850: 3-D distance from the player, 0 on another map.
    float Distance()
    {
        if (g_track.target.map != CurrentMap())
            return 0.0f;
        float p[3] = {0, 0, 0};
        PlayerPosition(p);
        const float z = TargetZ();
        const float dz = p[2] - z, dx = p[0] - g_track.target.x, dy = p[1] - g_track.target.y;
        return std::sqrt(dz * dz + dx * dx + dy * dy);
    }

    void Changed() { AscRuntime::Signal("SUPER_TRACKING_CHANGED"); }

    int32_t TargetState();

    Target FromRow(const uint8_t* row)   // a SuperTrack.dbc row IS a target (0x20 bytes)
    {
        Target t;
        memcpy(&t, row, sizeof(Target));
        return t;
    }

    AscDbc::Table& SuperTrackDbc() { return AscDbc::Get("DBFilesClient/SuperTrack.dbc"); }            // 0x10BDF618
    AscDbc::Table& QuestSuperTrackDbc() { return AscDbc::Get("DBFilesClient/QuestSuperTrack.dbc"); }  // 0x10BE01A0

    // ---- the visited list (client CVar int list) -------------------------------------------------
    struct IntList   // the client's wrapper; 0x766F70 / 0x766720 are __thiscall on it
    {
        CVar* cvar;
        uint32_t index;
        char buffer[0x100];
        int32_t status;

        explicit IntList(CVar* c) : cvar(c), index(2), status(0) { memset(buffer, 0, sizeof(buffer)); }
        uint32_t Next() { return reinterpret_cast<uint32_t(__thiscall*)(IntList*)>(0x766F70)(this); }
        void Add(uint32_t v) { reinterpret_cast<void(__thiscall*)(IntList*, uint32_t)>(0x766720)(this, v); }
        void Store()   // header + terminator, then CVar::Set (FUN_10114c40 -> 0x7668C0)
        {
            if (status == -1 || status == 1)
                return;
            buffer[0] = 0x76;
            buffer[1] = 0x01;
            buffer[index] = 0;
            reinterpret_cast<void(__thiscall*)(CVar*, const char*, int, int, int, int)>(0x7668C0)(cvar, buffer, 1, 0, 0, 1);
        }
    };
    const uint32_t kMaxListValue = 0xC4604;

    bool Visited(uint32_t id)   // FUN_10335020
    {
        if (!g_visited)
            return false;
        IntList list(g_visited);
        for (uint32_t v = list.Next(); v; v = list.Next())
            if (v == id)
                return true;
        return false;
    }

    void MarkVisited(uint32_t id)   // FUN_10334020
    {
        if (id >= kMaxListValue)
        {
            AscLog::Printf("AddSuperTrackToVisitedList.id >=  MAX_CVAR_INT_LIST_VALUE");
            return;
        }
        if (!g_visited || Visited(id))
            return;
        std::vector<uint32_t> ids;
        {
            IntList read(g_visited);
            for (uint32_t v = read.Next(); v; v = read.Next())
                ids.push_back(v);
        }
        ids.push_back(id);
        if (ids.size() > 20)
            ids.erase(ids.begin(), ids.end() - 20);
        IntList write(g_visited);
        for (uint32_t v : ids)
        {
            if (v > kMaxListValue)
                AscLog::Printf("CVarIntListWrapper::AddNextValue (value > %u)", kMaxListValue);
            if (write.index > 0xFC)
                AscLog::Printf("CVarIntListWrapper::AddNextValue (m_listIndex >= %u)", 0xFC);
            write.Add(v);
        }
        write.Store();
    }

    // FUN_10194090 -> client 0x5E1460(quest, query), over the query FUN_10194010 builds with the
    // objective index and +4 / +0xC set. Returns the query's last dword.
    uint32_t ObjectiveQuery(uint32_t quest, uint32_t objective)
    {
        std::vector<uint32_t> q(0x41A, 0);
        q[0] = objective;
        q[1] = 1;
        q[3] = 1;
        q[4] = 0x5DE930;
        q[5] = 0x5DEA10;
        q[6] = 0x5DE9E0;
        q[7] = 0x5DEA40;
        reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t*)>(0x5E1460)(quest, q.data());
        return q[0x419];
    }

    float DistanceTo(const float p[3], const uint8_t* row)
    {
        const float dx = p[0] - AscDbc::Table::F32(row, 8), dy = p[1] - AscDbc::Table::F32(row, 0xC),
                    dz = p[2] - AscDbc::Table::F32(row, 0x10);
        return std::sqrt(dz * dz + dx * dx + dy * dy);
    }

    // FUN_103345f0: the SuperTrack id to follow for QuestSuperTrack row `qrow`. Objective 0..3 = every
    // objective whose query comes back 0 contributes its 20-id list (+8 + k*0x50); -1 = the completion
    // list (+0x148). Each listed id is walked past already-visited chain nodes, then along the chain
    // (only while both nodes carry +0x1C) to the node nearest the player; nodes skipped on the way are
    // marked visited. The first candidate is taken; later ones only on the current map and nearer
    // (compared by the distance of the node the walk started from).
    uint32_t BestSuperTrack(const uint8_t* qrow, int32_t objective)
    {
        AscDbc::Table& st = SuperTrackDbc();
        float p[3] = {0, 0, 0};
        PlayerPosition(p);
        const uint8_t* lists[4] = {};
        const uint32_t quest = AscDbc::Table::U32(qrow, 4);
        if (objective >= 0 && objective <= 3)
        {
            for (uint32_t k = 0; k < 4; ++k)
                if (ObjectiveQuery(quest, k) == 0)
                    lists[k] = qrow + 8 + k * 0x50;
        }
        else if (objective == -1)
            lists[0] = qrow + 0x148;

        uint32_t bestId = 0;
        float bestDistance = 3.4028235e38f;
        for (const uint8_t* list : lists)
        {
            if (!list)
                continue;
            for (uint32_t i = 0; i < 20; ++i)
            {
                const uint32_t id = AscDbc::Table::U32(list, i * 4);
                const uint8_t* cur = id ? st.Row(id) : nullptr;
                if (!cur)
                    continue;
                for (;;)
                {
                    const uint32_t nextId = AscDbc::Table::U32(cur, 0x18);
                    const uint8_t* next = nextId ? st.Row(nextId) : nullptr;
                    if (!next || !Visited(AscDbc::Table::U32(cur, 0)))
                        break;
                    cur = next;
                }
                const float start = DistanceTo(p, cur);
                const uint8_t* best = cur;
                float nearest = start;
                std::vector<uint32_t> walked(1, AscDbc::Table::U32(cur, 0)), skipped;
                if (AscDbc::Table::U32(cur, 0x18))
                {
                    const uint8_t* n = st.Row(AscDbc::Table::U32(cur, 0x18));
                    while (n)
                    {
                        if (!AscDbc::Table::U32(n, 0x18) || !AscDbc::Table::U32(best, 0x1C) || !AscDbc::Table::U32(n, 0x1C))
                            break;
                        const float d = DistanceTo(p, n);
                        if (d < nearest)
                        {
                            nearest = d;
                            best = n;
                            skipped = walked;
                        }
                        n = st.Row(AscDbc::Table::U32(n, 0x18));
                        if (n)
                            walked.push_back(AscDbc::Table::U32(n, 0));
                        if (!AscDbc::Table::U32(best, 0x18))
                            break;
                    }
                }
                for (uint32_t s : skipped)
                    MarkVisited(s);
                if (bestId == 0 || (AscDbc::Table::I32(best, 4) == CurrentMap() && start < bestDistance))
                {
                    bestDistance = start;
                    bestId = AscDbc::Table::U32(best, 0);
                }
            }
        }
        return bestId;
    }

    // FUN_10336440: the tracked quest's target -- its QuestSuperTrack row's objective / completion
    // SuperTrack node, else the quest POI from client 0x5DEEE0 (x, y as ints, z 0, radius 5.5; +0x1C
    // is left as it was).
    bool ResolveQuestTarget()
    {
        if (!g_track.quest)
            return false;
        bool complete = false;
        int32_t map = 99999, floor = 0, x = 0, y = 0, unk = -1;
        typedef bool(__cdecl* PoiFn)(uint32_t, bool*, int32_t*, int32_t*, int32_t*, int32_t*, int32_t*);
        if (!reinterpret_cast<PoiFn>(0x5DEEE0)(g_track.quest, &complete, &map, &floor, &x, &y, &unk))
            return false;

        AscDbc::Table& qst = QuestSuperTrackDbc();
        for (uint32_t id = qst.MinId(); qst.Count() && id <= qst.MaxId(); ++id)
        {
            const uint8_t* qrow = qst.Row(id);
            if (!qrow || AscDbc::Table::U32(qrow, 4) != g_track.quest)
                continue;
            const uint8_t* row = nullptr;
            if (!complete)
            {
                for (uint32_t k = 0; k < 4 && !row; ++k)
                    if (ObjectiveQuery(g_track.quest, k) == 0)
                    {
                        const uint32_t st = BestSuperTrack(qrow, static_cast<int32_t>(k));
                        row = st ? SuperTrackDbc().Row(st) : nullptr;
                    }
            }
            else
                row = SuperTrackDbc().Row(BestSuperTrack(qrow, -1));   // FUN_10334e90
            if (row)
            {
                g_track.target = FromRow(row);
                return true;
            }
            break;
        }
        g_track.target.id = 0;
        g_track.target.map = map;
        g_track.target.x = static_cast<float>(x);
        g_track.target.y = static_cast<float>(y);
        g_track.target.z = 0;
        g_track.target.radius = 5.5f;
        g_track.target.next = 0;
        return true;
    }

    // ---- timers ----------------------------------------------------------------------------------
    int __cdecl ResolveTick(void*)   // 0x10336410
    {
        ResolveQuestTarget();
        g_timers[0] = AscRuntime::Schedule(1000, ResolveTick, nullptr);
        return 0;
    }

    int __cdecl ArrivalTick(void*)   // 0x103362F0
    {
        if (TargetState() == 4 && !Visited(g_track.target.id))
            MarkVisited(g_track.target.id);
        g_timers[1] = AscRuntime::Schedule(100, ArrivalTick, nullptr);
        return 0;
    }

    int __cdecl QuestChangeTick(void*)   // 0x10336340
    {
        if (!g_track.fresh && g_lastQuest && g_lastQuest != g_track.quest && g_visited)
        {
            IntList list(g_visited);
            list.Store();
        }
        g_lastQuest = g_track.quest;
        g_track.fresh = false;
        g_timers[2] = AscRuntime::Schedule(1000, QuestChangeTick, nullptr);
        return 0;
    }

    void StopTimers()   // 0x103350C0 (glue Lua init)
    {
        const AscRuntime::TimerFn fns[3] = {ResolveTick, ArrivalTick, QuestChangeTick};
        for (int i = 0; i < 3; ++i)
            if (g_timers[i])
            {
                AscRuntime::Cancel(g_timers[i], fns[i], nullptr);
                g_timers[i] = 0;
            }
    }

    void OnWorldLuaInit()   // 0x103345A0 (FUN_102787c0 list): tracker cleared, fresh, no last quest
    {
        g_track.quest = 0;
        g_track.target = Target{};
        g_track.fresh = true;
        g_lastQuest = 0;
    }

    // 0x103362A0 (FUN_10278510): the three timers. EnterWorld repeats on /reload and far
    // teleports without a glue screen in between, so cancel the running chains before re-arming.
    void OnEnterWorld()
    {
        StopTimers();
        g_timers[0] = AscRuntime::Schedule(1000, ResolveTick, nullptr);
        g_timers[1] = AscRuntime::Schedule(100, ArrivalTick, nullptr);
        g_timers[2] = AscRuntime::Schedule(1000, QuestChangeTick, nullptr);
    }

    // SMSG 0x740 SUPER_TRACKER_SET_POSITION (0x10334EC0): i32 map, f32 x, y, z; quest 0, id 0,
    // radius 5.5, next 0 (+0x1C untouched).
    void __cdecl OnSetPosition(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(p->m_buffer) + p->m_read;
        memcpy(&g_track.target.map, b, 4);
        memcpy(&g_track.target.x, b + 4, 4);
        memcpy(&g_track.target.y, b + 8, 4);
        memcpy(&g_track.target.z, b + 12, 4);
        p->m_read += 16;
        g_track.quest = 0;
        g_track.target.id = 0;
        g_track.target.radius = 5.5f;
        g_track.target.next = 0;
        Changed();
    }

    // FUN_10336140: (questID) -- tracker cleared to the quest, its target resolved, event fired.
    int SetSuperTrackedQuestID(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const uint32_t quest = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const uint32_t keep = g_track.target.unk1c;   // +0x1C is not written
        g_track.quest = quest;
        g_track.target = Target{};
        g_track.target.unk1c = keep;
        ResolveQuestTarget();
        Changed();
        return 0;
    }

    // FUN_10335400: PositionFrame(bool). true: client flag 0xD3F798 = 0 and the two x87 blocks at
    // 0x49DDBF (0x1C bytes) and 0x49DDE5 (0x1F) become NOPs; false: flag = 1 and the stock bytes go
    // back. (The original writes through a hash-resolved NtProtectVirtualMemory, RWX then restore.)
    int PositionFrame(lua_State* L)
    {
        bool on;
        if (!ReadBool(L, on))
            return 0;
        static uint8_t stockA[0x1C] = {0xE8, 0x1C, 0xE2, 0xFD, 0xFF, 0xD8, 0x0D, 0x0C, 0x30, 0x9E, 0x00, 0x83,
            0xC4, 0x04, 0xD8, 0x7D, 0xF4, 0xD9, 0x1C, 0x24, 0xE8, 0x98, 0xE2, 0xFD, 0xFF, 0xD9, 0x5D, 0xF4};
        static uint8_t stockB[0x1F] = {0xD9, 0x5D, 0xF8, 0xE8, 0xF3, 0xE1, 0xFD, 0xFF, 0xD8, 0x0D, 0x0C, 0x30,
            0x9E, 0x00, 0x83, 0xC4, 0x08, 0xD8, 0x7D, 0xF8, 0xD9, 0x1C, 0x24, 0xE8, 0x6F, 0xE2, 0xFD, 0xFF,
            0x83, 0xC4, 0x04};
        *reinterpret_cast<uint8_t*>(0xD3F798) = on ? 0 : 1;
        if (on)
        {
            Util::OverwriteBytesAtAddress(reinterpret_cast<void*>(0x49DDBF), 0x90, sizeof(stockA));
            Util::OverwriteBytesAtAddress(reinterpret_cast<void*>(0x49DDE5), 0x90, sizeof(stockB));
        }
        else
        {
            Util::OverwriteBytesAtAddress(0x49DDBF, stockA, sizeof(stockA));
            Util::OverwriteBytesAtAddress(0x49DDE5, stockB, sizeof(stockB));
        }
        return 0;
    }

    int ClearSuperTracker(lua_State*)
    {
        g_track.quest = 0;
        g_track.target = Target{};
        return 0;
    }

    int IsSuperTrackingAnything(lua_State* L)   // FUN_10217c70
    {
        PushBool(L, g_track.target.map != -1);
        return 1;
    }

    // FUN_10217ac0: 0 off-map, 4 inside the radius, else 2.
    int32_t TargetState()
    {
        if (g_track.target.map != CurrentMap())
            return 0;
        return Distance() <= g_track.target.radius ? 4 : 2;
    }

    int GetTargetState(lua_State* L)
    {
        PushInt(L, TargetState());
        return 1;
    }

    int GetSuperTrackedWorldPosition(lua_State* L)
    {
        if (!Tracking())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        float x = 0, y = 0, z = 0, d = 0;
        if (g_track.target.map == CurrentMap())
        {
            x = g_track.target.x;
            y = g_track.target.y;
            z = TargetZ();
            d = Distance();
        }
        PushNum(L, x);
        PushNum(L, y);
        PushNum(L, z);
        PushNum(L, d);
        return 4;
    }

    // FUN_102179e0: world -> screen through the world frame *(0xB7436C) (0x4F6D20), then the client's
    // coordinate conversions 0x493D70 / 0x493E00.
    int GetSuperTrackedPosition(lua_State* L)
    {
        if (!Tracking())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        float sx = 0, sy = 0, d = 0;
        if (g_track.target.map == CurrentMap())
        {
            const float world[3] = {g_track.target.x, g_track.target.y, TargetZ()};
            float screen[3] = {0, 0, 0};
            void* frame = *reinterpret_cast<void**>(0xB7436C);
            reinterpret_cast<void(__thiscall*)(void*, const float*, float*, int)>(0x4F6D20)(frame, world, screen, 0);
            sx = reinterpret_cast<float(__cdecl*)(float)>(0x493D70)(screen[0]);
            sy = reinterpret_cast<float(__cdecl*)(float)>(0x493E00)(screen[1]);
            d = Distance();
        }
        PushNum(L, sx);
        PushNum(L, sy);
        PushNum(L, d);
        return 3;
    }

    // FUN_10335fb0: (x, y, z, map); radius 5.5.
    int SetSuperTrackedPosition(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, NUMBER, NUMBER}))
            return 0;
        Target t;
        t.x = static_cast<float>(CheckNumber(L, 1));
        t.y = static_cast<float>(CheckNumber(L, 2));
        t.z = static_cast<float>(CheckNumber(L, 3));
        t.map = ToInt(CheckNumber(L, 4));
        g_track.quest = 0;
        g_track.target = t;
        Changed();
        return 0;
    }

    // FUN_10335eb0: the current map (0xBD088C) and the position at 0xBD0A58.
    int SetSuperTrackedCorpse(lua_State* L)
    {
        bool unused;
        if (!ReadBool(L, unused))
            return 0;
        const float* pos = reinterpret_cast<const float*>(0xBD0A58);
        Target t;
        t.map = CurrentMap();
        t.x = pos[0];
        t.y = pos[1];
        t.z = pos[2];
        g_track.quest = 0;
        g_track.target = t;
        Changed();
        return 0;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_SuperTrack", "ClearSuperTracker", ClearSuperTracker},
        {"C_SuperTrack", "IsSuperTrackingAnything", IsSuperTrackingAnything},
        {"C_SuperTrack", "GetTargetState", GetTargetState},
        {"C_SuperTrack", "GetSuperTrackedWorldPosition", GetSuperTrackedWorldPosition},
        {"C_SuperTrack", "GetSuperTrackedPosition", GetSuperTrackedPosition},
        {"C_SuperTrack", "SetSuperTrackedPosition", SetSuperTrackedPosition},
        {"C_SuperTrack", "SetSuperTrackedCorpse", SetSuperTrackedCorpse},
        {"C_SuperTrack", "SetSuperTrackedQuestID", SetSuperTrackedQuestID},
        {"C_SuperTrack", "PositionFrame", PositionFrame},
    };
    int __cdecl AcceptAnyValue() { return 1; }   // 0x102A0990

    void Init()   // 0x10334FB0
    {
        // FUN_10114540 with &0x10D3C1A4 as the output slot: no help, flags 0x21, default "", category 4.
        // The world registration writes the new CVar* into g_visited each time it runs.
        AscClientOptions::CVarSpec visited{"VisitedSuperTracks", "", 0x21, 4, reinterpret_cast<void*>(&AcceptAnyValue)};
        visited.out = reinterpret_cast<void**>(&g_visited);
        AscClientOptions::QueueWorldCVar(visited);
        sDC.AddPacketHandler(0x740, CNetClientCustomPacket((void*)&OnSetPosition, nullptr));
        AscBindings::OnWorldRegistered(OnWorldLuaInit);
        AscRuntime::OnEnterWorld(OnEnterWorld);
        AscBindings::OnGlueRegistered(StopTimers);
    }

    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}
