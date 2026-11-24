// The network profiler -- LogOpcodes / ResetOpcodeLogging / SetOpcodeLoggingEnabled and their signal-event
// siblings (0x102D35A0..0x102D5CD0).
//
// With opcode logging on (0x10BE3778), every packet received (FUN_102c3ab0, our CNetClient.cpp receive
// path) and sent (FUN_102c3ff0, a detour on ClientServices::Send 0x632B50) is counted in a map keyed
// "<name> (0x%08X)" / "SENT <name> (0x%08X)" (names: FUN_102c4a90's table). Each record (FUN_102c45b0,
// FUN_102c46a0):
//   +0x00 bytes  +0x04 max size  +0x08 min size (-1)  +0x0C count  +0x10 last ms  +0x18 first ms
//   +0x20 avg size  +0x24 packets/s  +0x28 bytes/s (floats; rates over max(last - first, 1 s))
//   +0x30 total us  +0x38 max us  +0x40 min us (-1)  +0x48 avg us (float, over ALL packets)  +0x4C timed
// Receive handlers are timed when one ran; sends are always timed.
//
// LogOpcodes (FUN_102d35a0) writes the table to the Trace channel, sorted by bytes/s descending, then the
// warnings pass (FUN_102d2620), then resets. Set*LoggingEnabled(on, periodic) set the flag, cancel the two
// shared timers, re-arm both at 5 s when on and periodic (each callback dumps and schedules itself again
// WITHOUT storing the new handle, so a later cancel misses it -- kept), and reset.
//
// Signal events: nothing in the original ever inserts into the event map 0x10BE37AC (the flag 0x10BE3779
// has no reader), so LogSignalEvents (FUN_102d4ba0) always finds it empty, writes nothing and resets.
// Its 3.7 KB formatter is therefore unreachable and not transcribed.
#include <Ascension/AscProfiler.hpp>
#include <Misc/DataContainer.hpp>
#include <Windows.h>
#include <process.h>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLogger.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    const char* const kOpcodeNames[] = {
#include <Ascension/AscOpcodeNames.generated.inc>
    };

    struct Record
    {
        uint32_t bytes = 0, maxSize = 0, minSize = 0xFFFFFFFF, count = 0;
        uint64_t lastMs = 0, firstMs = 0;
        float avgSize = 0, packetsPerSec = 0, bytesPerSec = 0;
        uint64_t totalUs = 0, maxUs = 0, minUs = 0xFFFFFFFFFFFFFFFFull;
        float avgUs = 0;
        bool timed = false;
    };

    // MSVC's std::hash<std::string> is the original's FNV-1a (FUN_102c2f30), so iteration order matches.
    std::unordered_map<std::string, Record> g_opcodes;   // 0x10BE3788
    bool g_opcodeLogging = false;                        // 0x10BE3778
    bool g_eventLogging = false;                         // 0x10BE3779 (no reader)
    uint32_t g_opcodeTimer = 0, g_eventTimer = 0;        // 0x10BE37D4 / 0x10BE37D8

    int64_t NowNs()   // FUN_100b8230
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    uint64_t NowMs()   // _Xtime_get_ticks() / 10000
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }

    std::string Key(const char* prefix, int opcode)   // "{} (0x{:08X})"
    {
        const char* name = static_cast<uint32_t>(opcode) < 0x9D5 ? kOpcodeNames[opcode] : "unknown";
        char buf[160];
        snprintf(buf, sizeof(buf), "%s%s (0x%08X)", prefix, name, static_cast<uint32_t>(opcode));
        return buf;
    }

    // FUN_102c45b0
    void Count(Record& r, uint32_t size, uint64_t ms)
    {
        if (r.firstMs == 0)
            r.firstMs = ms;
        r.bytes += size;
        r.maxSize = std::max(r.maxSize, size);
        r.minSize = std::min(r.minSize, size);
        ++r.count;
        r.lastMs = ms;
        const float bytes = static_cast<float>(static_cast<double>(r.bytes));
        const float count = static_cast<float>(static_cast<double>(r.count));
        r.avgSize = bytes / count;
        const float seconds = std::max(static_cast<float>(r.lastMs - r.firstMs) / 1000.0f, 1.0f);
        r.packetsPerSec = count / seconds;
        r.bytesPerSec = bytes / seconds;
    }

    // FUN_102c46a0
    void Time(Record& r, uint64_t us)
    {
        r.timed = true;
        r.totalUs += us;
        r.maxUs = std::max(r.maxUs, us);
        r.minUs = std::min(r.minUs, us);
        r.avgUs = static_cast<float>(r.totalUs) / static_cast<float>(static_cast<double>(r.count));
    }

    // FUN_102d5a20
    void ResetOpcodes()
    {
        for (auto& e : g_opcodes)
            e.second = Record{};
    }

    // ---- formatting ------------------------------------------------------------------------------------
    std::string Shortest(float v)   // fmt "{}" of a float
    {
        char buf[48];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        return std::string(buf, r.ptr);
    }
    std::string Fixed(const char* fmt, double v)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), fmt, v);
        return buf;
    }
    std::string Bytes(uint32_t n)   // FUN_102d3360
    {
        if (n < 0x400)
            return std::to_string(n) + " B";
        if (n < 0x100000)
            return Fixed("%.2f KB", static_cast<float>(static_cast<double>(n)) * 0.0009765625f);
        return Fixed("%.2f MB", static_cast<float>(static_cast<double>(n)) * 9.536743e-07f);
    }
    std::string Micros(uint64_t us)   // FUN_102d3450
    {
        if (us == 0xFFFFFFFFFFFFFFFFull)
            return "N/A";
        if (us < 1000)
            return std::to_string(us) + " \xC2\xB5s";
        if (us < 1000000)
            return Fixed("%.2f ms", static_cast<float>(us) / 1000.0f);
        return Fixed("%.2f s", static_cast<float>(us) / 1e+06f);
    }
    std::string Columns(const std::string& a, const std::string& b, const std::string& c, const std::string& d,
                        const std::string& e, const std::string& f)   // "{:<80} {:>10} {:>10} {:>10} {:>10} {:>10}"
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "%-80s %10s %10s %10s %10s %10s", a.c_str(), b.c_str(), c.c_str(), d.c_str(), e.c_str(), f.c_str());
        return buf;
    }
    void Trace(const std::string& line) { AscLogger::Write(0, line); }

    // FUN_102d2620: per-record warnings, then the block when there are any.
    void Warnings()
    {
        std::vector<std::string> out;
        for (const auto& e : g_opcodes)
        {
            const Record& r = e.second;
            std::vector<std::string> w;
            if (1048576.0f < r.bytesPerSec)
                w.push_back("- High bandwidth usage: " + Bytes(static_cast<uint32_t>(static_cast<int64_t>(r.bytesPerSec))) + "/s");
            if (0x500000 < r.bytes)
                w.push_back("- High total bandwidth: " + Bytes(r.bytes));
            if (1000.0f < r.packetsPerSec)
                w.push_back(Fixed("- High packet rate: %.1f packets/s", r.packetsPerSec));
            if (5000 < r.count)
                w.push_back("- High total packet count: " + std::to_string(r.count) + " packets");
            const float maxSize = static_cast<float>(static_cast<double>(r.maxSize));
            if (r.avgSize + r.avgSize < maxSize)
                w.push_back("- Packet size spike detected: max " + Bytes(r.maxSize) + " (" +
                            Shortest(maxSize / (r.avgSize <= 0.0f ? 1.0f : r.avgSize)) + "x average)");
            if (r.timed && r.avgUs + r.avgUs < static_cast<float>(r.maxUs))
                w.push_back("- Performance spike detected: max " + Micros(r.maxUs) + " (" +
                            Shortest(static_cast<float>(r.maxUs) / (r.avgUs <= 0.0f ? 1.0f : r.avgUs)) + "x average)");
            const float avgSize = r.avgSize <= 0.0f ? 1.0f : r.avgSize;
            const int32_t spread = static_cast<int32_t>(r.maxSize - r.minSize);
            const float spreadF = static_cast<float>(static_cast<double>(static_cast<uint32_t>(spread)));
            if (5.0f < spreadF / avgSize)
                w.push_back("- High packet size variance: " + Bytes(r.minSize) + " to " + Bytes(r.maxSize) + " (" +
                            Shortest(spreadF / avgSize) + "x difference)");
            if (r.timed)
            {
                const float avgUs = r.avgUs <= 0.0f ? 1.0f : r.avgUs;
                if (5.0f < static_cast<float>(r.maxUs) / avgUs)
                    w.push_back("- High processing time variance: " + Micros(r.minUs) + " to " + Micros(r.maxUs) + " (" +
                                Shortest(static_cast<float>(r.maxUs) / avgUs) + "x difference)");
            }
            if (!w.empty())
            {
                out.push_back("\nWarnings for " + e.first + ":");
                out.insert(out.end(), w.begin(), w.end());
            }
        }
        if (out.empty())
            return;
        const std::string rule(0x50, '=');
        Trace(rule);
        Trace("=== Network Analysis Warnings ===");
        for (const std::string& line : out)
            Trace(line);
        Trace(rule);
    }

    // FUN_102d35a0
    void DumpOpcodes()
    {
        struct Row { std::string key; Record r; };
        std::vector<Row> rows;
        uint32_t bytes = 0, count = 0;
        uint64_t timedUs = 0, firstMs = 0xFFFFFFFFFFFFFFFFull, lastMs = 0;
        for (const auto& e : g_opcodes)
        {
            const Record& r = e.second;
            if (!r.count)
                continue;
            rows.push_back({e.first, r});
            bytes += r.bytes;
            count += r.count;
            if (r.timed)
                timedUs += r.totalUs;
            if (r.firstMs && r.firstMs <= firstMs)
                firstMs = r.firstMs;
            if (r.lastMs && r.lastMs >= lastMs)
                lastMs = r.lastMs;
        }
        if (rows.empty())
        {
            ResetOpcodes();
            return;
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.r.bytesPerSec > b.r.bytesPerSec; });

        float seconds = static_cast<float>(lastMs - firstMs) / 1000.0f;
        if (seconds <= 1.0f)
            seconds = 1.0f;
        const float countF = static_cast<float>(static_cast<double>(count));
        const float avgUs = count ? static_cast<float>(timedUs) / countF : 0.0f;
        const float packetsPerSec = countF / seconds;
        const float bytesPerSec = static_cast<float>(static_cast<double>(bytes)) / seconds;

        char clock[32];
        const time_t now = time(nullptr);
        strftime(clock, sizeof(clock), "%H:%M:%S", localtime(&now));
        const std::string header = std::string("=== Network Statistics (") + clock + ") ===";
        const std::string rule(header.size(), '=');
        Trace(rule);
        Trace(header);
        Trace(rule);
        Trace(Fixed("Duration: %.1fs", seconds) + " | Bandwidth: " + Bytes(bytes) + " (" +
              Bytes(static_cast<uint32_t>(static_cast<int64_t>(bytesPerSec))) + "/s) | Packets: " + std::to_string(count) +
              Fixed(" (%.1f/s)", packetsPerSec) + " | Processing: " + Micros(timedUs) + " (avg: " +
              Micros(static_cast<uint64_t>(static_cast<int64_t>(avgUs))) + ")");
        Trace(rule);
        Trace(Columns("Opcode", "BW/s", "Total", "Pkts/s", "Count", "Avg Size"));
        Trace(Columns("", "Min Time", "Max Time", "Avg Time", "Total Time", "% Time"));
        for (const Row& row : rows)
        {
            const Record& r = row.r;
            std::string name = row.key;
            if (name.size() > 0x4D)
                name = name.substr(0, 0x4A) + "...";
            std::string first = Columns(name, Bytes(static_cast<uint32_t>(static_cast<int64_t>(r.bytesPerSec))), Bytes(r.bytes),
                                        Shortest(r.packetsPerSec), std::to_string(r.count),
                                        Bytes(static_cast<uint32_t>(static_cast<int64_t>(r.avgSize))));
            const float pct = timedUs ? static_cast<float>(r.totalUs) * 100.0f / static_cast<float>(timedUs) : 0.0f;
            char second[512];
            snprintf(second, sizeof(second), "%-80s %10s %10s %10s %10s %9.1f%%", "",
                     r.timed ? Micros(r.minUs).c_str() : "N/A", r.timed ? Micros(r.maxUs).c_str() : "N/A",
                     r.timed ? Micros(static_cast<uint64_t>(static_cast<int64_t>(r.avgUs))).c_str() : "N/A",
                     r.timed ? Micros(r.totalUs).c_str() : "N/A", pct);
            if (1048576.0f < r.bytesPerSec || 1000.0f < r.packetsPerSec ||
                r.avgSize + r.avgSize < static_cast<float>(static_cast<double>(r.maxSize)))
                first += " [!]";
            else if (r.timed && r.avgUs + r.avgUs < static_cast<float>(r.maxUs))
                first += " [!]";
            Trace(first);
            Trace(second);
        }
        Trace(rule);
        Warnings();
        ResetOpcodes();
    }

    // FUN_102d4ba0 over an always-empty map: nothing to write; resets (FUN_102d5ac0).
    void DumpEvents() {}

    int __cdecl OpcodeTimer(void*)   // LAB_102d5cb0
    {
        DumpOpcodes();
        g_opcodeTimer = AscRuntime::Schedule(5000, &OpcodeTimer, nullptr);
        return 0;
    }
    int __cdecl EventTimer(void*)    // LAB_102d5cd0
    {
        DumpEvents();
        g_eventTimer = AscRuntime::Schedule(5000, &EventTimer, nullptr);
        return 0;
    }

    // The shared body of Set{Opcode,SignalEvent}LoggingEnabled (FUN_100b8b40: {boolean, boolean}).
    bool SetLogging(lua_State* L, bool& flag)
    {
        if (!ValidateInput(L, {AscScript::BOOLEAN, AscScript::BOOLEAN}))
            return false;
        const bool on = AscLua::lua_toboolean(L, 1) != 0;
        const bool periodic = AscLua::lua_toboolean(L, 2) != 0;
        flag = on;
        if (g_opcodeTimer)
        {
            AscRuntime::Cancel(g_opcodeTimer, &OpcodeTimer, nullptr);
            g_opcodeTimer = 0;
        }
        if (g_eventTimer)
        {
            AscRuntime::Cancel(g_eventTimer, &EventTimer, nullptr);
            g_eventTimer = 0;
        }
        if (periodic && on)
        {
            g_opcodeTimer = AscRuntime::Schedule(5000, &OpcodeTimer, nullptr);   // FUN_102d5c70
            g_eventTimer = AscRuntime::Schedule(5000, &EventTimer, nullptr);
        }
        return true;
    }

    int LogOpcodes(lua_State*)              { DumpOpcodes(); return 0; }
    int LogSignalEvents(lua_State*)         { DumpEvents(); return 0; }
    int ResetOpcodeLogging(lua_State*)      { ResetOpcodes(); return 0; }
    int ResetSignalEventLogging(lua_State*) { return 0; }
    int SetOpcodeLoggingEnabled(lua_State* L)
    {
        if (SetLogging(L, g_opcodeLogging))
            ResetOpcodes();
        return 0;
    }
    int SetSignalEventLoggingEnabled(lua_State* L)
    {
        SetLogging(L, g_eventLogging);
        return 0;
    }

    // FUN_102c3ff0 over ClientServices::Send (0x632B50, __thiscall(services, packet)).
    typedef int(__thiscall* Send_t)(void*, CDataStore*);
    Send_t g_send = nullptr;
    int __fastcall SendDetour(void* services, void*, CDataStore* packet)
    {
        const bool on = g_opcodeLogging;
        const int64_t start = on ? NowNs() : 0;
        const int16_t opcode = *reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(packet->m_buffer) + packet->m_read);
        packet->m_read += 2;
        std::string key;
        if (on)
        {
            key = Key("SENT ", opcode);
            Count(g_opcodes[key], packet->m_size, NowMs());
        }
        packet->m_read = 0;
        const int r = g_send(services, packet);
        if (on)
            Time(g_opcodes[key], static_cast<uint64_t>((NowNs() - start) / 1000));
        return r;
    }

    // ---- the packet watchdog (installer FUN_102c3540) ----------------------------------------------------
    // Armed on entering the world (FUN_102c34f0), disarmed on leaving it and before 0x528F00 (FUN_102c34c0).
    // A thread (0x102C2ED0 -> FUN_102c4470) checks every second; 30 s without a received packet logs a
    // Warning and flags a disconnect, done on the next frame after 0x403340 (FUN_102c3510 -> 0x6B12D0).
    volatile uint64_t g_lastReceive = 0;   // 0x10BE3570 (GetTickCount64 of the last packet)
    volatile bool g_armed = false;         // 0x10BE3569
    volatile bool g_tripped = false;       // 0x10BE356A
    volatile bool g_disconnect = false;    // 0x10BE356B

    unsigned __stdcall Watchdog(void*)
    {
        for (;;)
        {
            if (g_armed && !g_tripped && g_lastReceive && GetTickCount64() - g_lastReceive >= 30000)
            {
                g_tripped = true;
                g_disconnect = true;
                g_armed = false;
                AscLogger::Write(4, "Packet watchdog timed out after 30 seconds without receiving a packet; forcing disconnect.");
            }
            Sleep(1000);
        }
    }
    void Arm()
    {
        g_lastReceive = GetTickCount64();
        g_tripped = false;
        g_armed = true;
    }
    void Disarm()
    {
        g_armed = false;
        g_tripped = false;
        g_disconnect = false;
        g_lastReceive = 0;
    }
    void DisconnectIfTripped()
    {
        if (!g_disconnect)
            return;
        if (void* connection = reinterpret_cast<void*(__cdecl*)()>(0x6B0970)())
        {
            g_disconnect = false;
            reinterpret_cast<void(__thiscall*)(void*)>(0x6B12D0)(connection);
        }
    }

    // FUN_102c3fc0-style clear on 0x631FC0 (__thiscall(net, opcode), ret 4): above 0x55E the original zeroes
    // the opcode's entries in its own tables (a later packet would call a null handler); ours drops it.
    typedef int(__fastcall* Clear_t)(void*, void*, uint32_t);
    Clear_t g_631FC0 = nullptr;
    int __fastcall ClearHandler(void* net, void* edx, uint32_t opcode)   // FUN_102c3f80
    {
        if (static_cast<int32_t>(opcode) > 0x55E)
        {
            sDC.GetPacketHandlerMap().erase(opcode);
            return 0;
        }
        return g_631FC0(net, edx, opcode);
    }

    void Init()
    {
        g_send = reinterpret_cast<Send_t>(AscRuntime::Detour(0x632B50, 6, reinterpret_cast<void*>(&SendDetour)));
        g_631FC0 = reinterpret_cast<Clear_t>(AscRuntime::Detour(0x631FC0, 8, reinterpret_cast<void*>(&ClearHandler)));
        CloseHandle(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &Watchdog, nullptr, 0, nullptr)));
        AscRuntime::OnEnterWorld(&Arm);
        AscRuntime::OnLeaveWorld(&Disarm);
        AscRuntime::OnBefore528F00(&Disarm);
        AscRuntime::OnAfter403340(&DisconnectIfTripped);
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "LogOpcodes", LogOpcodes},
        {nullptr, "LogSignalEvents", LogSignalEvents},
        {nullptr, "ResetOpcodeLogging", ResetOpcodeLogging},
        {nullptr, "ResetSignalEventLogging", ResetSignalEventLogging},
        {nullptr, "SetOpcodeLoggingEnabled", SetOpcodeLoggingEnabled},
        {nullptr, "SetSignalEventLoggingEnabled", SetSignalEventLoggingEnabled},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

namespace
{
    int32_t g_currentOpcode = 0;   // DAT_10BE3728
}

int32_t AscProfiler::CurrentOpcode() { return g_currentOpcode; }

int64_t AscProfiler::Received(int opcode, uint32_t size)
{
    g_lastReceive = GetTickCount64();   // FUN_102c3ab0 stores it first, for every packet
    g_currentOpcode = opcode;
    if (!g_opcodeLogging)
        return 0;
    const int64_t start = NowNs();
    Count(g_opcodes[Key("", opcode)], size, NowMs());
    return start;
}

void AscProfiler::Handled(int opcode, int64_t start)
{
    if (start)
        Time(g_opcodes[Key("", opcode)], static_cast<uint64_t>((NowNs() - start) / 1000));
}

// FUN_102c3ab0's no-handler branch for opcodes above 0x55E: "Unhandled packet: <name> (<opcode>)" (Fatal).
void AscProfiler::Unhandled(int opcode)
{
    const char* name = static_cast<uint32_t>(opcode) < 0x9D5 ? kOpcodeNames[opcode] : "unknown";
    char line[160];
    snprintf(line, sizeof(line), "Unhandled packet: %s (%u)", name, static_cast<uint32_t>(opcode));
    AscLogger::Write(5, line);
}
