import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    # The text from `signature` to the matching closing brace (python -I keeps this file's directory off the path).
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    index = opening + 1
    while depth:
        depth += (source[index] == '{') - (source[index] == '}')
        index += 1
    return source[start:index]


# The calling convention of every client function AscQuestLog64List.cpp calls, as the replaced client functions call
# it (checked in IDA, 2026-11-25: ecx set by the caller, retn N of the callee). A __thiscall callee that the DLL
# also detours as __fastcall(this, edx, ...) reads ecx even where the client body does not (0x6D4F80, AscGossipQuest).
CLIENT_CONVENTIONS = {
    0x4D3790: '__cdecl', 0x4D4DB0: '__cdecl', 0x67DE90: '__thiscall', 0x65C290: '__thiscall', 0x766940: '__thiscall',
    0x615020: '__cdecl', 0x6D50C0: '__cdecl', 0x76EC80: '__stdcall', 0x5D05B0: '__cdecl', 0x51A2B0: '__thiscall',
    0x754D00: '__thiscall', 0x5DEC70: '__cdecl', 0x7F4930: '__cdecl', 0x5DEB60: '__cdecl', 0x6D4F80: '__thiscall',
    0x5E28B0: '__cdecl', 0x5DFDC0: '__cdecl', 0x5E65B0: '__cdecl', 0x5DEA60: '__cdecl', 0x5E0D20: '__cdecl',
    0x767440: '__cdecl', 0x60C1C0: '__cdecl', 0x52C680: '__cdecl', 0x5E0870: '__thiscall', 0x5216F0: '__cdecl',
    0x543770: '__cdecl', 0x5E0180: '__cdecl', 0x5E0110: '__cdecl', 0x5E2900: '__cdecl', 0x746190: '__cdecl',
    0x530840: '__cdecl', 0x5222B0: '__cdecl', 0x81B530: '__cdecl', 0x6D4D40: '__thiscall', 0x5DEC10: '__cdecl',
}


def check_client_calls(source):
    # Every reinterpret_cast of a client address to a function pointer, direct or through a typedef, against the
    # table; an address missing from it is a call site nobody audited. Returns the failure count.
    typedef = r'typedef\s+[^;()]+\(\s*(__\w+)\s*\*\s*(\w+)\s*\)'
    typedefs = {name: convention for convention, name in re.findall(typedef, source)}
    found = {}
    for convention, address in re.findall(r'reinterpret_cast<[^<>;]*?\(\s*(__\w+)\s*\*\s*\)[^<>;]*>\(\s*(0x[0-9A-Fa-f]+)\)',
                                          source):
        found.setdefault(int(address, 16), set()).add(convention)
    for name, address in re.findall(r'reinterpret_cast<(\w+_t)>\(\s*(0x[0-9A-Fa-f]+)\)', source):
        if name in typedefs:
            found.setdefault(int(address, 16), set()).add(typedefs[name])
    failures = 0
    for address, conventions in sorted(found.items()):
        if address == 0x5E71A0:   # InitializeGame: the Detour trampoline, called as the port itself is declared
            continue
        ok = conventions == {CLIENT_CONVENTIONS.get(address)}
        failures += not ok
        print('%s: client call 0x%06X as %s' % ('PASS' if ok else 'FAIL', address, '/'.join(sorted(conventions))))
    missing = sorted(set(CLIENT_CONVENTIONS) - set(found))
    failures += bool(missing)
    print('%s: every audited client callee is still called (%s)' % (
        'FAIL' if missing else 'PASS', ', '.join('0x%06X' % a for a in missing) or 'all'))
    abandon = function(source, 'void __cdecl AbandonSelectedQuest()')
    ok = re.search(r'\(__thiscall\*\)\(void\*, uint32_t\)>\(0x6D4F80\)\(\s*player,', abandon) is not None
    failures += not ok
    print('%s: AbandonSelectedQuest calls 0x6D4F80 with the player in ecx' % ('PASS' if ok else 'FAIL'))
    return failures


HARNESS = r'''
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace AscScript
{
struct Sent { uint32_t opcode; std::vector<uint8_t> bytes; };
std::vector<Sent> sent;
class Packet
{
public:
    explicit Packet(uint32_t opcode) : m_opcode(opcode) { }
    Packet& U8(uint8_t v) { m_bytes.push_back(v); return *this; }
    void Send() { sent.push_back({m_opcode, m_bytes}); }
private:
    uint32_t m_opcode;
    std::vector<uint8_t> m_bytes;
};
}
using namespace AscScript;

@HEADER@
@STORE@
@HELLO@
@LIST@

int failures = 0;
void Check(bool condition, char const* name)
{
    failures += !condition;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
}

struct Bytes
{
    std::vector<uint8_t> data;
    Bytes& U8(uint32_t v) { data.push_back(static_cast<uint8_t>(v)); return *this; }
    Bytes& U16(uint32_t v) { U8(v & 0xFF); return U8(v >> 8); }
    Bytes& U32(uint32_t v) { U16(v & 0xFFFF); return U16(v >> 16); }
};

int changed = 0;
void Add(Bytes& b, uint32_t slot, uint32_t quest, uint32_t level = 0)
{
    b.U8(slot).U32(quest).U32(1).U16(1).U16(2).U16(3).U16(4).U32(99).U32(level).U32(500);
}
bool Feed(Bytes const& b) { return AscQuestLog64::Apply(b.data.data(), static_cast<uint32_t>(b.data.size())); }
void Setup()
{
    AscQuestLog64::Clear();
    changed = 0;
}

int main()
{
    AscQuestLog64::OnChanged([] { ++changed; });

    Setup();
    Bytes snap;
    snap.U8(1).U8(1).U8(64).U8(2);
    Add(snap, 30, 1234);
    Add(snap, 63, 77, 42);
    Check(Feed(snap), "Snapshot applies");
    Check(AscQuestLog64::Get(30) && AscQuestLog64::Get(30)->questId == 1234, "Snapshot slot 30 quest");
    Check(AscQuestLog64::Get(30) && AscQuestLog64::Get(30)->counters[3] == 4 && AscQuestLog64::Get(30)->time == 99
          && AscQuestLog64::Get(30)->rewardXP == 500, "Snapshot slot 30 fields");
    Check(AscQuestLog64::Get(63) && AscQuestLog64::Get(63)->level == 42, "Snapshot slot 63 level");
    Check(AscQuestLog64::LogSize() == 64, "Snapshot log size");
    Check(AscQuestLog64::Get(31) == nullptr, "Snapshot leaves slot 31 empty");
    Check(changed == 1, "Snapshot signals once");

    Setup();
    Bytes a;
    a.U8(1).U8(1).U8(64).U8(2);
    Add(a, 30, 100);
    Add(a, 31, 101);
    Feed(a);
    Bytes d;
    d.U8(1).U8(0).U8(64).U8(1);
    Add(d, 30, 0);
    Feed(d);
    Check(AscQuestLog64::Get(30) == nullptr, "Delta questId 0 clears the slot");
    Check(AscQuestLog64::Get(31) && AscQuestLog64::Get(31)->questId == 101, "Delta keeps the others");
    Check(changed == 2, "Delta signals");

    Setup();
    Bytes f1;
    f1.U8(1).U8(1).U8(64).U8(1);
    Add(f1, 30, 100);
    Feed(f1);
    Bytes f2;
    f2.U8(1).U8(1).U8(64).U8(1);
    Add(f2, 31, 101);
    Feed(f2);
    Check(AscQuestLog64::Get(30) == nullptr && AscQuestLog64::Get(31) && AscQuestLog64::Get(31)->questId == 101,
          "Full flag clears slots 25..63 first");

    Setup();
    Bytes ok;
    ok.U8(1).U8(1).U8(64).U8(1);
    Add(ok, 30, 100);
    Feed(ok);
    Bytes badSlot;
    badSlot.U8(1).U8(1).U8(50).U8(2);
    Add(badSlot, 31, 5);
    Add(badSlot, 70, 6);
    Bytes lowSlot;
    lowSlot.U8(1).U8(1).U8(50).U8(1);
    Add(lowSlot, 24, 6);
    Bytes shortPacket;
    shortPacket.U8(1).U8(1).U8(50).U8(3);
    Add(shortPacket, 31, 5);
    Add(shortPacket, 32, 6);
    Bytes badVersion;
    badVersion.U8(2).U8(1).U8(50).U8(1);
    Add(badVersion, 31, 5);
    Bytes header;
    header.U8(1).U8(1);
    Check(!Feed(badSlot) && !Feed(lowSlot) && !Feed(shortPacket) && !Feed(badVersion) && !Feed(header),
          "Malformed packets are refused");
    Check(AscQuestLog64::Get(30) && AscQuestLog64::Get(30)->questId == 100, "Malformed keeps slot 30");
    Check(AscQuestLog64::Get(31) == nullptr && AscQuestLog64::Get(32) == nullptr, "Malformed applies nothing");
    Check(AscQuestLog64::LogSize() == 64 && changed == 1, "Malformed leaves size and signals alone");

    Setup();
    Check(AscQuestLog64::LogSize() == 25, "NoPacketMeansNative log size");
    bool anySlot = false;
    for (uint32_t i = 0; i < 70; ++i)
        anySlot |= AscQuestLog64::Get(i) != nullptr;
    Check(!anySlot, "NoPacketMeansNative slots empty");

    Setup();
    Feed(ok);
    AscQuestLog64::Clear();
    Check(AscQuestLog64::Get(30) == nullptr && AscQuestLog64::LogSize() == 25, "ClearedOnGlue");

    Setup();
    SendHello();
    Check(sent.size() == 1 && sent[0].opcode == 0x8F6 && sent[0].bytes == std::vector<uint8_t>({1, 64}),
          "Hello is CMSG 0x8F6 01 40");

    Setup();
    Bytes lo;
    lo.U8(1).U8(1).U8(0).U8(1);
    Add(lo, 30, 100);
    Feed(lo);
    Check(AscQuestLog64::LogSize() == 25 && AscQuestLog64::Get(30), "log size clamps up to 25, slot applied");
    Bytes hi;
    hi.U8(1).U8(0).U8(200).U8(1);
    Add(hi, 31, 101);
    Feed(hi);
    Check(AscQuestLog64::LogSize() == 64 && AscQuestLog64::Get(31), "log size clamps down to 64, slot applied");

    Setup();
    uint8_t desc[0x278 + 25 * 0x14] = {};
    Bytes o;
    o.U8(1).U8(1).U8(64).U8(1);
    Add(o, 40, 777);
    Feed(o);
    Check(AscQuestLog64::SlotEntry(desc, 3) == desc + 0x278 + 3 * 0x14, "SlotEntry 3 is the descriptor entry");
    uint8_t const* e = AscQuestLog64::SlotEntry(desc, 40);
    uint32_t q, st, tm;
    uint16_t c3;
    memcpy(&q, e, 4);
    memcpy(&st, e + 4, 4);
    memcpy(&c3, e + 14, 2);   // counters[3]
    memcpy(&tm, e + 16, 4);
    Check(e && e != desc && q == 777 && st == 1 && c3 == 4 && tm == 99, "SlotEntry 40 mirrors the store");
    uint8_t const* empty = AscQuestLog64::SlotEntry(desc, 30);
    Check(empty && std::all_of(empty, empty + 0x14, [](uint8_t v) { return v == 0; }), "SlotEntry empty slot is zero");
    Check(AscQuestLog64::SlotEntry(desc, 40) != AscQuestLog64::SlotEntry(desc, 30), "SlotEntry mirrors are distinct");
    Check(AscQuestLog64::SlotEntry(desc, 64) == nullptr, "SlotEntry 64 is null");
    Bytes del;
    del.U8(1).U8(0).U8(64).U8(1);
    Add(del, 40, 0);
    Feed(del);
    memcpy(&q, AscQuestLog64::SlotEntry(desc, 40), 4);
    Check(q == 0, "SlotEntry re-reads after a delta");

    Setup();
    static std::vector<AscQuestLog64::Change> seen;
    AscQuestLog64::OnChanged([] { seen = AscQuestLog64::Changes(); });
    Bytes first;
    first.U8(1).U8(1).U8(64).U8(2);
    Add(first, 30, 100);
    Add(first, 31, 101);
    Feed(first);
    Check(seen.empty(), "Changes: the first packet after Clear reports none");
    Bytes resend;
    resend.U8(1).U8(1).U8(64).U8(2);
    Add(resend, 30, 100);
    Add(resend, 32, 102);
    Feed(resend);
    Check(seen.size() == 2 && seen[0].slot == 31 && seen[0].old.questId == 101
          && seen[1].slot == 32 && seen[1].old.questId == 0, "Changes: slots 31 and 32 with their old entries");
    Check(AscQuestLog64::Changes().empty(), "Changes empty outside a callback");

    // ---- the DLL's readers over 64 slots (docs/QUEST_LOG_64.md) ----
    Setup();
    static uint8_t qdesc[0x278 + 25 * 0x14];
    memset(qdesc, 0, sizeof(qdesc));
    auto setSlot = [](uint32_t slot, uint32_t quest) { memcpy(qdesc + 0x278 + slot * 0x14, &quest, 4); };
    setSlot(0, 11);
    setSlot(1, 12);
    setSlot(5, 55);   // after the empty slot 2: the genuine walk never reaches it
    Bytes ov;
    ov.U8(1).U8(1).U8(64).U8(2);
    ov.U8(40).U32(777).U32(0).U16(0).U16(0).U16(0).U16(0).U32(0).U32(21).U32(1350);
    ov.U8(50).U32(12).U32(0).U16(0).U16(0).U16(0).U16(0).U32(0).U32(30).U32(9);
    Feed(ov);
    uint32_t (*addonFn)(uint32_t) = [](uint32_t index) { return 1000u + index; };
    int32_t level = 0, xp = 0;
    const int32_t slot777 = AscQuestLog64::QuestSlot(qdesc, 777);
    Check(slot777 == 40 && AscQuestLog64::SlotValue(40, 0x3D, true, addonFn, level) && level == 21
          && AscQuestLog64::SlotValue(40, 0x24, false, addonFn, xp) && xp == 1350
          && AscQuestLog64::LinkLevel(qdesc, 777, 5, addonFn) == 21, "OverflowQuestLevelAndXp: slot 40 level 21, XP 1350, link 21");
    Check(AscQuestLog64::SlotValue(1, 0x3D, true, addonFn, level) && level == 1000 + 0x3E
          && AscQuestLog64::SlotValue(1, 0x24, false, addonFn, xp) && xp == 1000 + 0x25,
          "SlotValue: slots 0..24 read the addon field base + slot");
    Check(!AscQuestLog64::SlotValue(41, 0x3D, true, addonFn, level) && !AscQuestLog64::SlotValue(64, 0x3D, true, addonFn, level)
          && AscQuestLog64::QuestSlot(qdesc, 778) == -1 && AscQuestLog64::LinkLevel(qdesc, 778, 5, addonFn) == 5,
          "SlotValue / QuestSlot / LinkLevel: an empty or unknown slot keeps the native value");
    Check(AscQuestLog64::QuestSlot(qdesc, 55) == 5 && AscQuestLog64::QuestSlot(qdesc, 12) == 1,
          "QuestSlot: every slot read, first match");
    Check(AscQuestLog64::LinkLevel(qdesc, 12, 5, addonFn) == 30, "LinkLevel: every matching slot overrides, the last wins");
    Check(AscQuestLog64::FindLoggedQuest(qdesc, 12) == qdesc + 0x278 + 0x14 && AscQuestLog64::FindLoggedQuest(qdesc, 55) == nullptr,
          "FindLoggedQuest: the first empty slot ends the descriptor's 25");
    uint8_t const* found = AscQuestLog64::FindLoggedQuest(qdesc, 777);
    uint32_t foundId = 0;
    if (found)
        memcpy(&foundId, found, 4);
    Check(found == AscQuestLog64::SlotEntry(qdesc, 40) && foundId == 777 && AscQuestLog64::FindLoggedQuest(qdesc, 999) == nullptr,
          "FindLoggedQuest: slots 25..63 are read sparsely past the stop");

    // ---- the quest list (AscQuestLog64List.cpp) ----
    using namespace AscQuestLog64List;
    static List list;
    list = {};
    Check(Append(list, 100, 0, 12) == 0 && Append(list, 101, 1, 12) == 2 && Append(list, 102, 30, -5) == 3,
          "ListBuild: quest rows, a header after each new sort type");
    Check(list.count == 5 && list.numSortTypes == 2 && list.rows[1].isHeader && list.rows[1].questId == 12
          && list.rows[4].isHeader && list.rows[4].questId == -5 && list.rows[3].logSlot == 30,
          "ListBuild: headers carry the sort type, quests their slot");
    list = {};
    for (uint32_t slot = 0; slot < 64; ++slot)
        Append(list, static_cast<int32_t>(1000 + slot), slot, static_cast<int32_t>(slot + 1));
    Check(list.count == 128 && list.numSortTypes == 64 && list.rows[127].isHeader && list.rows[126].logSlot == 63,
          "ListBuild: 64 quests in 64 zones fill the 128 rows");
    Check(QuestIndex(list, 1063) == 127 && QuestIndex(list, 64) == 0, "QuestIndex: row 126 is index 127; headers skipped");
    Check(SortIndex(list, 41) == 40 && SortIndex(list, 999) == 0 && FindSort(list, 999, 64) == 64,
          "SortIndex: found past 25; absent is 0");

    Check(Collapsed(3, 0) && !Collapsed(3, 8) && !Collapsed(30, 0) && !Collapsed(24 + 8, 0),
          "Collapsed: only the first 25 sort types have a bit");

    int32_t old[64];
    list = {};
    for (uint32_t i = 0; i < 30; ++i)
        Append(list, static_cast<int32_t>(500 + i), i, static_cast<int32_t>(i + 1));
    SaveSortTypes(list, ~(1 << 2), old);   // type 3 (index 2) collapsed
    Check(old[2] == 3 && old[0] == -1 && old[27] == -28, "SaveSortTypes: collapsed kept, expanded and past-25 negated");
    // The rebuilt list has the types in another order: type 3 moves to index 0.
    List next = {};
    Append(next, 1, 0, 3);
    Append(next, 2, 1, 1);
    Append(next, 3, 2, 99);
    const int32_t merged = MergeFilter(next, 0, old, 30);
    Check(!(merged & 1) && (merged & 2) && (merged & 4), "MergeFilter: collapse follows the type; new types expand");
    List wide = {};
    for (uint32_t i = 0; i < 40; ++i)
        Append(wide, static_cast<int32_t>(i + 1), i, static_cast<int32_t>(i + 1));
    Check(MergeFilter(wide, 0, old, 0) == (1 << 25) - 1, "MergeFilter: no bit past the 25th type");

    auto names = [](char const* a, char const* b) { return std::strcmp(a, b); };
    SortKey questA = {false, true, 1, 10, "a"}, questB = {false, true, 1, 12, "b"}, headerKey = {true, true, 1, 0, nullptr};
    SortKey folded = {false, true, 0, 1, "z"}, wideRow = {false, true, 40, 1, "w"}, unknown = {false, false, 0, 0, nullptr};
    const int32_t filter = ~1;   // sort index 0 collapsed
    Check(Compare(headerKey, questA, filter, names) < 0 && Compare(questA, headerKey, filter, names) > 0, "Compare: header first");
    Check(Compare(questA, questB, filter, names) < 0 && Compare(questB, questA, filter, names) > 0, "Compare: by level");
    Check(Compare(folded, questA, filter, names) > 0 && Compare(questA, folded, filter, names) < 0, "Compare: folded last");
    Check(Compare(questA, wideRow, filter, names) < 0 && Compare(wideRow, questA, ~0, names) > 0,
          "Compare: sort index past 25 orders and never folds");
    Check(Compare(unknown, questA, filter, names) == 0 && Compare(questA, unknown, filter, names) == 0,
          "Compare: an uncached quest compares equal");
    SortKey sameLevel = {false, true, 1, 10, "c"};
    Check(Compare(questA, sameLevel, filter, names) < 0, "Compare: same level by name");

    Row native[50];
    int32_t nativeSort[25];
    uint32_t nativeCount = 0, nativeNumSort = 0;
    memset(native, 0xCC, sizeof(native));
    List full = {};
    for (uint32_t slot = 0; slot < 64; ++slot)
        Append(full, static_cast<int32_t>(1000 + slot), slot, static_cast<int32_t>(slot + 1));
    Mirror(full, native, nativeCount, nativeSort, nativeNumSort);
    Check(nativeCount == 50 && nativeNumSort == 25 && native[49].questId == full.rows[49].questId
          && nativeSort[24] == 25, "Mirror: the first 50 rows and 25 sort types");
    List small = {};
    Append(small, 7, 0, 1);
    Mirror(small, native, nativeCount, nativeSort, nativeNumSort);
    Check(nativeCount == 2 && native[2].questId == 0 && native[49].isHeader == 0 && nativeSort[1] == 0,
          "Mirror: the rest zeroed");

    Check(Relocated(0xC23AD0, 0x10000000, 0x20000000) == 0x20000000 && Relocated(0xC237B0, 0x10000000, 0) == 0x10000000
          && Relocated(0xC237BC, 0x10000000, 0) == 0x1000000C, "Relocated: count and row fields");

    // GetQuestLeaderBoardByID's site: slot edi, dest edi, player [ebp-4], 23 bytes at 0x5E1553, done 0x5E156C.
    uint8_t site[23];
    uint32_t used = SlotSiteCode(site, sizeof(site), 0x5E1553, 0x5E156C, kEdi, kEdi, -4, 0x10001000);
    uint32_t rel;
    memcpy(&rel, site + 5, 4);
    Check(used == 16 && site[0] == 0x57 && site[1] == 0xFF && site[2] == 0x75 && site[3] == 0xFC && site[4] == 0xE8
          && 0x5E1553 + 9 + rel == 0x10001000 && site[9] == 0x83 && site[10] == 0xC4 && site[11] == 0x08
          && site[12] == 0x8B && site[13] == 0xF8 && site[14] == 0xEB && 0x5E1553 + 16 + site[15] == 0x5E156C
          && site[16] == 0x90 && site[22] == 0x90, "SlotSiteCode: push edi / push [ebp-4] / call / add / mov edi,eax / jmp");
    // GetQuestLeaderBoardForItem's: slot eax, dest eax, player [ebp+10h] -- no mov.
    used = SlotSiteCode(site, sizeof(site), 0x5E26DD, 0x5E26F6, kEax, kEax, 0x10, 0x10001000);
    Check(used == 14 && site[0] == 0x50 && site[3] == 0x10 && site[12] == 0xEB && 0x5E26DD + 14 + site[13] == 0x5E26F6,
          "SlotSiteCode: dest eax needs no mov");
    used = SlotSiteCode(site, sizeof(site), 0x5E241D, 0x5E2436, kEax, kEcx, 0x10, 0x10001000);
    Check(used == 16 && site[12] == 0x8B && site[13] == 0xC8, "SlotSiteCode: mov ecx, eax");

    // Watches: the watch array (0xC23680, 25 x 12 bytes) moves to the DLL; its bounds go 25 -> 64.
    Check(sizeof(Watch) == 12, "Watch: 12 bytes, as m_questWatches");
    Check(RelocatedWatch(0xC23680, 0x10000000) == 0x10000000 && RelocatedWatch(0xC23684, 0x10000000) == 0x10000004
          && RelocatedWatch(0xC23688, 0x10000000) == 0x10000008, "RelocatedWatch: base and fields");
    Check(RelocatedWatch(0xC23674, 0x10000000) == 0x10000000 - 12 && RelocatedWatch(0xC23678, 0x10000000) == 0x10000000 - 8,
          "RelocatedWatch: RemoveQuestWatch's [count-1] displacements");
    Check(RowBound(0x32F883) == 0x80F880 && RowBound(0x32F983) == 0x80F980, "RowBound: cmp eax/ecx, 32h -> cmp al/cl, 80h");
    Check(RowBound(0x32FE83) == 0 && RowBound(0x19F883) == 0 && RowBound(0x32C083) == 0,
          "RowBound: refuses esi (no low byte), another immediate, not a cmp");
    Check(WatchBound(0x19F883) == 0x40F883 && WatchBound(0x19FA83) == 0x40FA83, "WatchBound: cmp eax/edx, 19h -> 40h");
    Check(WatchBound(0x32F883) == 0 && WatchBound(0x19C083) == 0, "WatchBound: refuses another immediate, not a cmp");
    Check(TimerLeft(100, 50, 140) == 9 && TimerLeft(0, 50, 51) == -2 && TimerLeft(10, 0x7FFFFFF0, 0) > 0,
          "TimerLeft: offset + time - now - 1");

    // POIs: m_questPOIs (0xC23B00, 25 x 0x30 bytes) moves to the DLL (64); UpdatePOIIcons' layout over 64.
    Check(sizeof(Poi) == 0x30 && offsetof(Poi, blobCount) == 8 && offsetof(Poi, blobData) == 0x0C
          && offsetof(Poi, iconX) == 0x20 && offsetof(Poi, visible) == 0x28 && offsetof(Poi, objectiveMask) == 0x2C,
          "Poi: 0x30 bytes, as m_questPOIs");
    Check(RelocatedPoi(0xC23B00, 0x10000000) == 0x10000000 && RelocatedPoi(0xC23B2C, 0x10000000) == 0x1000002C
          && RelocatedPoi(0xC23B04, 0x10000000) == 0x10000004, "RelocatedPoi: base and fields");
    Check(PoiImm(0x4B0) == 0xC00 && PoiImm(0x19) == 0x40 && PoiImm(0x18) == 0 && PoiImm(0x32) == 0,
          "PoiImm: 25 x 0x30 -> 64 x 0x30, 25 -> 64, nothing else");

    static Poi pois[64];
    int32_t active[64];
    uint8_t movable[64] = {};
    // 30 POIs in slots 30..59 on one spot, all movable but slot 45: they spread, 45 stays put.
    memset(pois, 0, sizeof(pois));
    for (int32_t i = 0; i < 30; ++i)
    {
        active[i] = 30 + i;
        pois[30 + i].iconX = 1000;
        pois[30 + i].iconY = 1000;
        movable[30 + i] = i != 15;
    }
    SpreadIcons(pois, active, 30, movable, 50.0f, 2500.0f, 2.5f, 20);
    bool moved = true, inRange = true;
    for (int32_t i = 30; i < 60; ++i)
    {
        if (i != 45)
            moved &= pois[i].iconX != 1000 || pois[i].iconY != 1000;
        inRange &= pois[i].iconX > 900 && pois[i].iconX < 1100 && pois[i].iconY > 900 && pois[i].iconY < 1100;
    }
    Check(moved && inRange && pois[45].iconX == 1000 && pois[45].iconY == 1000,
          "SpreadIcons: 30 stacked icons in slots 30..59 spread, a fixed one stays");
    // Two far apart: no force, nothing moves.
    memset(pois, 0, sizeof(pois));
    active[0] = 26;
    active[1] = 63;
    movable[26] = movable[63] = 1;
    pois[26].iconX = 0;
    pois[63].iconX = 5000;
    SpreadIcons(pois, active, 2, movable, 50.0f, 2500.0f, 2.5f, 20);
    Check(pois[26].iconX == 0 && pois[63].iconX == 5000 && pois[63].iconY == 0, "SpreadIcons: icons out of range stay");

    // Completed quests: the second of two on one spot is shifted by (50, 86) * shift; a lone one stays.
    memset(pois, 0, sizeof(pois));
    int32_t complete[3] = {26, 40, 63};
    pois[26].iconX = pois[40].iconX = 100;
    pois[63].iconX = 9000;
    ShiftComplete(pois, complete, 3, 0.5f);
    Check(pois[26].iconX == 100 && pois[26].iconY == 0 && pois[40].iconX == 125 && pois[40].iconY == 43
          && pois[63].iconX == 9000 && pois[63].iconY == 0, "ShiftComplete: stacked completed icons fan out past slot 25");

    // World checks and the refresh decisions.
    Check(SlotBytesBound(0x21C) == 0x528 && SlotBytesBound(0x200) == 0, "SlotBytesBound: 0x28 + 25 * 0x14 -> 0x28 + 64 * 0x14");
    // CheckForQuestItemLoot's site: slot eax, dest esi, player [ebp-0Ch], 20 bytes at 0x6D7E20, done = its end.
    used = SlotSiteCode(site, 20, 0x6D7E20, 0x6D7E34, kEax, kEsi, -0x0C, 0x10001000);
    memcpy(&rel, site + 5, 4);
    Check(used == 16 && site[0] == 0x50 && site[3] == 0xF4 && 0x6D7E20 + 9 + rel == 0x10001000 && site[12] == 0x8B
          && site[13] == 0xF0 && site[14] == 0xEB && 0x6D7E20 + 16 + site[15] == 0x6D7E34 && site[19] == 0x90,
          "SlotSiteCode: CheckForQuestItemLoot mov esi, eax and jmp to the block end");
    uint8_t failed[60];
    used = FailedSiteCode(failed, sizeof(failed), 0x6D807A, 0x6D80B9, 0x10002000);
    memcpy(&rel, failed + 5, 4);
    Check(used == 16 && failed[0] == 0x8B && failed[1] == 0x7D && failed[2] == 0xFC && failed[3] == 0x57 && failed[4] == 0xE8
          && 0x6D807A + 9 + rel == 0x10002000 && failed[9] == 0x83 && failed[10] == 0xC4 && failed[11] == 0x04
          && failed[12] == 0x8B && failed[13] == 0xD8 && failed[14] == 0xEB && 0x6D807A + 16 + failed[15] == 0x6D80B9
          && failed[16] == 0x90 && failed[59] == 0x90,
          "FailedSiteCode: mov edi, [ebp-4] / push edi / call / add esp, 4 / mov ebx, eax / jmp");
    {
        using AscQuestLog64::DecideSlot;
        uint32_t log[64] = {};
        log[30] = 600;
        AscQuestLog64::SlotDecision d = DecideSlot(log, 30, 0, 0, 0);
        bool ok = d.accepted && !d.removePoi;
        d = DecideSlot(log, 30, 0, 599, 0);
        ok &= d.accepted && d.removePoi;
        d = DecideSlot(log, 30, 0, 600, 2);
        ok &= d.accepted && !d.removePoi;
        d = DecideSlot(log, 30, 1, 600, 0);
        ok &= !d.accepted && !d.removePoi;
        d = DecideSlot(log, 30, 2, 600, 2);
        ok &= !d.accepted;
        log[30] = 0;
        d = DecideSlot(log, 30, 0, 600, 0);
        ok &= !d.accepted && d.removePoi;
        Check(ok, "DecideSlot: a new quest or a failure lifted is accepted; a removal drops the POI");

        // Promotion from slot 40 into native slot 3: the descriptor first, then 0x8F7 empties slot 40.
        uint32_t promo[64] = {};
        promo[3] = promo[40] = 500;
        d = DecideSlot(promo, 3, 0, 0, 0);
        ok = !d.accepted && !d.removePoi;
        promo[40] = 0;
        d = DecideSlot(promo, 40, 0, 500, 0);
        ok &= !d.accepted && !d.removePoi;
        Check(ok, "DecideSlot: a quest still in another slot moved; no accept, no POI removal");

        // A timed quest 800 pinned to slot 3: its quest 500 moves to slot 30 first, then slot 3 gets 800.
        uint32_t pin[64] = {};
        pin[3] = pin[30] = 500;
        d = DecideSlot(pin, 30, 0, 0, 0);
        ok = !d.accepted && !d.removePoi;
        pin[3] = 800;
        d = DecideSlot(pin, 3, 0, 500, 0);
        ok &= d.accepted && !d.removePoi;
        Check(ok, "DecideSlot: a pinned timed quest is accepted, the quest it displaced is not");
    }

    // The refresh reloads the saved watches on the login snapshot: the first packet after Clear.
    Setup();
    static std::vector<bool> firsts;
    AscQuestLog64::OnChanged([] { firsts.push_back(AscQuestLog64::FirstSinceClear()); });
    Bytes p1;
    p1.U8(1).U8(1).U8(64).U8(1);
    Add(p1, 30, 100);
    Bytes p2;
    p2.U8(1).U8(0).U8(64).U8(1);
    Add(p2, 31, 101);
    Bytes bad;
    bad.U8(2).U8(1).U8(64).U8(1);
    Add(bad, 30, 100);
    Feed(p1);
    bool outside = !AscQuestLog64::FirstSinceClear();
    Feed(p2);
    Feed(p1);
    AscQuestLog64::Clear();
    Feed(bad);
    Feed(p2);
    Check(outside && firsts == std::vector<bool>({true, false, false, true}),
          "FirstSinceClear: the first applied packet after Clear only, false outside a callback");
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description='Compile and exercise the quest log 64 store with client doubles.')
    parser.parse_args()
    cpp_source = (ROOT / 'src/Ascension/AscQuestLog64.cpp').read_text(encoding='utf-8')
    header = (ROOT / 'src/Ascension/AscQuestLog64.hpp').read_text(encoding='utf-8')
    harness = HARNESS.replace('@HEADER@', function(header, 'namespace AscQuestLog64'))
    harness = harness.replace('@STORE@', function(cpp_source, 'namespace AscQuestLog64'))
    harness = harness.replace('@HELLO@', function(cpp_source, 'void SendHello()'))
    list_source = (ROOT / 'src/Ascension/AscQuestLog64List.cpp').read_text(encoding='utf-8')
    harness = harness.replace('@LIST@', function(list_source, 'namespace AscQuestLog64List'))
    static_failures = check_client_calls(list_source)
    compiler = shutil.which(os.environ.get('CXX', 'cl.exe' if os.name == 'nt' else 'c++'))
    if not compiler:
        raise SystemExit('A C++20 compiler is required.')
    with tempfile.TemporaryDirectory(prefix='quest-log-64-') as directory:
        out = Path(directory)
        cpp = out / 'quest_log.cpp'
        cpp.write_text(harness, encoding='utf-8')
        executable = out / ('quest_log.exe' if os.name == 'nt' else 'quest_log')
        if Path(compiler).stem.lower() == 'cl':
            flags = ['/nologo', '/std:c++20', '/EHsc', str(cpp), '/Fe' + str(executable)]
        else:
            flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                     str(cpp), '-o', str(executable)]
        subprocess.run([compiler, *flags], cwd=out, check=True)
        return subprocess.run([str(executable)], cwd=out).returncode or (1 if static_failures else 0)


if __name__ == '__main__':
    raise SystemExit(main())
