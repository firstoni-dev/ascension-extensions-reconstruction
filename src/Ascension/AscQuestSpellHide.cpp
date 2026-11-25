// FUN_10112620: spell 0xE4E2D is hidden -- its effects removed (0x10112650, list 0x10BE2D78) and its visual
// suppressed (0x10112640, list 0x10BE2D98) -- while quest 0x1D0BB4 is in the client's quest log
// (FUN_10112560 over FUN_101940a0: 0xC237B0, 16-byte entries, count 0xC23AD0, +0 quest id).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscRuntime.hpp>

namespace
{
    bool HiddenSpell(uint32_t spell)   // FUN_10112560
    {
        if (spell != 0xE4E2D)
            return false;
#if ASCENSION_QUEST_LOG_64
        // The DLL's list (128 rows; AscQuestLog64List.cpp), same layout.
        uint32_t count = 0;
        const uint32_t* rows = AscQuestLog64::QuestListRows(count);
#else
        const uint32_t count = *reinterpret_cast<const uint32_t*>(0xC23AD0);
        const uint32_t* rows = reinterpret_cast<const uint32_t*>(0xC237B0);
#endif
        for (uint32_t i = 0; i < count; ++i)
            if (rows[i * 4] == 0x1D0BB4)
                return true;
        return false;
    }

    bool __cdecl RemoveEffect(void*, void* effect)   // 0x10112650
    {
        return HiddenSpell(*reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(effect) + 0x18));
    }

    bool __cdecl HideVisual(uint32_t, uint32_t, uint32_t, uint32_t spell)   // 0x10112640
    {
        return HiddenSpell(spell);
    }

    void Init()
    {
        AscRuntime::OnEffectFilter(&RemoveEffect, 0x10112625);
        AscRuntime::OnVisualHide(&HideVisual, 0x1011262F);
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
