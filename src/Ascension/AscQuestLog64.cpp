// Quest log of 64 quests. Not in the genuine DLL; built with ASCENSION_QUEST_LOG_64 (ON by default,
// -DASCENSION_QUEST_LOG_64=OFF gives the genuine 25-slot behaviour). The CoA64 client's QuestLogExtended module,
// ported.
//
//   CMSG 0x8F6 HELLO  u8 version = 1, u8 clientMaxSlots = 64, sent after every world entry.
//   SMSG 0x8F7        u8 version = 1, u8 flags (bit 0 = full), u8 logSize, u8 count, then count x
//                     { u8 slot, u32 questId, u32 state, u16 counters[4], u32 time, u32 level, u32 rewardXP }
//                     (29 bytes each). Slots are 25..63 only; questId 0 clears a slot; every occupied slot is sent
//                     even at or above logSize.
//
// A packet with an unknown version, a slot outside 25..63 or fewer bytes than 4 + count * 29 is ignored whole. A
// full packet clears 25..63 first. logSize is clamped to [25, 64] and the slot data is applied regardless. A session
// that never gets a packet keeps the native 25 slots. This file only stores the slots; the client's quest functions
// read them through AscQuestLog64::SlotEntry.
#include <PatchConfig.hpp>

#if ASCENSION_QUEST_LOG_64
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <iterator>
#include <vector>

using namespace AscScript;

namespace AscQuestLog64
{
    namespace
    {
        constexpr uint32_t kEntryBytes = 29;
        constexpr uint8_t kVersion = 1;
        constexpr uint32_t kMirrored = kMaxSlots - kNativeSlots;

        struct Slot
        {
            bool used = false;
            Entry entry = {};
        };

        Slot s_slots[kMaxSlots];
        uint32_t s_logSize = kNativeSlots;
        std::vector<void (*)()> s_changed;
        std::vector<Change> s_changes;
        bool s_filled = false;   // a packet has arrived since the last Clear
        uint8_t s_mirror[kMirrored][0x14];

        struct Parsed
        {
            uint8_t slot;
            Entry entry;
        };

        template <class T>
        T Take(const uint8_t*& p)
        {
            T v;
            memcpy(&v, p, sizeof(T));
            p += sizeof(T);
            return v;
        }
    }

    const Entry* Get(uint32_t slot)
    {
        if (slot < kNativeSlots || slot >= kMaxSlots || !s_slots[slot].used)
            return nullptr;
        return &s_slots[slot].entry;
    }

    uint32_t LogSize() { return s_logSize; }

    void OnChanged(void (*cb)()) { s_changed.push_back(cb); }

    const std::vector<Change>& Changes() { return s_changes; }

    const uint8_t* SlotEntry(const uint8_t* descriptor, uint32_t slot)
    {
        if (slot < kNativeSlots)
            return descriptor ? descriptor + 0x278 + slot * 0x14 : nullptr;
        if (slot >= kMaxSlots)
            return nullptr;
        uint8_t* out = s_mirror[slot - kNativeSlots];
        memset(out, 0, 0x14);
        if (const Entry* e = Get(slot))
        {
            memcpy(out, &e->questId, 4);
            memcpy(out + 4, &e->state, 4);
            memcpy(out + 8, e->counters, 8);
            memcpy(out + 16, &e->time, 4);
        }
        return out;
    }

    bool Apply(const uint8_t* data, uint32_t size)
    {
        if (size < 4)
            return false;
        const uint8_t* p = data;
        const uint8_t version = Take<uint8_t>(p);
        const uint8_t flags = Take<uint8_t>(p);
        const uint8_t logSize = Take<uint8_t>(p);
        const uint8_t count = Take<uint8_t>(p);
        if (version != kVersion || size - 4 < count * kEntryBytes)
            return false;

        // Parse everything first so a bad packet applies nothing.
        std::vector<Parsed> parsed(count);
        for (Parsed& r : parsed)
        {
            r.slot = Take<uint8_t>(p);
            r.entry.questId = Take<uint32_t>(p);
            r.entry.state = Take<uint32_t>(p);
            for (uint16_t& c : r.entry.counters)
                c = Take<uint16_t>(p);
            r.entry.time = Take<uint32_t>(p);
            r.entry.level = Take<uint32_t>(p);
            r.entry.rewardXP = Take<uint32_t>(p);
            if (r.slot < kNativeSlots || r.slot >= kMaxSlots)
                return false;
        }

        Slot before[kMaxSlots];
        std::copy(std::begin(s_slots), std::end(s_slots), before);

        if (flags & 1)
            for (Slot& s : s_slots)
                s = Slot();

        for (const Parsed& r : parsed)
        {
            Slot& s = s_slots[r.slot];
            s.used = r.entry.questId != 0;
            s.entry = s.used ? r.entry : Entry();
        }

        s_changes.clear();
        if (s_filled)
            for (uint32_t i = kNativeSlots; i < kMaxSlots; ++i)
                if (memcmp(&before[i].entry, &s_slots[i].entry, sizeof(Entry)) != 0)
                    s_changes.push_back({i, before[i].entry});
        s_filled = true;

        // A bad server value must not drive the UI limit outside the supported range.
        s_logSize = logSize < kNativeSlots ? kNativeSlots : logSize > kMaxSlots ? kMaxSlots : logSize;

        for (void (*cb)() : s_changed)
            cb();
        s_changes.clear();
        return true;
    }

    void Clear()
    {
        for (Slot& s : s_slots)
            s = Slot();
        s_logSize = kNativeSlots;
        s_filled = false;
    }
}

namespace
{
    int __cdecl OnQuestLog(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t left = p->m_size > p->m_read ? p->m_size - p->m_read : 0;
        AscQuestLog64::Apply(reinterpret_cast<const uint8_t*>(p->m_buffer) + p->m_read, left);
        p->m_read = p->m_size;
        return 1;
    }

    void SendHello()
    {
        Packet packet(0x8F6);
        packet.U8(1);
        packet.U8(64);
        packet.Send();
    }

    void Init()
    {
        sDC.AddPacketHandler(0x8F7, CNetClientCustomPacket((void*)&OnQuestLog, nullptr));
        AscRuntime::OnGlueScreen(&AscQuestLog64::Clear);
        AscRuntime::OnAfterEnterWorld(&SendHello);
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
#endif
