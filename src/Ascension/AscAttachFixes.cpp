// The attach init's own client fixes -- hooks installed by FUN_10a66d40 (hook census group 10a66d40).
// Each hook object keeps the client target at +0 and the original (trampoline) at +4; the DLL's
// call-original thunks (FUN_100e0ca0, FUN_1019d9a0, FUN_10322c50, ...) only forward ecx / edx and the
// stack arguments. Here AscRuntime::Detour returns that trampoline.
//
// Batch 1 (2026-09-26):
//   0x4D5F70 -> sub_10a43730   replaced: the colour it returns is always 0xFFFFFF00
//   0x6B0F90 -> sub_10a4aae0   replaced: returns 0x57
//   0x6D23C0 -> sub_10a449b0   only while the qword at 0xC1DC10 is 0: 0x5A56F0, byte 0xC1DC0D = 0, then
//                              the original; otherwise the original is skipped
//   0x6E7B00 -> FUN_10a43820   spell cast: patches the client's `je` at 0x542D4E around the original
//   0x5AB120 -> sub_10a42770   consumes the state 0x6E7B00 leaves in DAT_10D3D824
//   0x6F9260 -> sub_10a42740   swallowed once after 0x730050 saw a type-3 record (flag DAT_10BCC8DA)
//   0x730050 -> sub_10a76810
//   0x7F3B60 -> sub_10a4e9f0   returns 8 instead of 6 when called from 0x57BF59
//   0x806030 -> sub_10a77190   false for spells with SpellCustomAttr +0x14 bit 0x400000
//   0x744A50 -> sub_10a43760   + FUN_10a3d840's rewrite of 0x744A55: the fade-time virtual (+0xE4) is
//                              replaced by the DLL flag DAT_10BCC8DB (0 for the one special object)
//   0x81F970 -> sub_10a46590   + SMSG 0x65C (handler_0x065c): a unit's "talk to me" marker model override
//
// Batch 2:
//   0x424B50 -> FUN_10a6b960   SFileOpen: a failed open logs the name once to C_Logger MissingFiles (6)
//   0x58A550 -> FUN_10a42e60   gossip GUID cleared: CMSG 0x741 {old gossip guid}, unless inside SMSG
//                              0x19F / 0x1B1 / 0x188 / 0x18D
//   0x58CA70 -> FUN_10a449f0   (0, x, 1): CMSG 0x741 {guid at 0xC0D648}, unless inside SMSG 0x17D
//   0x5940E0 -> FUN_10a42aa0   GUID cleared: CMSG 0x741 {guid at 0xC0E490}
//   0x584600 -> 0x10A43590     (installed through FUN_101144a0) CMSG 0x741 {guid at 0xBFA3E8} while it is set
//   0x620EE0 -> FUN_10a44c30   unit descriptor +0xD8 reads as the unit's addon field 86 during the call
//   0x70B960 -> FUN_10a42d60   on success: object +0xB0 cached from 0x7E5FD0(guid), then 0x7E6390(it)
//   0x718A00 -> FUN_10a452a0   for a unit (type 3) under the challenge rule 0x6D: 0 / 5 -> 1, 4 -> 2
//
// Batch 3:
//   0x598830 -> FUN_10a4bdc0   (a Lua-called client function) argument 3 == 16 / 17 -> byte 0xACF548 = 15 / 16
//   0x76A630 -> 0x10A76B40     (installed through FUN_101144a0) after the stock gx CVars: CVar gxMonitor
//   0x6904D0 -> FUN_10a46400   after the original: centre the foreground window on monitor gxMonitor
//   0x6B1080 -> FUN_10a4aaf0   the player's class, as seen by five LFG-role call sites: a class that stands
//                              for the roles ChrClassesRoles.dbc (and its conditions) grants
//
// Batch 4:
//   0x53D580 -> sub_10a44b50   spells 0xEF373 and 0x9788B3..0x978926 (a list) refused in a battleground
//                              or arena (Map.dbc InstanceType 3 / 4 of the current map, FUN_1008e2a0)
//   0x754D00 -> sub_10a42820   1000 when an aura on the player has an effect of type 0x100 whose misc
//                              values name the item
//   0x81B380 -> sub_10a4c700   while the world UI loads (0xBD0793, set by 0x52A980 and cleared by its call
//                              of 0x528010), an event name the client does not know is added to the custom
//                              events and the event table rebuilt (0x81B5F0 over the 722 stock names)
//   0x8A65E0 -> sub_10a760e0   three state fields (+0x17C / +0x180 / +0x184) clamped: past 0x13C -> 4, and
//                              0x6B / 0x6C -> 4 once their counters (+0x1B8 / +0x1BC / +0x1C0) pass 30
//
// Batch 5 -- the original's tamper reports (CMSG 0x51F {"Ascension", "FrameScript_RegisterFunction",
// "External call to FrameScript_RegisterFunction. Hacker/botter."}, the same text from both):
//   0x4181B0 -> sub_10a4c790   called from outside Ascension.exe (return address past 0xDFCBFF)
//   0x84E400 -> sub_10a78a30   lua_pushcclosure of a function whose first byte is 0xCC (int3)
//   Both are muted by DAT_10BDB1B0, which the original sets only while FUN_100a08e0 registers its own Lua
//   functions (through fresh E9 thunks). Our bindings never start with 0xCC and never call 0x4181B0, so
//   the flag has no setter here.
//
// Batch 6:
//   0x70CDF0 -> FUN_10a42b80   an object's name: when it contains "$CN", its first three characters are
//                              replaced by the name of the GUID at descriptor +0x18 (0x74D750), in place
//   0x71B7F0 -> sub_10a456d0   IsOutdoors called from outside Ascension.exe: CMSG 0x51F {"Ascension",
//                              "IsOutdoors", "External call to IsOutdoors. Hacker/botter."} (no mute flag)
//   0x76DDE0 -> FUN_10a76870   auction deposit, replaced (the original is never called): 0 without the
//                              auction item (GUID 0xC0F3F8) or its template; 1 for template +0x14 == 6 or a
//                              duration other than 720 / 1440 / 2880; else trunc(price * count / 100 *
//                              duration / 240 * RATE_AUCTION_DEPOSIT) when that is at least 1, else 1
//   0x743530 -> sub_10a437c0   a game object's name shows when UnitNameGO is on and it is quest-relevant
//   0x729C70 -> sub_10a45c90   a creature's name (after the original agrees): hidden while it carries aura
//                              2180198 unless it is the target; shown by UnitNameInteractiveNPC (0x729530),
//                              UnitNameQuestNPC (not friendly-reaction 0 / 1 or quest-relevant),
//                              UnitNameHostileNPC (reaction below 2), else UnitNameAllNPC
//   0x751F70 -> FUN_10a761e0   periodic-heal combat text: the client's event-name pointer 0xADB88C (read at
//                              0x7509F6) is set to "PERIODIC_HEAL", or "PERIODIC_HEAL_CRIT" when argument 7
//                              is 1, before the original
//   0x720010 -> FUN_10a44e70   the client's squared-distance constant 0xA104B0 (stock 625 = 25^2) is set
//                              before the original: for argument 0xC / 0xE / 0x33, 28900 (170^2) in a
//                              dungeon or raid (Map.dbc +8 of 0xBD088C is 1 / 2) else 10000 (100^2); else 625
//   0x6DC3F0 -> FUN_10a442f0   unless called from 0x584FBE: its own stack-size check at 0x6DC41D
//                              (mov ecx,[esi+38h] / cmp ecx,[eax+0C0h] / jg) is NOPed for the call, then the
//                              15 stock bytes are written back
//   0x805D70 -> FUN_10a77240   for the spell record passed: SpellCustomAttr +0x14 & 0x800000 turns the jbe at
//                              0x805E2D into jmp, +0x18 & 0x10 the jne at 0x805DDD into jmp +0x1E, for the
//                              call; both are written back to stock (76 / 75 61) afterwards
//   FUN_1008de00 / FUN_1008e050: an unfinished quest in the log still needs this creature / object killed
//   or used (ReqCreatureOrGOId +0x1C24 vs the slot's counts) or one of its quest items (template +0x40 /
//   +0x78 vs ReqItemId +0x1C44, counted by FUN_1008d8c0 against ReqItemCount +0x1C5C).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscChallenge.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLogger.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscProfiler.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Client/CVar.hpp>
#include <Client/FrameScript.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <cstring>
#include <string>
#include <intrin.h>
#include <windows.h>

namespace
{
    // The original writes code through NtProtectVirtualMemory, which it finds by hashing ntdll's export
    // names (FNV-1a, lower-cased, hash 0xF0ADCB54, cached in DAT_10BDB104): make writable (0x40), copy,
    // restore the old protection. VirtualProtect does the same to the page.
    void WriteCode(uint32_t at, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(at), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    }

    // ---- 0x4D5F70 / 0x6B0F90: replaced outright ------------------------------------------------------
    // sub_10a43730 (__stdcall(out), ret 4): *out = FUN_102bd200(0xFF, 0xFF, 0xFF, 0) = 0xFFFFFF00.
    void __stdcall FixedColour(uint32_t* out) { *out = 0xFFFFFF00; }
    // sub_10a4aae0
    uint32_t __cdecl Fixed57() { return 0x57; }

    // ---- 0x6D23C0 (__thiscall(this, a), ret 4) -----------------------------------------------------------
    typedef int(__fastcall* Fn6D23C0_t)(void*, void*, uint32_t);
    Fn6D23C0_t g_6D23C0 = nullptr;
    int __fastcall Detour6D23C0(void* ecx, void* edx, uint32_t a)
    {
        if (*reinterpret_cast<const uint64_t*>(0xC1DC10) != 0)
            return 0xC1DC10;   // original skipped; eax still holds the pointer the check loaded
        reinterpret_cast<void(__cdecl*)()>(0x5A56F0)();
        *reinterpret_cast<uint8_t*>(0xC1DC0D) = 0;
        return g_6D23C0(ecx, edx, a);
    }

    // ---- 0x6E7B00 (spell cast, __thiscall(this, spell, a, b, c), ret 0x10) + 0x5AB120 ------------------
    // DAT_10D3D824: -1 = skip the next 0x5AB120 in favour of 0x519280(1, 1); 1 = let it run once; 0 = idle.
    int32_t g_castState = 0;
    const uint32_t kCastJump = 0x542D4E;   // je 0x542DEB (0F 84 97 00 00 00)

    typedef int(__fastcall* Fn6E7B00_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn6E7B00_t g_6E7B00 = nullptr;
    int __fastcall Detour6E7B00(void* ecx, void* edx, uint32_t spell, uint32_t a, uint32_t b, uint32_t c)
    {
        uint8_t rec[0x2B0];
        if (AscScript::FetchSpell(spell, rec))
        {
            uint32_t f240;
            memcpy(&f240, rec + 0x240, 4);
            if (f240 == 0)
                g_castState = -1;
            const uint8_t* attr = AscCA::SpellCustomAttrRow(spell);   // FUN_103249d0: +0x10 & 0x10000
            uint32_t a10 = 0, a14 = 0;
            if (attr)
            {
                memcpy(&a10, attr + 0x10, 4);
                memcpy(&a14, attr + 0x14, 4);
            }
            if ((a10 & 0x10000) && reinterpret_cast<uint32_t(__cdecl*)(const void*)>(0x53B540)(rec) < 2)
            {
                g_castState = 1;
                static const uint8_t kJmp4F[5] = {0xE9, 0x4F, 0x00, 0x00, 0x00};
                WriteCode(kCastJump, kJmp4F, sizeof(kJmp4F));
            }
            if (a14 & 0x1000000)   // FUN_10324a10
            {
                g_castState = -1;
                static const uint8_t kJmp98[5] = {0xE9, 0x98, 0x00, 0x00, 0x00};
                WriteCode(kCastJump, kJmp98, sizeof(kJmp98));
            }
        }
        const int r = g_6E7B00(ecx, edx, spell, a, b, c);
        g_castState = 0;
        static const uint8_t kJe[6] = {0x0F, 0x84, 0x97, 0x00, 0x00, 0x00};
        WriteCode(kCastJump, kJe, sizeof(kJe));
        return r;
    }

    typedef int(__cdecl* Fn5AB120_t)(int32_t);
    Fn5AB120_t g_5AB120 = nullptr;
    int __cdecl Detour5AB120(int32_t slot)
    {
        if (g_castState == -1)
        {
            g_castState = 0;
            return reinterpret_cast<int(__cdecl*)(int, int)>(0x519280)(1, 1);
        }
        if (g_castState == 1)
            g_castState = 0;
        return g_5AB120(slot);
    }

    // ---- 0x730050 (__cdecl, 5 arguments) + 0x6F9260 (3 stack arguments, ret 0xC) -----------------------
    uint8_t g_allow6F9260 = 1;   // DAT_10BCC8DA (initialised 1)

    typedef int(__cdecl* Fn730050_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn730050_t g_730050 = nullptr;
    int __cdecl Detour730050(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
    {
        const uint8_t* rec = reinterpret_cast<const uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(a, b, 0xFFFF);
        if (*reinterpret_cast<const uint32_t*>(rec + 0x14) == 3)
            g_allow6F9260 = 0;
        return g_730050(a, b, c, d, e);
    }

    typedef int(__fastcall* Fn6F9260_t)(void*, void*, uint32_t, uint32_t, uint32_t);
    Fn6F9260_t g_6F9260 = nullptr;
    int __fastcall Detour6F9260(void* ecx, void* edx, uint32_t a, uint32_t b, uint32_t c)
    {
        if (g_allow6F9260 == 0)
        {
            g_allow6F9260 = 1;
            return 0;
        }
        return g_6F9260(ecx, edx, a, b, c);
    }

    // ---- 0x7F3B60 (returns 6) ------------------------------------------------------------------------
    typedef int(__cdecl* Fn7F3B60_t)();
    Fn7F3B60_t g_7F3B60 = nullptr;
    int __cdecl Detour7F3B60()
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x57BF59)
            return 8;
        return g_7F3B60();
    }

    // ---- 0x806030 (__cdecl(spell) -> bool) -------------------------------------------------------------
    typedef bool(__cdecl* Fn806030_t)(uint32_t);
    Fn806030_t g_806030 = nullptr;
    bool __cdecl Detour806030(uint32_t spell)
    {
        const uint8_t* attr = AscCA::SpellCustomAttrRow(spell);   // FUN_10324a10(spell, 0x400000)
        uint32_t a14 = 0;
        if (attr)
            memcpy(&a14, attr + 0x14, 4);
        if (a14 & 0x400000)
            return false;
        return g_806030(spell);
    }

    // ---- 0x744A50 (__thiscall(object, a, b, c), ret 0xC) + the rewrite at 0x744A55 ----------------------
    // FUN_10a3d840 rewrites `mov edx, [eax + 0xE4]` (8B 90 E4 00 00 00) at 0x744A55 as `mov edx, getter`
    // (C7 C2 + LAB_10a437a0), so the following `call edx` reads DAT_10BCC8DB instead of the object's
    // virtual; the result picks a 0 or 1000 fade. The detour clears the flag for a type-6 object whose
    // descriptor +0x24 is 93306 (0x16C7A) and sets it for everything else.
    uint8_t g_fadeFlag = 1;   // DAT_10BCC8DB (initialised 1)
    uint32_t __cdecl FadeFlag() { return g_fadeFlag; }   // LAB_10a437a0 (called with ecx = the object)

    typedef int(__fastcall* Fn744A50_t)(void*, void*, uint32_t, uint32_t, uint32_t);
    Fn744A50_t g_744A50 = nullptr;
    int __fastcall Detour744A50(void* ecx, void* edx, uint32_t a, uint32_t b, uint32_t c)
    {
        const uint8_t* object = static_cast<const uint8_t*>(ecx);
        g_fadeFlag = 1;
        if (*reinterpret_cast<const uint32_t*>(object + 0x14) == 6)
        {
            const uint8_t* fields = *reinterpret_cast<const uint8_t* const*>(object + 8);
            if (*reinterpret_cast<const uint32_t*>(fields + 0x24) == 0x16C7A)
                g_fadeFlag = 0;
        }
        return g_744A50(ecx, edx, a, b, c);
    }

    // ---- SMSG 0x65C + 0x81F970 (__thiscall(cache, model, b), ret 8) -----------------------------------
    // PTR_DAT_10BCCAD8: the marker model path in force while handler_0x065c rebuilds one object's marker;
    // "" (DAT_10B1B240) otherwise.
    const char* g_markerModel = "";

    typedef void*(__fastcall* Fn81F970_t)(void*, void*, void*, uint32_t);
    Fn81F970_t g_81F970 = nullptr;
    void* __fastcall Detour81F970(void* ecx, void* edx, void* model, uint32_t b)
    {
        if (g_markerModel && *g_markerModel)
            model = reinterpret_cast<void*(__thiscall*)(void*, const char*, uint32_t)>(0x81F8F0)(ecx, g_markerModel, 0);
        return g_81F970(ecx, edx, model, b);
    }

    // The path for (kind, state); nullptr leaves the current one (the original's switch has no default).
    const char* MarkerModel(uint32_t kind, int32_t state)
    {
        switch (kind)
        {
        case 1:
            switch (state)
            {
            case 1: return "";
            case 2: case 8: return "interface\\buttons\\talktome_new.m2";
            case 3: case 6: case 9: return "interface\\buttons\\talktomequestion_new_ltblue.m2";
            case 4: case 7: return "interface\\buttons\\talktomeblue_new.m2";
            case 5: return "interface\\buttons\\talktomequestionmark_new_grey.m2";
            case 10: return "interface\\buttons\\talktomequestionmark_new.m2";
            }
            break;
        case 2:
            switch (state)
            {
            case 1: return "";
            case 2: case 8: return "interface\\buttons\\talktome_journey.m2";
            case 3: case 6: case 9: return "interface\\buttons\\talktome_callingsquestion.m2";
            case 4: case 7: return "interface\\buttons\\talktome_callings.m2";
            case 5: return "interface\\buttons\\talktomequestion_journey_grey.m2";
            case 10: return "interface\\buttons\\talktomequestion_journey.m2";
            }
            break;
        case 3:
            switch (state)
            {
            case 1: return "";
            case 2: case 8: return "interface\\buttons\\talktome_important.m2";
            case 5: return "interface\\buttons\\talktomequestion_important_grey.m2";
            case 10: return "interface\\buttons\\talktomequestion_important.m2";
            }
            break;
        case 4:
            switch (state)
            {
            case 1: return "interface\\buttons\\talktomeorange_new_grey.m2";
            case 2: case 8: return "interface\\buttons\\talktomeorange_new.m2";
            case 5: return "interface\\buttons\\talktome_new_questionlegendary_grey.m2";
            case 10: return "interface\\buttons\\talktome_new_questionlegendary.m2";
            }
            break;
        case 5:
            if (state == 8)
                return "interface\\buttons\\talktome_petbattles.m2";
            break;
        case 6:
            switch (state)
            {
            case 1: return "";
            case 2: case 8: return "interface\\buttons\\talktome_asc.m2";
            case 3: case 6: return "interface\\buttons\\talktome_questionmark_daily_asc.m2";
            case 4: case 7: return "interface\\buttons\\talktome_daily_asc.m2";
            case 5: return "interface\\buttons\\talktome_questionmark_grey_asc.m2";
            case 10: return "interface\\buttons\\talktome_questionmark_asc.m2";
            }
            break;
        }
        return nullptr;
    }

    // handler_0x065c: u64 guid, u32 kind, u32 state. Picks the path, has the object rebuild its marker
    // (virtual +0x54) -- which loads through 0x81F970 -- then clears the path again.
    uint32_t ReadU32(CDataStore* p)
    {
        uint32_t v;
        memcpy(&v, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        return v;
    }
    void __cdecl OnMarkerModel(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t lo = ReadU32(p), hi = ReadU32(p), kind = ReadU32(p);
        const int32_t state = static_cast<int32_t>(ReadU32(p));
        if (const char* path = MarkerModel(kind, state))
            g_markerModel = path;
        void* object = reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(lo, hi, 0xFFFF);
        if (object)
        {
            void** vt = *static_cast<void***>(object);
            reinterpret_cast<void(__thiscall*)(void*)>(vt[0x54 / 4])(object);
        }
        g_markerModel = "";
    }

    // ---- batch 2 -------------------------------------------------------------------------------------------
    // 0x424B50 SFileOpen (__stdcall(archive, name, flags, out), ret 0x10)
    typedef int(__stdcall* SFileOpen_t)(void*, const char*, uint32_t, void**);
    SFileOpen_t g_sfileOpen = nullptr;
    int __stdcall DetourSFileOpen(void* archive, const char* name, uint32_t flags, void** out)
    {
        const int r = g_sfileOpen(archive, name, flags, out);
        if (r == 0)
            AscLogger::WriteOnce(6, name);
        return r;
    }

    // CMSG 0x741 {u64 guid}
    void SendGuidCleared(uint64_t guid) { AscScript::Packet(0x741).U64(guid).Send(); }

    // 0x58A550 (__cdecl(const u64* newGuid))
    typedef int(__cdecl* Fn58A550_t)(const uint64_t*);
    Fn58A550_t g_58A550 = nullptr;
    int __cdecl Detour58A550(const uint64_t* guid)
    {
        const uint64_t current = *reinterpret_cast<const uint64_t*>(0xC016F0);
        const int32_t op = AscProfiler::CurrentOpcode();
        if (current != 0 && *guid == 0 && op != 0x19F && op != 0x1B1 && op != 0x188 && op != 0x18D)
            SendGuidCleared(current);
        return g_58A550(guid);
    }

    // 0x58CA70 (__cdecl, 3 arguments)
    typedef int(__cdecl* Fn58CA70_t)(uint32_t, uint32_t, uint32_t);
    Fn58CA70_t g_58CA70 = nullptr;
    int __cdecl Detour58CA70(uint32_t a, uint32_t b, uint32_t c)
    {
        if (a == 0 && c == 1 && AscProfiler::CurrentOpcode() != 0x17D)
            SendGuidCleared(*reinterpret_cast<const uint64_t*>(0xC0D648));
        return g_58CA70(a, b, c);
    }

    // 0x5940E0 (__cdecl(const u64* guid, x))
    typedef int(__cdecl* Fn5940E0_t)(const uint64_t*, uint32_t);
    Fn5940E0_t g_5940E0 = nullptr;
    int __cdecl Detour5940E0(const uint64_t* guid, uint32_t b)
    {
        if (*guid == 0)
            SendGuidCleared(*reinterpret_cast<const uint64_t*>(0xC0E490));
        return g_5940E0(guid, b);
    }

    // 0x620EE0 (__thiscall(this, unit, b, c), ret 0xC)
    typedef int(__fastcall* Fn620EE0_t)(void*, void*, uint8_t*, uint32_t, uint32_t);
    Fn620EE0_t g_620EE0 = nullptr;
    int __fastcall Detour620EE0(void* ecx, void* edx, uint8_t* unit, uint32_t b, uint32_t c)
    {
        uint8_t* fields = *reinterpret_cast<uint8_t**>(unit + 8);
        uint64_t guid;
        memcpy(&guid, fields, 8);
        const uint32_t saved = *reinterpret_cast<uint32_t*>(fields + 0xD8);
        *reinterpret_cast<uint32_t*>(fields + 0xD8) = AscObjectAddon::Unit(guid)[86].value;   // record +0x408
        const int r = g_620EE0(ecx, edx, unit, b, c);
        *reinterpret_cast<uint32_t*>(fields + 0xD8) = saved;
        return r;
    }

    // 0x70B960 (__thiscall(object, a, b, c), ret 0xC)
    typedef int(__fastcall* Fn70B960_t)(void*, void*, uint32_t, uint32_t, uint32_t);
    Fn70B960_t g_70B960 = nullptr;
    int __fastcall Detour70B960(void* ecx, void* edx, uint32_t a, uint32_t b, uint32_t c)
    {
        const int r = g_70B960(ecx, edx, a, b, c);
        if (r == 0)
            return r;
        uint8_t* object = static_cast<uint8_t*>(ecx);
        uint32_t& cached = *reinterpret_cast<uint32_t*>(object + 0xB0);
        if (cached == 0)
        {
            const uint64_t guid = **reinterpret_cast<const uint64_t* const*>(object + 8);
            cached = reinterpret_cast<uint32_t(__cdecl*)(const uint64_t*)>(0x7E5FD0)(&guid);
        }
        reinterpret_cast<void(__cdecl*)(uint32_t)>(0x7E6390)(cached);
        return r;
    }

    // 0x718A00 (__thiscall(object))
    typedef int(__fastcall* Fn718A00_t)(void*, void*);
    Fn718A00_t g_718A00 = nullptr;
    int __fastcall Detour718A00(void* ecx, void* edx)
    {
        const int r = g_718A00(ecx, edx);
        if (*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(ecx) + 0x14) != 3 || !AscChallenge::ActiveHasRule(0x6D))
            return r;
        if (r == 0 || r == 5)
            return 1;
        if (r == 4)
            return 2;
        return r;
    }

    // ---- batch 3 -------------------------------------------------------------------------------------------
    // 0x598830 (__cdecl(L))
    typedef int(__cdecl* Fn598830_t)(void*);
    Fn598830_t g_598830 = nullptr;
    int __cdecl Detour598830(void* L)
    {
        if (reinterpret_cast<int(__cdecl*)(void*, int)>(0x84DBD0)(L, 3) == 3)   // lua_type == LUA_TNUMBER
        {
            const int v = static_cast<int>(reinterpret_cast<double(__cdecl*)(void*, int)>(0x84FAB0)(L, 3));
            if (v >= 0 && (v == 0x10 || v == 0x11))
            {
                const uint8_t b = static_cast<uint8_t>(v - 1);
                WriteCode(0xACF548, &b, 1);
            }
        }
        return g_598830(L);
    }

    // 0x76A630 (__cdecl) + FUN_10114510: gxMonitor, 1 = flags, default "0", category 1.
    void* g_gxMonitor = nullptr;   // DAT_10D3DC40
    typedef int(__cdecl* Fn76A630_t)();
    Fn76A630_t g_76A630 = nullptr;
    int __cdecl Detour76A630()
    {
        const int r = g_76A630();
        g_gxMonitor = reinterpret_cast<void*>(static_cast<uintptr_t>(CVar::Register("gxMonitor", nullptr, 1, "0", nullptr, 1, false, 0, false)));
        return r;
    }

    // LAB_10a4ea10: counts up from gxMonitor's value and takes the monitor at which the count is 1.
    HMONITOR g_centreMonitor = nullptr;   // DAT_10D3D6B0
    BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
    {
        int32_t* count = reinterpret_cast<int32_t*>(data);
        if (*count == 1)
        {
            g_centreMonitor = monitor;
            return FALSE;
        }
        ++*count;
        return TRUE;
    }

    // 0x6904D0 (__thiscall(this, a), ret 4)
    typedef int(__fastcall* Fn6904D0_t)(void*, void*, uint32_t);
    Fn6904D0_t g_6904D0 = nullptr;
    int __fastcall Detour6904D0(void* ecx, void* edx, uint32_t a)
    {
        const int r = g_6904D0(ecx, edx, a);
        if (!g_gxMonitor)
            return r;
        int32_t count = *reinterpret_cast<const int32_t*>(static_cast<uint8_t*>(g_gxMonitor) + 0x30);
        HWND window;
        if (count == 0 || !(window = GetForegroundWindow()))
            return r;
        g_centreMonitor = nullptr;
        EnumDisplayMonitors(nullptr, nullptr, &FindMonitor, reinterpret_cast<LPARAM>(&count));
        if (!g_centreMonitor)
            return r;
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        RECT rect;
        if (!GetMonitorInfoA(g_centreMonitor, &info) || !GetWindowRect(window, &rect))
            return r;
        const LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
        const RECT* area = &info.rcWork;
        if (info.rcWork.right - info.rcWork.left < w || info.rcWork.bottom - info.rcWork.top < h)
            area = &info.rcMonitor;
        LONG x = area->left, y = area->top;
        const LONG cx = ((area->right - x) - w) / 2 + x;
        const LONG cy = ((area->bottom - y) - h) / 2 + y;
        const LONG px = cx <= x ? cx : cx - 1;
        if (x <= px)
            x = px + w <= area->right ? px : area->right - w;
        if (y <= cy)
            y = cy + h <= area->bottom ? cy : area->bottom - h;
        SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return r;
    }

    // FUN_10a4e970: a ChrClassesRoles.dbc condition -- 0 always, 1 the spell is known (FUN_1008e5b0),
    // 2 the player has an aura of that spell; never without a player.
    bool RoleConditionMet(uint8_t* player, int32_t type, uint32_t value)
    {
        if (!player)
            return false;
        if (type == 0)
            return true;
        if (type == 1)
            return value && AscCA::SpellKnown(value, true);
        if (type == 2 && value)
        {
            typedef uint32_t(__thiscall * Count_t)(void*);
            typedef const uint8_t*(__thiscall * AuraAt_t)(void*, uint32_t);
            for (uint32_t i = 0; i < reinterpret_cast<Count_t>(0x4F8850)(player); ++i)
            {
                const uint8_t* aura = reinterpret_cast<AuraAt_t>(0x556E10)(player, i);
                if (aura && *reinterpret_cast<const uint32_t*>(aura + 8) == value)
                    return true;
            }
        }
        return false;
    }

    // 0x6B1080 (__cdecl() -> the player's class byte)
    typedef uint8_t(__cdecl* Fn6B1080_t)();
    Fn6B1080_t g_6B1080 = nullptr;
    uint8_t __cdecl Detour6B1080()
    {
        const uint8_t cls = g_6B1080();
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (from != 0x5548F3 && from != 0x553B58 && from != 0x5548F8 && from != 0x552946 && from != 0x553DBE)
            return cls;
        uint8_t* player = reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)();
        uint32_t roles = cls == 8 ? 0xD : cls == 9 ? 0xB : 0;
        if (const uint8_t* row = AscDbc::Get("DBFilesClient\\ChrClassesRoles.dbc").Row(cls))
        {
            roles |= AscDbc::Table::U32(row, 4) & 0xE;
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t extra = AscDbc::Table::U32(row, 8 + i * 4) & 0xE;
                if (extra && RoleConditionMet(player, static_cast<int32_t>(AscDbc::Table::U32(row, 0x14 + i * 4)),
                                              AscDbc::Table::U32(row, 0x20 + i * 4)))
                    roles |= extra;
            }
        }
        if ((roles & 0xE) == 0)
            return cls;
        if (roles & 2)
            return (roles & 4) ? 0xB : 1;   // tank (+ healer): druid, else warrior
        if (roles & 4)
            return 5;                       // healer: priest
        return (roles & 8) ? 4 : 0;         // damage: rogue
    }

    // ---- batch 4 -------------------------------------------------------------------------------------------
    // FUN_1008e2a0: the current map (0xBD088C) is a battleground or an arena (Map.dbc +8 == 3 or 4).
    bool InPvPInstance()
    {
        const uint8_t* map = nullptr;
        const uint32_t id = *reinterpret_cast<const uint32_t*>(0xBD088C);
        const uint8_t* const* rows = *reinterpret_cast<const uint8_t* const* const*>(0xAD4180);
        const uint32_t lo = *reinterpret_cast<const uint32_t*>(0xAD4170), hi = *reinterpret_cast<const uint32_t*>(0xAD416C);
        if (rows && id >= lo && id <= hi)
            map = rows[id - lo];
        if (!map)
            return false;
        const uint32_t type = *reinterpret_cast<const uint32_t*>(map + 8);
        return type == 4 || type == 3;
    }

    // 0x53D580 (__cdecl(const u32* spell, a, b))
    typedef int(__cdecl* Fn53D580_t)(const uint32_t*, uint32_t, uint32_t);
    Fn53D580_t g_53D580 = nullptr;
    int __cdecl Detour53D580(const uint32_t* spell, uint32_t a, uint32_t b)
    {
        bool listed = false;
        switch (*spell)
        {
        case 0xEF373: case 0x9788B3: case 0x9788B5: case 0x9788B6: case 0x9788B7: case 0x9788B8: case 0x9788B9:
        case 0x9788BA: case 0x9788BB: case 0x9788BC: case 0x9788BF: case 0x9788C0: case 0x9788C1: case 0x9788D6:
        case 0x9788D7: case 0x9788D8: case 0x978916: case 0x978925: case 0x978926:
            listed = true;
            break;
        }
        if (listed)
        {
            reinterpret_cast<void*(__cdecl*)()>(0x4038F0)();   // the original fetches the player (unused)
            if (InPvPInstance())
                return 0;
        }
        return g_53D580(spell, a, b);
    }

    // 0x754D00 (__thiscall(bag, item, b), ret 8)
    typedef int(__fastcall* Fn754D00_t)(void*, void*, int32_t, uint32_t);
    Fn754D00_t g_754D00 = nullptr;
    int __fastcall Detour754D00(void* ecx, void* edx, int32_t item, uint32_t b)
    {
        uint8_t* player = reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)();
        if (player)
        {
            typedef uint32_t(__thiscall * Count_t)(void*);
            typedef const uint8_t*(__thiscall * AuraAt_t)(void*, uint32_t);
            for (uint32_t i = 0; i < reinterpret_cast<Count_t>(0x4F8850)(player); ++i)
            {
                const uint8_t* aura = reinterpret_cast<AuraAt_t>(0x556E10)(player, i);
                uint8_t rec[0x2B0];
                if (!aura || !AscScript::FetchSpell(*reinterpret_cast<const uint32_t*>(aura + 8), rec))
                    continue;
                // The first effect of aura type 0x100 decides: any of the three misc values naming the item.
                for (uint32_t e = 0; e < 3; ++e)
                {
                    if (*reinterpret_cast<const uint32_t*>(rec + 0x17C + e * 4) != 0x100)
                        continue;
                    for (uint32_t m = 0; m < 3; ++m)
                        if (*reinterpret_cast<const int32_t*>(rec + 0x1B8 + m * 4) == item)
                            return 1000;
                    break;
                }
            }
        }
        return g_754D00(ecx, edx, item, b);
    }

    // 0x81B380 (__thiscall(frame, event name), ret 4)
    typedef int(__fastcall* Fn81B380_t)(void*, void*, const char*);
    Fn81B380_t g_81B380 = nullptr;
    int __fastcall Detour81B380(void* ecx, void* edx, const char* name)
    {
        if (*reinterpret_cast<const uint8_t*>(0xBD0793) != 0 && *reinterpret_cast<const uint32_t*>(0xB6AA2C) != 0 &&
            AscRuntime::EventId(name) == -1)
        {
            const size_t n = strlen(name) + 1;
            char* copy = static_cast<char*>(malloc(n));
            memcpy(copy, name, n);
            sDC.RegisterCustomEvent(copy);   // thunk_FUN_10278e50 (the set 0x10BE2E54)
            reinterpret_cast<void(__cdecl*)(const char**, size_t)>(0x81B5F0)(reinterpret_cast<const char**>(0xC24EB0), 0x2D2);   // the detour appends the customs
        }
        return g_81B380(ecx, edx, name);
    }

    // 0x8A65E0 (__thiscall(this, a, b), ret 8)
    typedef int(__fastcall* Fn8A65E0_t)(void*, void*, uint32_t, uint32_t);
    Fn8A65E0_t g_8A65E0 = nullptr;
    void ClampState(uint8_t* self, uint32_t state, uint32_t counter)
    {
        uint32_t& v = *reinterpret_cast<uint32_t*>(self + state);
        int32_t& c = *reinterpret_cast<int32_t*>(self + counter);
        if (v > 0x13C)
            v = 4;
        else if ((v == 0x6B || v == 0x6C) && c > 0x1E)
        {
            v = 4;
            c = 0;
        }
    }
    int __fastcall Detour8A65E0(void* ecx, void* edx, uint32_t a, uint32_t b)
    {
        const int r = g_8A65E0(ecx, edx, a, b);
        uint8_t* self = static_cast<uint8_t*>(ecx);
        ClampState(self, 0x17C, 0x1B8);
        ClampState(self, 0x180, 0x1BC);
        ClampState(self, 0x184, 0x1C0);
        return r;
    }

    // ---- batch 5: tamper reports ------------------------------------------------------------------------
    bool g_ownRegistration = false;   // DAT_10BDB1B0
    void SendTamperReport(const char* what = "FrameScript_RegisterFunction")
    {
        const std::string text = std::string("External call to ") + what + ". Hacker/botter.";
        AscScript::Packet(0x51F).Str("Ascension").Str(what).Str(text.c_str()).Send();
    }

    // 0x4181B0 (__cdecl, 2 arguments)
    typedef int(__cdecl* Fn4181B0_t)(uint32_t, uint32_t);
    Fn4181B0_t g_4181B0 = nullptr;
    int __cdecl Detour4181B0(uint32_t a, uint32_t b)
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) > 0xDFCBFF && !g_ownRegistration)
            SendTamperReport();
        return g_4181B0(a, b);
    }

    // 0x84E400 lua_pushcclosure (__cdecl(L, fn, n))
    typedef void(__cdecl* Fn84E400_t)(void*, const uint8_t*, int);
    Fn84E400_t g_84E400 = nullptr;
    void __cdecl Detour84E400(void* L, const uint8_t* fn, int n)
    {
        if (*fn == 0xCC && !g_ownRegistration)
            SendTamperReport();
        g_84E400(L, fn, n);
    }

    // ---- 0x70CDF0 (__thiscall(object) -> char*) ----------------------------------------------------------
    typedef char*(__fastcall* Fn70CDF0_t)(void*, void*);
    Fn70CDF0_t g_70CDF0 = nullptr;
    char* __fastcall Detour70CDF0(void* ecx, void* edx)
    {
        char* name = g_70CDF0(ecx, edx);
        if (!name)
            return name;
        std::string text(name);
        if (text.size() < 3 || text.find("$CN") == std::string::npos)
            return name;
        const uint8_t* fields = *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(ecx) + 8);
        uint32_t guid[2] = {*reinterpret_cast<const uint32_t*>(fields + 0x18), *reinterpret_cast<const uint32_t*>(fields + 0x1C)};
        const char* owner = reinterpret_cast<const char*(__cdecl*)(uint32_t*)>(0x74D750)(guid);
        if (!owner)
            return name;
        text.replace(0, 3, owner);   // position 0, not where "$CN" was found (as the original)
        strcpy(name, text.c_str());  // written back into the client's buffer, unbounded (as the original)
        return name;
    }

    // ---- 0x71B7F0 IsOutdoors (__thiscall(object)) ------------------------------------------------------------
    typedef int(__fastcall* Fn71B7F0_t)(void*, void*);
    Fn71B7F0_t g_71B7F0 = nullptr;
    int __fastcall Detour71B7F0(void* ecx, void* edx)
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) > 0xDFCBFF)
            SendTamperReport("IsOutdoors");
        return g_71B7F0(ecx, edx);
    }

    // ---- 0x76DDE0 (__cdecl(price, count, minutes)) -------------------------------------------------------
    int32_t __cdecl AuctionDeposit(uint32_t price, uint32_t count, uint32_t minutes)
    {
        const uint32_t* guid = reinterpret_cast<const uint32_t*>(0xC0F3F8);
        if (guid[0] == 0 && guid[1] == 0)
            return 0;
        const uint8_t* item = reinterpret_cast<const uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(guid[0], guid[1], 2);
        if (!item)
            return 0;
        const uint32_t entry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(item + 8) + 0xC);
        const uint8_t* tmpl = static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67CA30)(
            reinterpret_cast<void*>(0xC5D828), entry, nullptr, nullptr, nullptr, 0));
        if (!tmpl)
            return 0;
        if (*reinterpret_cast<const uint32_t*>(tmpl + 0x14) == 6 || (minutes != 720 && minutes != 1440 && minutes != 2880))
            return 1;
        float deposit = static_cast<float>(static_cast<double>(price)) * static_cast<float>(static_cast<double>(count)) / 100.0f *
                        static_cast<float>(static_cast<double>(minutes)) * 0.004166667f;
        const float* rate = AscConfig::Rate("RATE_AUCTION_DEPOSIT");   // FUN_10196bd0(name, 1.0)
        deposit = static_cast<float>(static_cast<long double>(rate ? *rate : 1.0f) * deposit);
        if (1.0f <= deposit)
            return static_cast<int32_t>(static_cast<int64_t>(deposit));   // _ftol2
        return 1;
    }

    // ---- quest relevance (FUN_1008de00 / FUN_1008e050) ------------------------------------------------------
    // FUN_1008d8c0: how many of `item` the player holds -- the bag / inventory slots 23..38 of the player's
    // descriptor (+0x510), then the contents of the bags in slots 19..22 (0xC23540 - 0x98 table).
    int32_t CountItem(const uint8_t* player, uint32_t item)
    {
        typedef const uint8_t*(__cdecl* Obj_t)(uint32_t, uint32_t, uint32_t);
        const Obj_t obj = reinterpret_cast<Obj_t>(0x4D4DB0);
        const uint8_t* fields = *reinterpret_cast<const uint8_t* const*>(player + 8);
        int32_t n = 0;
        for (uint32_t i = 0x17; i != 0x27; ++i)
        {
            const uint8_t* it = obj(*reinterpret_cast<const uint32_t*>(fields + 0x510 + i * 8), *reinterpret_cast<const uint32_t*>(fields + 0x514 + i * 8), 2);
            if (it && *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(it + 8) + 0xC) == item)
                n += *reinterpret_cast<const int32_t*>(*reinterpret_cast<const uint8_t* const*>(it + 8) + 0x38);
        }
        const uint8_t* bags = reinterpret_cast<const uint8_t*>(0xC23540);
        for (uint32_t i = 0x13; i != 0x17; ++i)
        {
            uint8_t* bag = const_cast<uint8_t*>(obj(*reinterpret_cast<const uint32_t*>(bags - 0x98 + i * 8), *reinterpret_cast<const uint32_t*>(bags - 0x94 + i * 8), 4));
            if (!bag)
                continue;
            void** vt = *reinterpret_cast<void***>(bag);
            void* inv = reinterpret_cast<void*(__thiscall*)(void*)>(vt[0x24 / 4])(bag);   // the slot list 0x754390 reads
            if (!inv)
                continue;
            const uint32_t slots = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(bag + 8) + 0x100);
            for (uint32_t k = 0; k < slots; ++k)
            {
                const uint8_t* it = reinterpret_cast<const uint8_t*(__thiscall*)(void*, uint32_t)>(0x754390)(inv, k);
                if (it && *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(it + 8) + 0xC) == item)
                    n += *reinterpret_cast<const int32_t*>(*reinterpret_cast<const uint8_t* const*>(it + 8) + 0x38);
            }
        }
        return n;
    }

#if !ASCENSION_QUEST_LOG_64
    // The descriptor entry (+0x278, 0x14 each) of the first of the 25 slots holding `quest`, the walk stopping at
    // the first empty slot; nullptr if none.
    const uint8_t* LoggedQuest(const uint8_t* fields, uint32_t quest)
    {
        for (uint8_t slot = 0; slot < 0x19; ++slot)
        {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(fields + 0x278 + slot * 0x14);
            if (id == 0)
                return nullptr;
            if (id == quest)
                return fields + 0x278 + slot * 0x14;
        }
        return nullptr;
    }
#endif

    // FUN_101940a0's view of the client's quest log (0xC237B0, 16-byte entries, count 0xC23AD0): {id, ?, done}.
    // ASCENSION_QUEST_LOG_64: the DLL's list (128 rows) and the 64 slots, the first empty slot ending the
    // descriptor's 25 only (AscQuestLog64::FindLoggedQuest).
    bool QuestRelevant(const uint8_t* player, uint32_t entry, void* cache, uint32_t cacheGet, uint32_t itemOffset)
    {
        typedef const uint8_t*(__thiscall* Get_t)(void*, uint32_t, void*, void*, void*, int);
        const uint8_t* tmpl = reinterpret_cast<Get_t>(cacheGet)(cache, entry, nullptr, nullptr, nullptr, 0);
        if (!tmpl)
            return false;
        const uint8_t* fields = *reinterpret_cast<const uint8_t* const*>(player + 8);
#if ASCENSION_QUEST_LOG_64
        uint32_t logCount = 0;
        const uint32_t* rows = AscQuestLog64::QuestListRows(logCount);
#else
        const uint32_t logCount = *reinterpret_cast<const uint32_t*>(0xC23AD0);
        const uint32_t* rows = reinterpret_cast<const uint32_t*>(0xC237B0);
#endif
        for (uint32_t e = 0; e < logCount; ++e)
        {
            const uint32_t* log = rows + e * 4;
            if (log[2] != 0)
                continue;
            const uint32_t quest = log[0];
            // PLAYER_QUEST_LOG_x_1 is 32 bits. The original reads its low word (1008dea0 movzx word), so
            // no quest above 65535 ever matched and custom quests drew no circle.
#if ASCENSION_QUEST_LOG_64
            const uint8_t* slotEntry = AscQuestLog64::FindLoggedQuest(fields, quest);
#else
            const uint8_t* slotEntry = LoggedQuest(fields, quest);
#endif
            if (!slotEntry)
                continue;
            const uint8_t* q = reinterpret_cast<Get_t>(0x67DE90)(reinterpret_cast<void*>(0xC5DA48), quest, nullptr, nullptr, nullptr, 0);
            if (!q)
                continue;
            const uint64_t counts = *reinterpret_cast<const uint64_t*>(slotEntry + 8);
            for (uint32_t i = 0; i < 4; ++i)
                if (*reinterpret_cast<const uint32_t*>(q + 0x1C24 + i * 4) == entry &&
                    *reinterpret_cast<const uint32_t*>(q + 0x1C34 + i * 4) != static_cast<uint16_t>(counts >> (i * 16)))
                    return true;
            for (uint32_t k = 0; k < 6; ++k)
            {
                const uint32_t item = *reinterpret_cast<const uint32_t*>(tmpl + itemOffset + k * 4);
                if (!item)
                    continue;
                for (uint32_t i = 0; i < 6; ++i)
                    if (item == *reinterpret_cast<const uint32_t*>(q + 0x1C44 + i * 4) &&
                        *reinterpret_cast<const int32_t*>(q + 0x1C5C + i * 4) != CountItem(player, item))
                        return true;
            }
        }
        return false;
    }
    const uint8_t* ActivePlayer() { return reinterpret_cast<const uint8_t*(__cdecl*)()>(0x4038F0)(); }

    // The five UnitName* CVars: output slots the world registration refills each time (0x10114090).
    int32_t CVarInt(const void* cvar)
    {
        return cvar ? *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(cvar) + 0x30) : 0;
    }
    void* g_goVar = nullptr;           // DAT_10D3DA24
    void* g_interactiveVar = nullptr;  // DAT_10D3DA2C
    void* g_questVar = nullptr;        // DAT_10D3DC30
    void* g_hostileVar = nullptr;      // DAT_10D3DC34
    void* g_allVar = nullptr;          // DAT_10D3DC38

    // 0x743530 (__thiscall(object, a), ret 4)
    typedef int(__fastcall* Fn743530_t)(void*, void*, uint32_t);
    Fn743530_t g_743530 = nullptr;
    int __fastcall Detour743530(void* ecx, void* edx, uint32_t a)
    {
        const uint8_t* object = static_cast<const uint8_t*>(ecx);
        if (*reinterpret_cast<const uint32_t*>(object + 0x14) == 5 && CVarInt(g_goVar) == 1)
        {
            const uint32_t entry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(object + 8) + 0xC);
            const uint8_t* player = ActivePlayer();
            if (player && QuestRelevant(player, entry, reinterpret_cast<void*>(0xC5D718), 0x67BD40, 0x78))
                return 1;
        }
        return g_743530(ecx, edx, a);
    }

    // 0x729C70 (__thiscall(unit, a), ret 4)
    typedef int(__fastcall* Fn729C70_t)(void*, void*, uint32_t);
    Fn729C70_t g_729C70 = nullptr;
    int __fastcall Detour729C70(void* ecx, void* edx, uint32_t a)
    {
        const int r = g_729C70(ecx, edx, a);
        if (r == 0)
            return 0;
        uint8_t* unit = static_cast<uint8_t*>(ecx);
        if (*reinterpret_cast<const uint32_t*>(unit + 0x14) != 3)
            return r;
        if (reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x7282A0)(unit, 0x214466))
        {
            const uint32_t* target = reinterpret_cast<const uint32_t*>(0xBD07B0);
            const uint32_t* guid = *reinterpret_cast<const uint32_t* const*>(unit + 8);
            if (target[0] != guid[0] || target[1] != guid[1])
                return 0;
        }
        if (CVarInt(g_interactiveVar) == 1 &&
            reinterpret_cast<char(__thiscall*)(const void*, void*)>(0x729530)(ActivePlayer(), unit))
            return 1;
        if (CVarInt(g_questVar) == 1)
        {
            const uint32_t reaction = *reinterpret_cast<const uint32_t*>(unit + 0x90);
            if (reaction != 0 && reaction != 1)
                return 1;
            const uint32_t entry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(unit + 8) + 0xC);
            const uint8_t* player = ActivePlayer();
            if (player && QuestRelevant(player, entry, reinterpret_cast<void*>(0xC5D690), 0x67B6A0, 0x40))
                return 1;
        }
        if (CVarInt(g_hostileVar) == 1 &&
            reinterpret_cast<int(__thiscall*)(const void*, void*)>(0x7251C0)(ActivePlayer(), unit) <= 1)
            return 1;
        return CVarInt(g_allVar) == 1 ? 1 : 0;
    }

    // ---- 0x751F70 (__cdecl, 9 arguments) --------------------------------------------------------------------
    typedef int(__cdecl* Fn751F70_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn751F70_t g_751F70 = nullptr;
    int __cdecl Detour751F70(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7,
                             uint32_t a8, uint32_t a9)
    {
        static const char* const kHeal = "PERIODIC_HEAL";
        static const char* const kCrit = "PERIODIC_HEAL_CRIT";
        const char* name = static_cast<uint8_t>(a7) == 1 ? kCrit : kHeal;
        WriteCode(0xADB88C, &name, 4);
        return g_751F70(a1, a2, a3, a4, a5, a6, a7, a8, a9);
    }

    // ---- 0x720010 (__thiscall(this, kind, b), ret 8) -------------------------------------------------------
    typedef int(__fastcall* Fn720010_t)(void*, void*, uint32_t, uint32_t);
    Fn720010_t g_720010 = nullptr;
    int __fastcall Detour720010(void* ecx, void* edx, uint32_t kind, uint32_t b)
    {
        float range = 625.0f;
        if (kind == 0xC || kind == 0xE || kind == 0x33)
        {
            // FUN_100b1c60 on the current map; the original reads +8 without a null check.
            const uint32_t id = *reinterpret_cast<const uint32_t*>(0xBD088C);
            const uint8_t* const* rows = *reinterpret_cast<const uint8_t* const* const*>(0xAD4180);
            const uint32_t lo = *reinterpret_cast<const uint32_t*>(0xAD4170), hi = *reinterpret_cast<const uint32_t*>(0xAD416C);
            const uint8_t* map = rows && id >= lo && id <= hi ? rows[id - lo] : nullptr;
            const uint32_t type = *reinterpret_cast<const uint32_t*>(map + 8);
            range = type == 1 || type == 2 ? 28900.0f : 10000.0f;
        }
        WriteCode(0xA104B0, &range, 4);
        return g_720010(ecx, edx, kind, b);
    }

    // ---- 0x6DC3F0 (__thiscall(this, a, b), ret 8) -----------------------------------------------------------
    typedef int(__fastcall* Fn6DC3F0_t)(void*, void*, uint32_t, uint32_t);
    Fn6DC3F0_t g_6DC3F0 = nullptr;
    int __fastcall Detour6DC3F0(void* ecx, void* edx, uint32_t a, uint32_t b)
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x584FBE)
            return g_6DC3F0(ecx, edx, a, b);
        static const uint8_t kNops[15] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        static const uint8_t kStock[15] = {0x8B, 0x4E, 0x38, 0x3B, 0x88, 0xC0, 0x00, 0x00, 0x00, 0x0F, 0x8F, 0x06, 0x01, 0x00, 0x00};
        WriteCode(0x6DC41D, kNops, sizeof(kNops));
        const int r = g_6DC3F0(ecx, edx, a, b);
        WriteCode(0x6DC41D, kStock, sizeof(kStock));
        return r;
    }

    // ---- 0x805D70 (__cdecl(const u32* spell, b)) -------------------------------------------------------------
    typedef int(__cdecl* Fn805D70_t)(const uint32_t*, uint32_t);
    Fn805D70_t g_805D70 = nullptr;
    int __cdecl Detour805D70(const uint32_t* spell, uint32_t b)
    {
        const uint8_t* attr = AscCA::SpellCustomAttrRow(*spell);
        if (attr && (*reinterpret_cast<const uint32_t*>(attr + 0x14) & 0x800000))   // FUN_10324a10
        {
            static const uint8_t kJmp = 0xEB;
            WriteCode(0x805E2D, &kJmp, 1);
        }
        if (attr && (*reinterpret_cast<const uint32_t*>(attr + 0x18) & 0x10))       // FUN_10324a50
        {
            static const uint8_t kJmp1E[2] = {0xEB, 0x1E};
            WriteCode(0x805DDD, kJmp1E, 2);
        }
        const int r = g_805D70(spell, b);
        static const uint8_t kJbe = 0x76, kJne[2] = {0x75, 0x61};
        WriteCode(0x805E2D, &kJbe, 1);
        WriteCode(0x805DDD, kJne, 2);
        return r;
    }

    // ---- 0x584600 (no arguments) -----------------------------------------------------------------------------
    typedef int(__cdecl* Fn584600_t)();
    Fn584600_t g_584600 = nullptr;
    int __cdecl Detour584600()
    {
        const uint64_t guid = *reinterpret_cast<const uint64_t*>(0xBFA3E8);
        if (guid != 0)
            SendGuidCleared(guid);
        return g_584600();
    }

    void Init()
    {
        AscClientOptions::SetOutSlot("UnitNameGO", &g_goVar);
        AscClientOptions::SetOutSlot("UnitNameInteractiveNPC", &g_interactiveVar);
        AscClientOptions::SetOutSlot("UnitNameQuestNPC", &g_questVar);
        AscClientOptions::SetOutSlot("UnitNameHostileNPC", &g_hostileVar);
        AscClientOptions::SetOutSlot("UnitNameAllNPC", &g_allVar);
        AscRuntime::ReplaceFunction(0x4D5F70, reinterpret_cast<void*>(&FixedColour));
        AscRuntime::ReplaceFunction(0x6B0F90, reinterpret_cast<void*>(&Fixed57));
        g_6D23C0 = reinterpret_cast<Fn6D23C0_t>(AscRuntime::Detour(0x6D23C0, 8, reinterpret_cast<void*>(&Detour6D23C0)));
        g_6E7B00 = reinterpret_cast<Fn6E7B00_t>(AscRuntime::Detour(0x6E7B00, 9, reinterpret_cast<void*>(&Detour6E7B00)));
        g_5AB120 = reinterpret_cast<Fn5AB120_t>(AscRuntime::Detour(0x5AB120, 9, reinterpret_cast<void*>(&Detour5AB120)));
        g_730050 = reinterpret_cast<Fn730050_t>(AscRuntime::Detour(0x730050, 6, reinterpret_cast<void*>(&Detour730050)));
        g_6F9260 = reinterpret_cast<Fn6F9260_t>(AscRuntime::Detour(0x6F9260, 5, reinterpret_cast<void*>(&Detour6F9260)));
        g_7F3B60 = reinterpret_cast<Fn7F3B60_t>(AscRuntime::Detour(0x7F3B60, 5, reinterpret_cast<void*>(&Detour7F3B60)));
        g_806030 = reinterpret_cast<Fn806030_t>(AscRuntime::Detour(0x806030, 9, reinterpret_cast<void*>(&Detour806030)));
        uint8_t movEdx[6] = {0xC7, 0xC2};
        const uint32_t getter = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&FadeFlag));
        memcpy(movEdx + 2, &getter, 4);
        WriteCode(0x744A55, movEdx, sizeof(movEdx));
        g_744A50 = reinterpret_cast<Fn744A50_t>(AscRuntime::Detour(0x744A50, 5, reinterpret_cast<void*>(&Detour744A50)));
        g_81F970 = reinterpret_cast<Fn81F970_t>(AscRuntime::Detour(0x81F970, 5, reinterpret_cast<void*>(&Detour81F970)));
        sDC.AddPacketHandler(0x65C, CNetClientCustomPacket((void*)&OnMarkerModel, nullptr));
        g_sfileOpen = reinterpret_cast<SFileOpen_t>(AscRuntime::Detour(0x424B50, 9, reinterpret_cast<void*>(&DetourSFileOpen)));
        g_58A550 = reinterpret_cast<Fn58A550_t>(AscRuntime::Detour(0x58A550, 7, reinterpret_cast<void*>(&Detour58A550)));
        g_58CA70 = reinterpret_cast<Fn58CA70_t>(AscRuntime::Detour(0x58CA70, 9, reinterpret_cast<void*>(&Detour58CA70)));
        g_5940E0 = reinterpret_cast<Fn5940E0_t>(AscRuntime::Detour(0x5940E0, 7, reinterpret_cast<void*>(&Detour5940E0)));
        g_620EE0 = reinterpret_cast<Fn620EE0_t>(AscRuntime::Detour(0x620EE0, 9, reinterpret_cast<void*>(&Detour620EE0)));
        g_70B960 = reinterpret_cast<Fn70B960_t>(AscRuntime::Detour(0x70B960, 6, reinterpret_cast<void*>(&Detour70B960)));
        g_718A00 = reinterpret_cast<Fn718A00_t>(AscRuntime::Detour(0x718A00, 6, reinterpret_cast<void*>(&Detour718A00)));
        g_598830 = reinterpret_cast<Fn598830_t>(AscRuntime::Detour(0x598830, 8, reinterpret_cast<void*>(&Detour598830)));
        g_76A630 = reinterpret_cast<Fn76A630_t>(AscRuntime::Detour(0x76A630, 9, reinterpret_cast<void*>(&Detour76A630)));
        g_6904D0 = reinterpret_cast<Fn6904D0_t>(AscRuntime::Detour(0x6904D0, 6, reinterpret_cast<void*>(&Detour6904D0)));
        g_6B1080 = reinterpret_cast<Fn6B1080_t>(AscRuntime::Detour(0x6B1080, 5, reinterpret_cast<void*>(&Detour6B1080)));
        g_53D580 = reinterpret_cast<Fn53D580_t>(AscRuntime::Detour(0x53D580, 7, reinterpret_cast<void*>(&Detour53D580)));
        g_754D00 = reinterpret_cast<Fn754D00_t>(AscRuntime::Detour(0x754D00, 6, reinterpret_cast<void*>(&Detour754D00)));
        g_81B380 = reinterpret_cast<Fn81B380_t>(AscRuntime::Detour(0x81B380, 6, reinterpret_cast<void*>(&Detour81B380)));
        g_8A65E0 = reinterpret_cast<Fn8A65E0_t>(AscRuntime::Detour(0x8A65E0, 9, reinterpret_cast<void*>(&Detour8A65E0)));
        g_4181B0 = reinterpret_cast<Fn4181B0_t>(AscRuntime::Detour(0x4181B0, 6, reinterpret_cast<void*>(&Detour4181B0)));
        g_84E400 = reinterpret_cast<Fn84E400_t>(AscRuntime::Detour(0x84E400, 5, reinterpret_cast<void*>(&Detour84E400)));
        g_70CDF0 = reinterpret_cast<Fn70CDF0_t>(AscRuntime::Detour(0x70CDF0, 6, reinterpret_cast<void*>(&Detour70CDF0)));
        g_71B7F0 = reinterpret_cast<Fn71B7F0_t>(AscRuntime::Detour(0x71B7F0, 6, reinterpret_cast<void*>(&Detour71B7F0)));
        AscRuntime::ReplaceFunction(0x76DDE0, reinterpret_cast<void*>(&AuctionDeposit));
        g_743530 = reinterpret_cast<Fn743530_t>(AscRuntime::Detour(0x743530, 10, reinterpret_cast<void*>(&Detour743530)));
        g_729C70 = reinterpret_cast<Fn729C70_t>(AscRuntime::Detour(0x729C70, 5, reinterpret_cast<void*>(&Detour729C70)));
        g_751F70 = reinterpret_cast<Fn751F70_t>(AscRuntime::Detour(0x751F70, 9, reinterpret_cast<void*>(&Detour751F70)));
        g_720010 = reinterpret_cast<Fn720010_t>(AscRuntime::Detour(0x720010, 6, reinterpret_cast<void*>(&Detour720010)));
        g_6DC3F0 = reinterpret_cast<Fn6DC3F0_t>(AscRuntime::Detour(0x6DC3F0, 7, reinterpret_cast<void*>(&Detour6DC3F0)));
        g_805D70 = reinterpret_cast<Fn805D70_t>(AscRuntime::Detour(0x805D70, 6, reinterpret_cast<void*>(&Detour805D70)));
        g_584600 = reinterpret_cast<Fn584600_t>(AscRuntime::Detour(0x584600, 5, reinterpret_cast<void*>(&Detour584600)));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}

// FUN_1008de00 for the unit-select quest circles (AscUnitSelect.cpp).
bool AscQuest_CreatureRelevant(const uint8_t* player, uint32_t entry)
{
    return QuestRelevant(player, entry, reinterpret_cast<void*>(0xC5D690), 0x67B6A0, 0x40);
}
