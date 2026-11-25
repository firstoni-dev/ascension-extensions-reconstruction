// Quest log slots 25..63, sent by the server in SMSG 0x8F7. Not in the genuine DLL; built with
// ASCENSION_QUEST_LOG_64 (ON by default). The native log stops at 25 slots in the player descriptor; this store holds
// the extension. See AscQuestLog64.cpp.
#pragma once

#include <PatchConfig.hpp>

#include <cstdint>
#include <vector>

#if ASCENSION_QUEST_LOG_64
namespace AscQuestLog64
{
    constexpr uint32_t kNativeSlots = 25, kMaxSlots = 64;

    struct Entry
    {
        uint32_t questId, state;
        uint16_t counters[4];
        uint32_t time, level, rewardXP;
    };

    // Slot 25..63; nullptr outside that range or when the slot is empty.
    const Entry* Get(uint32_t slot);
    // The log size the server sent, clamped to [25, 64]; 25 until a packet sets it and after the glue screen.
    uint32_t LogSize();
    // Runs after every applied 0x8F7.
    void OnChanged(void (*cb)());

    struct Change
    {
        uint32_t slot;
        Entry old;   // zeroed when the slot was empty
    };
    // While OnChanged runs: the slots the packet changed, in slot order. None for the first packet after Clear.
    const std::vector<Change>& Changes();

    // The client's quest log entry for `slot` (0x14 bytes: u32 questId, u32 state, u16 counts[4], u32 time):
    // descriptor + 0x278 + slot * 0x14 for 0..24, a mirror refreshed from the store on every call (zeroed when the
    // slot is empty) for 25..63, nullptr otherwise. Each slot has its own mirror.
    const uint8_t* SlotEntry(const uint8_t* descriptor, uint32_t slot);

    // Parses and applies one SMSG 0x8F7 body; false (and nothing applied) when it is malformed.
    bool Apply(const uint8_t* data, uint32_t size);
    // The store emptied and LogSize back to kNativeSlots.
    void Clear();
}
#endif
