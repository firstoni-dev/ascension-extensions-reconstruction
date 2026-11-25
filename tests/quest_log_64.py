import argparse
import os
from pathlib import Path
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


HARNESS = r'''
#include <algorithm>
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
        return subprocess.run([str(executable)], cwd=out).returncode


if __name__ == '__main__':
    raise SystemExit(main())
