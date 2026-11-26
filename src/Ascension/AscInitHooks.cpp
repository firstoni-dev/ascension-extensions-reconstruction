// The hook set of the original's main init FUN_10a60d20 (23 FUN_100010f0 / FUN_101144a0 hooks; the code
// patches it also applies live elsewhere). Each detour is small and self-contained:
//
//   0x717910 -> sub_10a45c60   after the original, argument 0 sets the unit's +0xB7C to 0x5B
//   0x740450 -> sub_10a45bb0   after the original, the unit's selection radius is recomputed (0x720330)
//   0x720BF0 -> FUN_10a46990   before the original: for an object whose +0x20C row (DBC 0xAD4AB8) has
//                              +0x1C / +0x0C / +0x50 set and whose +0x240 is set, the immediates at
//                              0x6FD59E / 0x6FD4C7 / 0x6FD335 become 0 ...
//   0x6FDA20 -> FUN_10a45d80   ... and this one sets them back to 1 before its original
//   0x756240 -> FUN_10a452f0   the je at 0x756458 is a jmp for the duration of the call
//   0x7512B0 -> sub_10a761c0   skipped while CombatLogDisableAll is on
//   0x6DE330 -> sub_10a44900   from 0x62BAC8 on the player: the entry of the item in slot N itself
//   0x401A80 -> sub_10a4bd10   its third argument is always "0"
//   0x5E0420 / 0x61E5C0 / 0x5E0D20 -> sub_10a44ae0 / sub_10a4d770 / sub_10a790a0
//                              one of the player's 25 visible item entries (+0x278, stride 0x14) reads
//                              the player's addon field 0x24+i (0x5E0420) or 0x3D+i (the other two);
//                              with ASCENSION_QUEST_LOG_64 over 64 quest log slots, 25..63 from SMSG 0x8F7
//   0x755130 -> sub_10a45bd0   the object's unit (if any) gets 0x755E40 first
//   0x6E4950 -> sub_10a44860   CVar autoRangedCombat "1" -> its int is whether the player has a target
//                              (+0x598 / +0x59C) before the original
//   0x7441D0 -> sub_10a46630   after the original, state 5 (+0x14) calls the object's vtable +0x14 (1)
//   0x52E4D0 -> sub_10a42900   after the original, 0x6DCB40(player, 0)
//   0x6E9600 -> sub_10a465d0   after the original, 0x5186A0(0, 0)
//   0x7234D0 -> sub_10a77d60   after the original on the active player's GUID, 0x53CF10()
//   0x59D0B0 -> sub_10a427b0   false for an item that is one of the four equipped bags (0xC23540)
//   0x7FF070 -> sub_10a77160   returns 0 when called from 0x57ABE4
//   0x8D2400 -> sub_10a4c020   1 when either argument is null
//   0x744030 -> sub_10a46600   the second argument is capped: over 100000 becomes 1000
//   0x710390 -> sub_10a79110   true for the object with entry 900010 (0xDBBAA)
//   0x54D010 -> sub_10a79080   replaced: "AllianceFlag" / "HordeFlag"
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CVar.hpp>
#include <cstring>
#include <intrin.h>
#include <windows.h>

namespace AscGlobalsJ { bool CombatLogDisableAll(); }

namespace
{
    typedef uint8_t*(__cdecl* ObjectByGuid_t)(uint32_t lo, uint32_t hi, uint32_t typeMask);
    const ObjectByGuid_t ObjectByGuid = reinterpret_cast<ObjectByGuid_t>(0x4D4DB0);
    uint8_t* ActivePlayer() { return reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)(); }
    uint8_t* Descriptor(const void* object) { return *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(object) + 8); }

    void WriteByte(uint32_t at, uint8_t value)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), 1, PAGE_EXECUTE_READWRITE, &old))
            return;
        *reinterpret_cast<uint8_t*>(at) = value;
        VirtualProtect(reinterpret_cast<void*>(at), 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 1);
    }

    typedef int(__fastcall* This1_t)(void*, void*, uint32_t);
    typedef int(__fastcall* This2_t)(void*, void*, uint32_t, uint32_t);
    typedef int(__fastcall* This3_t)(void*, void*, uint32_t, uint32_t, uint32_t);
    typedef int(__fastcall* This4_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef int(__fastcall* This0_t)(void*, void*);
    typedef int(__cdecl* C0_t)();
    typedef int(__cdecl* C1_t)(uint32_t);
    typedef int(__cdecl* C2_t)(uint32_t, uint32_t);
    typedef int(__cdecl* C3_t)(uint32_t, uint32_t, uint32_t);
    typedef int(__cdecl* C4_t)(uint32_t, uint32_t, uint32_t, uint32_t);

    This1_t g_717910 = nullptr, g_740450 = nullptr, g_6DE330 = nullptr, g_755130 = nullptr, g_7441D0 = nullptr, g_6E9600 = nullptr;
    This2_t g_720BF0 = nullptr, g_744030 = nullptr;
    This3_t g_6E4950 = nullptr;
    This4_t g_756240 = nullptr;
    This0_t g_710390 = nullptr;
    C0_t g_6FDA20 = nullptr, g_52E4D0 = nullptr;
    C1_t g_7512B0 = nullptr, g_5E0D20 = nullptr;
    C2_t g_5E0420 = nullptr, g_61E5C0 = nullptr, g_59D0B0 = nullptr, g_8D2400 = nullptr;
    C3_t g_7234D0 = nullptr, g_7FF070 = nullptr;
    C4_t g_401A80 = nullptr;

    int __fastcall Detour717910(void* self, void* edx, uint32_t a)   // sub_10a45c60
    {
        const int r = g_717910(self, edx, a);
        if (a == 0)
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(self) + 0xB7C) = 0x5B;
        return r;
    }

    int __fastcall Detour740450(void* self, void* edx, uint32_t a)   // sub_10a45bb0
    {
        const int r = g_740450(self, edx, a);
        reinterpret_cast<void(__thiscall*)(void*)>(0x720330)(self);
        return r;
    }

    int __fastcall Detour720BF0(void* self, void* edx, uint32_t a, uint32_t b)   // FUN_10a46990
    {
        const uint8_t* obj = reinterpret_cast<const uint8_t*>(b);
        const uint32_t id = *reinterpret_cast<const uint32_t*>(obj + 0x20C);
        const uint8_t* row = nullptr;
        if (id && *reinterpret_cast<const uint32_t*>(0xAD4AC8))   // FUN_100b2620
        {
            const uint32_t minId = *reinterpret_cast<const uint32_t*>(0xAD4AB8), maxId = *reinterpret_cast<const uint32_t*>(0xAD4AB4);
            if (id >= minId && id <= maxId)
                row = *reinterpret_cast<uint8_t* const*>(*reinterpret_cast<const uint32_t*>(0xAD4AC8) + (id - minId) * 4);
        }
        if (row && *reinterpret_cast<const uint32_t*>(obj + 0x240) && *reinterpret_cast<const uint32_t*>(row + 0x1C) &&
            *reinterpret_cast<const uint32_t*>(row + 0xC) && *reinterpret_cast<const uint32_t*>(row + 0x50))
        {
            WriteByte(0x6FD59E, 0);
            WriteByte(0x6FD4C7, 0);
            WriteByte(0x6FD335, 0);
        }
        return g_720BF0(self, edx, a, b);
    }

    int __cdecl Detour6FDA20()   // FUN_10a45d80
    {
        WriteByte(0x6FD59E, 1);
        WriteByte(0x6FD4C7, 1);
        WriteByte(0x6FD335, 1);
        return g_6FDA20();
    }

    int __fastcall Detour756240(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d)   // FUN_10a452f0
    {
        WriteByte(0x756458, 0xEB);
        const int r = g_756240(self, edx, a, b, c, d);
        WriteByte(0x756458, 0x74);
        return r;
    }

    int __cdecl Detour7512B0(uint32_t a)   // sub_10a761c0
    {
        if (!AscGlobalsJ::CombatLogDisableAll())
            return g_7512B0(a);
        return 0;
    }

    int __fastcall Detour6DE330(void* self, void* edx, uint32_t slot)   // sub_10a44900
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x62BAC8 && self == ActivePlayer())
        {
            const uint8_t* desc = Descriptor(self);
            const uint8_t* item = ObjectByGuid(*reinterpret_cast<const uint32_t*>(desc + 0x510 + slot * 8),
                                               *reinterpret_cast<const uint32_t*>(desc + 0x514 + slot * 8), 2);
            if (item)
                return reinterpret_cast<int>(Descriptor(item) + 0xC);
        }
        return g_6DE330(self, edx, slot);
    }

    int __cdecl Detour401A80(uint32_t a, uint32_t b, uint32_t, uint32_t d)   // sub_10a4bd10
    {
        return g_401A80(a, b, reinterpret_cast<uint32_t>("0"), d);
    }

    uint32_t PlayerAddon(uint32_t index) { return AscObjectAddon::Unit(AscScript::ActivePlayerGuid())[index & 0xFFFF].value; }

#if ASCENSION_QUEST_LOG_64
    // The three below over the 64 quest log slots (CoA64 InitHooks): slots 25..63 from SMSG 0x8F7, whose entry
    // carries the scaled level and reward XP that addon fields 0x3D+i / 0x24+i hold for 0..24.
    bool QuestValue(const uint8_t* player, uint32_t entry, uint32_t addonBase, bool level, int32_t& out)
    {
        const int32_t slot = AscQuestLog64::QuestSlot(Descriptor(player), entry);
        return slot >= 0 && AscQuestLog64::SlotValue(static_cast<uint32_t>(slot), addonBase, level, &PlayerAddon, out);
    }
#else
    // The player's visible item index whose entry is `entry`, or -1 (sub_10a44ae0 and its two siblings).
    int VisibleItemIndex(const uint8_t* player, uint32_t entry)
    {
        for (uint32_t i = 0; i < 0x19; ++i)
        {
            const uint32_t e = *reinterpret_cast<const uint32_t*>(Descriptor(player) + 0x278 + i * 0x14);
            if (e != 0 && e == entry)
                return static_cast<int>(i);
        }
        return -1;
    }
    bool QuestValue(const uint8_t* player, uint32_t entry, uint32_t addonBase, bool, int32_t& out)
    {
        const int i = VisibleItemIndex(player, entry);
        if (i < 0)
            return false;
        out = static_cast<int32_t>(PlayerAddon(static_cast<uint32_t>(i) + addonBase));
        return true;
    }
#endif

    int __cdecl Detour5E0420(uint32_t entry, uint32_t b)   // sub_10a44ae0
    {
        int32_t xp;
        if (const uint8_t* player = ActivePlayer())
            if (QuestValue(player, entry, 0x24, false, xp))
                return xp;
        return g_5E0420(entry, b);
    }

    int __cdecl Detour61E5C0(uint32_t entry, uint32_t b)   // sub_10a4d770: every matching index overrides
    {
        if (const uint8_t* player = ActivePlayer())
        {
#if ASCENSION_QUEST_LOG_64
            b = static_cast<uint32_t>(AscQuestLog64::LinkLevel(Descriptor(player), entry, static_cast<int32_t>(b), &PlayerAddon));
#else
            for (uint32_t i = 0; i < 0x19; ++i)
            {
                const uint32_t e = *reinterpret_cast<const uint32_t*>(Descriptor(player) + 0x278 + i * 0x14);
                if (e != 0 && e == entry)
                    b = PlayerAddon(i + 0x3D);
            }
#endif
        }
        return g_61E5C0(entry, b);
    }

    int __cdecl Detour5E0D20(uint32_t entry)   // sub_10a790a0
    {
        int32_t level;
        if (const uint8_t* player = ActivePlayer())
            if (QuestValue(player, entry, 0x3D, true, level))
                return level;
        return g_5E0D20(entry);
    }

    int __fastcall Detour755130(void* self, void* edx, uint32_t objPtr)   // sub_10a45bd0
    {
        const uint8_t* obj = reinterpret_cast<const uint8_t*>(objPtr);
        if (uint8_t* unit = ObjectByGuid(*reinterpret_cast<const uint32_t*>(obj + 8), *reinterpret_cast<const uint32_t*>(obj + 0xC), 8))
            reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x755E40)(unit, objPtr);
        return g_755130(self, edx, objPtr);
    }

    int __fastcall Detour6E4950(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c)   // sub_10a44860
    {
        if (CVar* cvar = CVar::Lookup("autoRangedCombat"))
        {
            const uint8_t* player = ActivePlayer();
            if (strcmp(*reinterpret_cast<const char* const*>(reinterpret_cast<uint8_t*>(cvar) + 0x28), "1") == 0)
            {
                const uint8_t* desc = Descriptor(player);
                *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(cvar) + 0x30) =
                    (*reinterpret_cast<const uint32_t*>(desc + 0x598) == 0 && *reinterpret_cast<const uint32_t*>(desc + 0x59C) == 0) ? 0 : 1;
            }
        }
        return g_6E4950(self, edx, a, b, c);
    }

    int __fastcall Detour7441D0(void* self, void* edx, uint32_t f)   // sub_10a46630
    {
        const int r = g_7441D0(self, edx, f);
        if (static_cast<const uint32_t*>(self)[5] == 5)
            (*reinterpret_cast<void(__thiscall**)(void*, int)>(*static_cast<uint8_t**>(self) + 0x14))(self, 1);
        return r;
    }

    int __cdecl Detour52E4D0()   // sub_10a42900
    {
        const int r = g_52E4D0();
        if (uint8_t* player = ActivePlayer())
            reinterpret_cast<void(__thiscall*)(void*, int)>(0x6DCB40)(player, 0);
        return r;
    }

    int __fastcall Detour6E9600(void* self, void* edx, uint32_t f)   // sub_10a465d0
    {
        const int r = g_6E9600(self, edx, f);
        reinterpret_cast<void(__cdecl*)(int, int)>(0x5186A0)(0, 0);
        return r;
    }

    int __cdecl Detour7234D0(uint32_t lo, uint32_t hi, uint32_t c)   // sub_10a77d60
    {
        const int r = g_7234D0(lo, hi, c);
        if (AscScript::ActivePlayerGuid() == (static_cast<uint64_t>(hi) << 32 | lo))
            reinterpret_cast<void(__cdecl*)()>(0x53CF10)();
        return r;
    }

    int __cdecl Detour59D0B0(uint32_t itemPtr, uint32_t b)   // sub_10a427b0
    {
        const int r = g_59D0B0(itemPtr, b);
        const uint32_t* itemGuid = reinterpret_cast<const uint32_t*>(Descriptor(reinterpret_cast<const void*>(itemPtr)));
        for (uint32_t slot = 0x13; slot <= 0x16; ++slot)
        {
            const uint32_t* g = reinterpret_cast<const uint32_t*>(0xC23540 + (slot - 0x13) * 8);   // DAT_10BC91E0 - 0x98 + slot * 8
            if (const uint8_t* bag = ObjectByGuid(g[0], g[1], 4))
            {
                const uint32_t* bagGuid = reinterpret_cast<const uint32_t*>(Descriptor(bag));
                if (bagGuid[0] == itemGuid[0] && bagGuid[1] == itemGuid[1])
                    return r & ~0xFF;
            }
        }
        return r;
    }

    int __cdecl Detour7FF070(uint32_t a, uint32_t b, uint32_t c)   // sub_10a77160
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x57ABE4)
            return 0;
        return g_7FF070(a, b, c);
    }

    int __cdecl Detour8D2400(uint32_t a, uint32_t b)   // sub_10a4c020
    {
        if (a != 0 && b != 0)
            return g_8D2400(a, b);
        return 1;
    }

    int __fastcall Detour744030(void* self, void* edx, uint32_t f, uint32_t n)   // sub_10a46600
    {
        return g_744030(self, edx, f, n < 0x186A1 ? n : 1000);
    }

    int __fastcall Detour710390(void* self, void* edx)   // sub_10a79110
    {
        if (*reinterpret_cast<const uint32_t*>(Descriptor(self) + 0xC) == 0xDBBAA)
            return (reinterpret_cast<uintptr_t>(Descriptor(self)) & ~0xFFu) | 1;
        return g_710390(self, edx);
    }

    const char* __cdecl FlagModel(uint32_t alliance)   // sub_10a79080 (replaces 0x54D010)
    {
        return alliance ? "AllianceFlag" : "HordeFlag";
    }

    void Init()
    {
        g_717910 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x717910, 5, reinterpret_cast<void*>(&Detour717910)));
        g_740450 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x740450, 6, reinterpret_cast<void*>(&Detour740450)));
        g_720BF0 = reinterpret_cast<This2_t>(AscRuntime::Detour(0x720BF0, 6, reinterpret_cast<void*>(&Detour720BF0)));
        g_6FDA20 = reinterpret_cast<C0_t>(AscRuntime::Detour(0x6FDA20, 9, reinterpret_cast<void*>(&Detour6FDA20)));
        g_7512B0 = reinterpret_cast<C1_t>(AscRuntime::Detour(0x7512B0, 6, reinterpret_cast<void*>(&Detour7512B0)));
        g_6DE330 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x6DE330, 6, reinterpret_cast<void*>(&Detour6DE330)));
        g_401A80 = reinterpret_cast<C4_t>(AscRuntime::Detour(0x401A80, 6, reinterpret_cast<void*>(&Detour401A80)));
        g_5E0420 = reinterpret_cast<C2_t>(AscRuntime::Detour(0x5E0420, 6, reinterpret_cast<void*>(&Detour5E0420)));
        g_61E5C0 = reinterpret_cast<C2_t>(AscRuntime::Detour(0x61E5C0, 6, reinterpret_cast<void*>(&Detour61E5C0)));
        g_5E0D20 = reinterpret_cast<C1_t>(AscRuntime::Detour(0x5E0D20, 6, reinterpret_cast<void*>(&Detour5E0D20)));
        g_755130 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x755130, 5, reinterpret_cast<void*>(&Detour755130)));
        g_756240 = reinterpret_cast<This4_t>(AscRuntime::Detour(0x756240, 9, reinterpret_cast<void*>(&Detour756240)));
        g_6E4950 = reinterpret_cast<This3_t>(AscRuntime::Detour(0x6E4950, 6, reinterpret_cast<void*>(&Detour6E4950)));
        g_7441D0 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x7441D0, 8, reinterpret_cast<void*>(&Detour7441D0)));
        g_52E4D0 = reinterpret_cast<C0_t>(AscRuntime::Detour(0x52E4D0, 5, reinterpret_cast<void*>(&Detour52E4D0)));
        g_6E9600 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x6E9600, 6, reinterpret_cast<void*>(&Detour6E9600)));
        g_7234D0 = reinterpret_cast<C3_t>(AscRuntime::Detour(0x7234D0, 6, reinterpret_cast<void*>(&Detour7234D0)));
        g_59D0B0 = reinterpret_cast<C2_t>(AscRuntime::Detour(0x59D0B0, 6, reinterpret_cast<void*>(&Detour59D0B0)));
        g_7FF070 = reinterpret_cast<C3_t>(AscRuntime::Detour(0x7FF070, 7, reinterpret_cast<void*>(&Detour7FF070)));
        g_8D2400 = reinterpret_cast<C2_t>(AscRuntime::Detour(0x8D2400, 6, reinterpret_cast<void*>(&Detour8D2400)));
        g_744030 = reinterpret_cast<This2_t>(AscRuntime::Detour(0x744030, 7, reinterpret_cast<void*>(&Detour744030)));
        g_710390 = reinterpret_cast<This0_t>(AscRuntime::Detour(0x710390, 6, reinterpret_cast<void*>(&Detour710390)));
        AscRuntime::ReplaceFunction(0x54D010, reinterpret_cast<void*>(&FlagModel));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
