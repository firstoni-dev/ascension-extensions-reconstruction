#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscCallbackOrder.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscScript.hpp>
#include <Misc/DataContainer.hpp>
#include <Client/FrameScript.hpp>
#include <Windows.h>
#include <cstring>
#include <ctime>
#include <array>
#include <unordered_map>
#include <vector>

namespace AscRuntime
{
namespace
{
    const char* const kEventNames[] = {
#include <Ascension/AscEvents.generated.inc>
    };

    std::vector<Callback>& EnterWorldList() { static std::vector<Callback> v; return v; }
    std::vector<Callback>& GlueScreenList() { static std::vector<Callback> v; return v; }

    void __cdecl RunEnterWorld() { for (Callback cb : EnterWorldList()) cb(); }
    void __cdecl RunGlueScreen() { for (Callback cb : GlueScreenList()) cb(); }
    std::vector<Callback>& AfterEnterWorldList() { static std::vector<Callback> v; return v; }
    void __cdecl RunAfterEnterWorld() { for (Callback cb : AfterEnterWorldList()) cb(); }

    std::vector<std::pair<int, StockEventCallback>>& StockEventList() { static std::vector<std::pair<int, StockEventCallback>> v; return v; }
    std::vector<Callback>& After403340List() { static std::vector<Callback> v; return v; }

    // 0x81AA00 begins push ebp / mov ebp,esp / sub esp,0xC: six bytes.
    typedef int(__cdecl* Dispatch_t)(int, lua_State*, int);
    Dispatch_t g_dispatchTrampoline = nullptr;
    int __cdecl DispatchDetour(int eventId, lua_State* L, int nargs)
    {
        if (L)
            for (auto& e : StockEventList())
                if (e.first == eventId)
                    e.second(L);
        return g_dispatchTrampoline(eventId, L, nargs);
    }

    // 0x403340 begins push ebp / mov ebp,esp / push esi / mov esi,[ebp+8]: seven bytes.
    typedef int(__cdecl* Fn403340_t)(void*);
    Fn403340_t g_403340Trampoline = nullptr;
    int __cdecl Detour403340(void* p)
    {
        const int r = g_403340Trampoline(p);
        for (Callback cb : After403340List())
            cb();
        return r;
    }

    // 0x495810 begins push ebp / mov ebp,esp / fld dword [ebp+0x14]: six bytes.
    std::vector<Callback>& After495810List() { static std::vector<Callback> v; return v; }
    typedef int(__cdecl* Fn495810_t)(void*, void*, void*, uint32_t);   // 4th = float dt, passed through as bits
    Fn495810_t g_495810Trampoline = nullptr;
    int __cdecl Detour495810(void* a, void* b, void* c, uint32_t d)
    {
        const int r = g_495810Trampoline(a, b, c, d);
        for (Callback cb : After495810List())
            cb();
        return r;
    }

    // 0x528C30 begins push ebp / mov ebp,esp / sub esp,8: six bytes. The original's detour (0x10275CA0)
    // runs list 0x10BE28AC, then jumps into the original with ecx = its hook object, so the function
    // takes nothing in registers; ours keeps every register anyway.
    std::vector<Callback>& LeaveWorldList() { static std::vector<Callback> v; return v; }
    void __cdecl RunLeaveWorld() { for (Callback cb : LeaveWorldList()) cb(); }
    void* g_528C30Trampoline = nullptr;
    __declspec(naked) void Detour528C30()
    {
        __asm
        {
            pushad
            pushfd
            call RunLeaveWorld
            popfd
            popad
            jmp dword ptr [g_528C30Trampoline]
        }
    }

    // Hook-manager detours of the original that are deliberately not installed (census markers):

    // 0x528F00 begins push ebp / mov ebp,esp / mov ecx,[0xC5DF88]: nine bytes, absolute operand. Same
    // shape as 0x528C30: list 0x10BE28CC, then into the original.
    std::vector<Callback>& Before528F00List() { static std::vector<Callback> v; return v; }
    void __cdecl RunBefore528F00() { for (Callback cb : Before528F00List()) cb(); }
    void* g_528F00Trampoline = nullptr;
    __declspec(naked) void Detour528F00()
    {
        __asm
        {
            pushad
            pushfd
            call RunBefore528F00
            popfd
            popad
            jmp dword ptr [g_528F00Trampoline]
        }
    }

    // 0x402910 (no arguments) begins with call 0x7E21B0, which a copied prologue cannot carry, so the
    // detour makes that call itself and continues at 0x402915; then list 0x10BE2A8C.
    std::vector<Callback>& After402910List() { static std::vector<Callback> v; return v; }
    void __cdecl RunAfter402910() { for (Callback cb : After402910List()) cb(); }
    __declspec(naked) void Detour402910()
    {
        __asm
        {
            mov eax, 0x7E21B0
            call eax
            mov eax, 0x402915
            call eax
            pushad
            pushfd
            call RunAfter402910
            popfd
            popad
            ret
        }
    }

    // 0x5204C0 (__cdecl, seven arguments) begins push ebp / mov ebp,esp / mov eax,[0xBD080C]: eight
    // bytes, absolute operand. The original's detour runs it, then list 0x10BE290C with the first
    // argument (and list 0x10BE2930 with the second, which nothing we transcribe uses).
    std::vector<ObjectCallback>& After5204C0List() { static std::vector<ObjectCallback> v; return v; }
    typedef int(__cdecl* Fn5204C0_t)(void*, void*, void*, void*, void*, void*, void*);
    Fn5204C0_t g_5204C0Trampoline = nullptr;
    int __cdecl Detour5204C0(void* a, void* b, void* c, void* d, void* e, void* f, void* g)
    {
        const int r = g_5204C0Trampoline(a, b, c, d, e, f, g);
        for (ObjectCallback cb : After5204C0List())
            cb(a);
        return r;
    }

    // 0x4F6F90 (__thiscall, no stack arguments) begins push esi / mov esi,ecx / mov eax,[esi+0x380]:
    // nine bytes. The original's detour (0x10276270) runs it, then list 0x10BE2AAC.
    std::vector<Callback>& After4F6F90List() { static std::vector<Callback> v; return v; }
    typedef int(__fastcall* Fn4F6F90_t)(void*, void*);
    Fn4F6F90_t g_4F6F90Trampoline = nullptr;
    int __fastcall Detour4F6F90(void* ecx, void* edx)
    {
        const int r = g_4F6F90Trampoline(ecx, edx);
        for (Callback cb : After4F6F90List())
            cb();
        return r;
    }

    // 0x6FC0F0 (removes an object from the client's list at 0xCA0ADC) begins push ebp / mov ebp,esp /
    // mov ecx,[0xCA0ADC]: nine bytes, absolute operand. The original's detour (FUN_10278840) runs
    // list 0x10BE2BEC with the object before the original.
    std::vector<ObjectCallback>& ObjectFreeList() { static std::vector<ObjectCallback> v; return v; }
    typedef void(__cdecl* Fn6FC0F0_t)(void*);
    Fn6FC0F0_t g_6FC0F0Trampoline = nullptr;
    void __cdecl Detour6FC0F0(void* object)
    {
        for (ObjectCallback cb : ObjectFreeList())
            cb(object);
        g_6FC0F0Trampoline(object);
    }

    // 0x70F680 (__thiscall, no stack arguments: a game object's use) begins push ebp / mov ebp,esp /
    // sub esp,0x2C: six bytes. The original's detour 0x10A42DD0 runs the original, then its branches
    // with the object (mystic altar -> FUN_102EA2C0).
    std::vector<ObjectCallback>& GameObjectUseList() { static std::vector<ObjectCallback> v; return v; }
    typedef void(__thiscall* Fn70F680_t)(void*);
    Fn70F680_t g_70F680Trampoline = nullptr;
    void __fastcall Detour70F680(void* object, void*)
    {
        g_70F680Trampoline(object);
        for (ObjectCallback cb : GameObjectUseList())
            cb(object);
    }

    // 0x708C20 (__thiscall(item, a, b), ret 8: an item's use) begins push ebp / mov ebp,esp /
    // sub esp,0x2D0: nine bytes. The original's detour 0x10A42F30 runs the original, then branches on
    // the item's entry (mystic scroll -> FUN_102EA3A0, preset unlock token -> FUN_102EA330, ...).
    std::vector<ObjectCallback>& ItemUseList() { static std::vector<ObjectCallback> v; return v; }
    typedef int(__thiscall* Fn708C20_t)(void*, int, int);
    Fn708C20_t g_708C20Trampoline = nullptr;
    int __fastcall Detour708C20(void* item, void*, int a, int b)
    {
        const int r = g_708C20Trampoline(item, a, b);
        for (ObjectCallback cb : ItemUseList())
            cb(item);
        return r;
    }

    // 0x743EC0 (__thiscall(object, a), ret 4) begins push ebp / mov ebp,esp / push esi / mov esi,ecx:
    // six bytes. The original's detour 0x10275E50 runs the original, then list 0x10BE2CF8 with the object.
    std::vector<ObjectCallback>& After743EC0List() { static std::vector<ObjectCallback> v; return v; }
    typedef int(__thiscall* Fn743EC0_t)(void*, int);
    Fn743EC0_t g_743EC0Trampoline = nullptr;
    int __fastcall Detour743EC0(void* object, void*, int a)
    {
        const int r = g_743EC0Trampoline(object, a);
        for (ObjectCallback cb : After743EC0List())
            cb(object);
        return r;
    }

    // 0x524BF0 (__cdecl(guid lo, hi), the target change; push ebp / mov ebp,esp / sub esp,0xC... 9 bytes).
    // The original's detour 0x10275D70 (hook object 0x10BCB574), skipped entirely while the byte at
    // 0xBD0791 is set: list 0x10BE2950 (FUN_10278390), the original, then -- for a unit target that is
    // not flagged 0x10 and whose +0x10C / +0x110 differ -- its 0x616B10 record, looked up in 0xC5CE30
    // (0x6F6020) gets +0x18 = 1 and 0x512B50(&guid, 3) runs; last, list 0x10BE2970 (FUN_10278330).
    std::vector<Callback>& BeforeTargetList() { static std::vector<Callback> v; return v; }
    std::vector<Callback>& AfterTargetList() { static std::vector<Callback> v; return v; }
    typedef void(__cdecl* Fn524BF0_t)(uint32_t, uint32_t);
    Fn524BF0_t g_524BF0Trampoline = nullptr;
    void __cdecl Detour524BF0(uint32_t lo, uint32_t hi)
    {
        if (*reinterpret_cast<const uint8_t*>(0xBD0791) != 0)
            return;
        for (Callback cb : BeforeTargetList())
            cb();
        g_524BF0Trampoline(lo, hi);
        if (lo | hi)
        {
            uint8_t* unit = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(lo, hi, 8);
            if (unit)
            {
                const uint8_t* desc = *reinterpret_cast<uint8_t* const*>(unit + 8);
                if (!(desc[8] & 0x10) && *reinterpret_cast<const uint32_t*>(desc + 0x10C) != *reinterpret_cast<const uint32_t*>(desc + 0x110))
                    if (void* rec = reinterpret_cast<void*(__thiscall*)(void*)>(0x616B10)(unit))
                        if (uint8_t* e = reinterpret_cast<uint8_t*(__thiscall*)(void*, void*, int)>(0x6F6020)(reinterpret_cast<void*>(0xC5CE30), rec, 0))
                        {
                            *reinterpret_cast<uint32_t*>(e + 0x18) = 1;
                            uint32_t guid[2] = {*reinterpret_cast<const uint32_t*>(desc), *reinterpret_cast<const uint32_t*>(desc + 4)};
                            if (guid[0] | guid[1])
                                reinterpret_cast<void(__cdecl*)(uint32_t*, int)>(0x512B50)(guid, 3);
                        }
            }
        }
        for (Callback cb : AfterTargetList())
            cb();
    }

    // 0x6ECF80 (__thiscall, 7 stack arguments, ret 0x1C; 6 bytes). The original's detour 0x102762B0 (hook
    // object 0x10BCB650) runs list 0x10BE2BAC (FUN_10278660) with the object's +0x144, then the original.
    std::vector<void (*)(uint32_t)>& Before6ECF80List() { static std::vector<void (*)(uint32_t)> v; return v; }
    typedef int(__fastcall* Fn6ECF80_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn6ECF80_t g_6ECF80Trampoline = nullptr;
    int __fastcall Detour6ECF80(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
    {
        for (auto cb : Before6ECF80List())
            cb(*reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(self) + 0x144));
        return g_6ECF80Trampoline(self, edx, a, b, c, d, e, f, g);
    }

    // 0x724820 (9 bytes) / 0x71E930 (6 bytes): __thiscall(unit, a, b), ret 8, edx passed through. The
    // original's detours 0x10275F60 / 0x10276090 (hook objects 0x10BCB62C / 0x10BCB644) run their list with
    // (unit, a, b); the first false return is returned without calling the original.
    template <class T> void InsertBySite(std::vector<std::pair<uint32_t, T>>& v, T cb, uint32_t site)
    {
        auto at = v.begin();
        while (at != v.end() && at->first <= site)
            ++at;
        v.insert(at, {site, cb});
    }
    std::vector<std::pair<uint32_t, UnitSpellVeto>>& Before724820List() { static std::vector<std::pair<uint32_t, UnitSpellVeto>> v; return v; }
    std::vector<std::pair<uint32_t, UnitSpellVeto>>& Before71E930List() { static std::vector<std::pair<uint32_t, UnitSpellVeto>> v; return v; }
    typedef int(__fastcall* FnUnitAB_t)(void*, void*, uint32_t, uint32_t);
    FnUnitAB_t g_724820Trampoline = nullptr, g_71E930Trampoline = nullptr;
    int __fastcall Detour724820(void* unit, void* edx, uint32_t a, uint32_t b)
    {
        for (auto& cb : Before724820List())
            if (!cb.second(unit, a, b))
                return 0;
        return g_724820Trampoline(unit, edx, a, b);
    }
    int __fastcall Detour71E930(void* unit, void* edx, uint32_t a, uint32_t b)
    {
        for (auto& cb : Before71E930List())
            if (!cb.second(unit, a, b))
                return 0;
        return g_71E930Trampoline(unit, edx, a, b);
    }

    // 0x80B5D0 (__cdecl, 5 arguments, bool; 6 bytes). The original's detour 0x10277900 (hook object
    // 0x10BCB680): the original, and when it returned true, list 0x10BE2DD8 until a callback says false.
    std::vector<std::pair<uint32_t, CastVeto>>& After80B5D0List() { static std::vector<std::pair<uint32_t, CastVeto>> v; return v; }
    typedef int(__cdecl* Fn80B5D0_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn80B5D0_t g_80B5D0Trampoline = nullptr;
    int __cdecl Detour80B5D0(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)
    {
        if (!(g_80B5D0Trampoline(a, b, c, d, e) & 0xFF))
            return 0;
        for (auto& cb : After80B5D0List())
            if (!cb.second(a, b, c, d, e))
                return 0;
        return 1;
    }

    std::vector<std::pair<uint32_t, EffectFilter>>& EffectFilterList() { static std::vector<std::pair<uint32_t, EffectFilter>> v; return v; }
    std::vector<std::pair<uint32_t, VisualHide>>& VisualHideList() { static std::vector<std::pair<uint32_t, VisualHide>> v; return v; }

    // Every effect of the unit's chain, the next one read before the callbacks run.
    void FilterEffects(void* unit)
    {
        uint8_t* effect = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(unit) + 0xA8);
        while (effect)
        {
            uint8_t* next = *reinterpret_cast<uint8_t**>(effect + 0x108);
            for (auto& cb : EffectFilterList())
                if (cb.second(unit, effect))
                {
                    reinterpret_cast<void(__thiscall*)(void*)>(0x6F87C0)(effect);
                    break;
                }
            effect = next;
        }
    }

    // 0x10276310, replacing 0x744AC0 (__thiscall(unit, all), ret 4). With `all` every effect is torn down
    // (0x6F74B0, then 0x6F7A00 for an unattached one with +0xCC, else 0x6FA390); without it only what list
    // 0x10BE2D78 votes out goes (the stock function removed every effect lacking attribute 0x100000).
    void __fastcall Replace744AC0(void* unit, void*, uint32_t all)
    {
        if (!all)
        {
            FilterEffects(unit);
            return;
        }
        while (uint8_t* effect = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(unit) + 0xA8))
        {
            reinterpret_cast<void(__thiscall*)(void*)>(0x6F74B0)(effect);
            if (*reinterpret_cast<int32_t*>(effect + 0x24) == -1 && *reinterpret_cast<uint32_t*>(effect + 0xCC) != 0)
                reinterpret_cast<void(__thiscall*)(void*)>(0x6F7A00)(effect);
            else
                reinterpret_cast<void(__thiscall*)(void*)>(0x6FA390)(effect);
        }
    }

    // 0x1019C4C0: 0x4D4B30's per-object callback -- the unit with this GUID (type mask 8), if any, is kept.
    int __cdecl CollectUnit(uint32_t lo, uint32_t hi, void* param)
    {
        if (void* unit = reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(lo, hi, 8))
            static_cast<std::vector<void*>*>(param)->push_back(unit);
        return 1;
    }

    // 0x10277970 (list 0x10BE29CC, after 0x403340): unless _time64 equals DAT_10BE2E40 -- which nothing
    // writes, so it always runs -- every unit (0x4D4B30 with collector 0x1019C4C0, type mask 8) is filtered.
    void Sweep()
    {
        static const int64_t lastRun = 0;   // DAT_10BE2E40, never written
        if (_time64(nullptr) == lastRun)
            return;
        std::vector<void*> units;
        reinterpret_cast<void(__cdecl*)(int(__cdecl*)(uint32_t, uint32_t, void*), void*)>(0x4D4B30)(&CollectUnit, &units);
        for (void* unit : units)
            FilterEffects(unit);
    }

    // 0x52A980 (__cdecl, no arguments; 9 bytes). The original's detour 0x10275C60 (hook object 0x10BCB544)
    // reads the byte 0xBD0790 BEFORE the original, runs the original, then list 0x10BE29EC (FUN_10278600)
    // only if that byte was set.
    std::vector<Callback>& After52A980List() { static std::vector<Callback> v; return v; }
    typedef void(__cdecl* Fn52A980_t)();
    Fn52A980_t g_52A980Trampoline = nullptr;
    void __cdecl Detour52A980()
    {
        const bool flag = *reinterpret_cast<const uint8_t*>(0xBD0790) != 0;
        g_52A980Trampoline();
        if (flag)
            for (Callback cb : After52A980List())
                cb();
    }

    // 0x6DF050 (a stock packet handler) begins push ebp / mov ebp,esp / sub esp,0x20: six bytes. The
    // original's detour (0x102776A0) runs list 0x10BE28EC only when its flag 0x10BE2E38 is set and
    // stores the flag's inverse, so the list runs on every second call.
    std::vector<Callback>& Alternate6DF050List() { static std::vector<Callback> v; return v; }
    typedef int(__cdecl* Fn6DF050_t)(void*, uint32_t, uint32_t, void*);
    Fn6DF050_t g_6DF050Trampoline = nullptr;
    bool g_6DF050Flag = false;
    PacketHook g_6DF050Hook = nullptr;
    int __cdecl Detour6DF050(void* a, uint32_t opcode, uint32_t time, void* packet)
    {
        if (g_6DF050Flag)
        {
            for (Callback cb : Alternate6DF050List())
                cb();
            g_6DF050Flag = false;
        }
        else
            g_6DF050Flag = true;
        if (g_6DF050Hook)
            return g_6DF050Hook(reinterpret_cast<PacketHandler>(g_6DF050Trampoline), a, opcode, time, packet);
        return g_6DF050Trampoline(a, opcode, time, packet);
    }

    // Other hooked packet handlers: a fixed set of slots, each with its own detour entry.
    struct HookSlot { uint32_t address = 0, prologue = 0; PacketHook hook = nullptr; PacketHandler trampoline = nullptr; };
    HookSlot g_slots[4];
    template <int N> int __cdecl SlotDetour(void* a, uint32_t opcode, uint32_t time, void* packet)
    {
        return g_slots[N].hook(g_slots[N].trampoline, a, opcode, time, packet);
    }
    PacketHandler const kSlotDetours[4] = {&SlotDetour<0>, &SlotDetour<1>, &SlotDetour<2>, &SlotDetour<3>};

    void* MakeTrampoline(uint32_t target, uint32_t prologue)
    {
        uint8_t* t = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        memcpy(t, reinterpret_cast<void*>(target), prologue);
        // A target already detoured (the original's hook manager chains several hooks on one address,
        // e.g. 0x766640): the copied jmp is relative, so re-aim it at the earlier detour.
        if (t[0] == 0xE9)
        {
            const uint32_t dest = target + 5 + *reinterpret_cast<const uint32_t*>(target + 1);
            *reinterpret_cast<uint32_t*>(t + 1) = dest - reinterpret_cast<uint32_t>(t + 5);
        }
        t[prologue] = 0xE9;
        *reinterpret_cast<uint32_t*>(t + prologue + 1) = (target + prologue) - reinterpret_cast<uint32_t>(t + prologue + 5);
        return t;
    }

    // 0x528010 begins push ebp / mov ebp,esp / sub esp,0x3C: six bytes, relocatable as-is.
    const uint32_t kEnterWorld = 0x528010;   // hooked 0x528010
    const uint32_t kEnterWorldPrologue = 6;
    void* g_enterWorldTrampoline = nullptr;

    // The original's detour (0x10275c10): run the pre list, the original with ecx/edx intact, then
    // the post list. 0x528010 takes no stack arguments and returns with a plain ret, so calling it
    // (rather than jumping) is safe.
    __declspec(naked) void EnterWorldDetour()
    {
        __asm
        {
            pushad
            pushfd
            call RunEnterWorld
            popfd
            popad
            call dword ptr [g_enterWorldTrampoline]
            pushad
            pushfd
            call RunAfterEnterWorld
            popfd
            popad
            ret
        }
    }

    // The original's cave (0x102762a0) replaces 0x4DA5F0's five-byte epilogue at 0x4DA9AC.
    // HOOKED 0x4DA9AC (written through kGlueEpilogue)
    const uint32_t kGlueEpilogue = 0x4DA9AC;
    __declspec(naked) void GlueEpilogueCave()
    {
        __asm
        {
            pop ebx
            mov esp, ebp
            pop ebp
            pushad
            pushfd
            call RunGlueScreen
            popfd
            popad
            ret
        }
    }

    void WriteJmp(uint32_t at, const void* to, uint32_t len)
    {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &old);
        uint8_t* p = reinterpret_cast<uint8_t*>(at);
        p[0] = 0xE9;
        *reinterpret_cast<uint32_t*>(p + 1) = reinterpret_cast<uint32_t>(to) - (at + 5);
        for (uint32_t i = 5; i < len; ++i)
            p[i] = 0x90;
        VirtualProtect(reinterpret_cast<void*>(at), len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), p, len);
    }

    // The original's registration hook FUN_10278b50 also extends the GLUE event table: when the client
    // registers its 41 glue events it appends list 0x10BE2E74 (every FUN_102783c0 / FUN_10278e70 name).
    // The glue registers them at 0x4DA757 (push 0x29, push 0xB6AFF8, call 0x81B5F0).
    const char* const kGlueEventNames[] = {
        "CHARACTER_ACTIVATE_RESULT",            // FUN_10191190
        "CHARACTER_DEACTIVATE_RESULT",
        "CHARACTER_LIST_UPDATED",
        "CHARACTER_CREATE_SELECTION_CHANGED",   // FUN_1018b290
        "CHARACTER_CREATE_PREVIEW_CHANGED",
    };
    std::vector<const char*> g_glueEvents;
    typedef void(__cdecl* RegisterEvents_t)(const char**, size_t);
    RegisterEvents_t g_registerEvents = nullptr;

    // FUN_10278b50, the original's detour on FrameScript_RegisterEvents 0x81B5F0 (5 bytes: push ebp / mov
    // ebp,esp / push ecx / push ebx). The client's two calls are told apart by their count: 0x29 (the glue's
    // 41, from 0x4DA757) gets the glue customs appended, 0x2D2 (the world's 722, from 0x52AB25) the world's
    // (sDC's custom event list, queued by the modules); any other call passes through.
    void __cdecl RegisterEventsDetour(const char** list, size_t count)
    {
        if (count == 0x29)
        {
            g_glueEvents.assign(list, list + count);
            g_glueEvents.insert(g_glueEvents.end(), std::begin(kGlueEventNames), std::end(kGlueEventNames));
            g_registerEvents(g_glueEvents.data(), g_glueEvents.size());
        }
        else if (count == 0x2D2)
        {
            sDC.SetupFrameEventVector(list, count);
            g_registerEvents(sDC.GetFrameEventVector().data(), sDC.GetFrameEventVector().size());
        }
        else
            g_registerEvents(list, count);
    }

    // 0x727E70 (the unit's aura-type flags) REPLACED by the original (0x102760F0): the +0xF20 bitset gets
    // every type below 0x140 of the unit's auras' applied effects, and a side map (0x10BE2DF8, unit -> 0x30
    // bytes) every type below 0x16E. Nothing in the DLL reads the side map; it is kept, erased when the
    // object is freed (0x10276580) and cleared with the world (0x10276570).
    std::unordered_map<void*, std::array<uint32_t, 12>> g_auraTypes;
    void __fastcall Replace727E70(uint8_t* unit, void*)
    {
        if (!unit)
            return;
        memset(unit + 0xF20, 0, 0x28);
        g_auraTypes[unit].fill(0);
        uint8_t rec[0x2A8] = {};
        const uint32_t n = reinterpret_cast<uint32_t(__thiscall*)(uint8_t*)>(0x4F8850)(unit);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint8_t* aura = reinterpret_cast<const uint8_t*(__thiscall*)(uint8_t*, uint32_t)>(0x556E10)(unit, i);
            if (!aura || !AscScript::FetchSpell(*reinterpret_cast<const uint32_t*>(aura + 8), rec))
                continue;
            const uint32_t mask = *reinterpret_cast<const uint16_t*>(aura + 0xC) & 7;
            for (uint32_t j = 0; j < 3; ++j)
            {
                if (!(mask & (1u << j)))
                    continue;
                const uint32_t type = *reinterpret_cast<const uint32_t*>(rec + 0x17C + j * 4);
                if (type < 0x16E)
                    g_auraTypes[unit][type >> 5] |= 1u << (type & 0x1F);
                if (type < 0x140)
                    unit[0xF20 + (type >> 3)] |= static_cast<uint8_t>(1u << (type & 7));
            }
        }
    }
    void EraseAuraTypes(void* object) { g_auraTypes.erase(object); }   // 0x10276580
    void ClearAuraTypes() { g_auraTypes.clear(); }                     // 0x10276570

    // 0x730290 (__thiscall(unit, a), ret 4) begins push ebp / mov ebp,esp / sub esp,0x200: nine bytes.
    // The original's detour (0x10276000) runs it, then -- for a unit whose descriptor lacks flag 0x10
    // at +8 and whose +0x10C / +0x110 differ -- flags the client object 0x616B10 names (0x6F6020 on the
    // manager 0xC5CE30, +0x18 = 1) and passes the unit's guid to 0x512B50 with 3 (FUN_10111d20).
    typedef int(__fastcall* Fn730290_t)(uint8_t*, void*, uint32_t);
    Fn730290_t g_730290Trampoline = nullptr;
    int __fastcall Detour730290(uint8_t* unit, void* edx, uint32_t a)
    {
        const int r = g_730290Trampoline(unit, edx, a);
        if (!unit)
            return r;
        const uint8_t* d = *reinterpret_cast<uint8_t* const*>(unit + 8);
        if ((d[8] & 0x10) || *reinterpret_cast<const uint32_t*>(d + 0x10C) == *reinterpret_cast<const uint32_t*>(d + 0x110))
            return r;
        if (void* v = reinterpret_cast<void*(__thiscall*)(uint8_t*)>(0x616B10)(unit))
            if (uint8_t* o = reinterpret_cast<uint8_t*(__thiscall*)(void*, void*, int)>(0x6F6020)(reinterpret_cast<void*>(0xC5CE30), v, 0))
            {
                *reinterpret_cast<uint32_t*>(o + 0x18) = 1;
                uint64_t guid = *reinterpret_cast<const uint64_t*>(d);
                if (guid)
                    reinterpret_cast<void(__cdecl*)(uint64_t*, int)>(0x512B50)(&guid, 3);
            }
        return r;
    }

    // 0x6E7F50 (__thiscall(object)) begins push ebp / mov ebp,esp / sub esp,0x1C: six bytes. The original's
    // detour (0x10275E90) runs it, then list 0x10BE2DB8 with the object.
    std::vector<ObjectCallback>& After6E7F50List() { static std::vector<ObjectCallback> v; return v; }
    typedef int(__fastcall* Fn6E7F50_t)(void*, void*);
    Fn6E7F50_t g_6E7F50Trampoline = nullptr;
    int __fastcall Detour6E7F50(void* object, void* edx)
    {
        const int r = g_6E7F50Trampoline(object, edx);
        for (ObjectCallback cb : After6E7F50List())
            cb(object);
        return r;
    }

    // 0x4943C0 (__cdecl(event, b)) begins push ebp / mov ebp,esp / sub esp,0x24: six bytes. The original's
    // detour (0x102763F0): list 0x10BE2B0C with the event while the byte 0xBD0792 is set, then the original.
    // The original's five hook-manager detours whose lists never gain a real registrant (installed since
    // 2026-09-27 for exactness; until then census markers only). Each keeps its list and position:
    //   0x402B20 (__cdecl(), 9 bytes)                0x10277840: original, then list 0x10BE2ACC
    //   0x494490 (__cdecl(event, b), 6 bytes)        0x10276490: in world (0xBD0792), list 0x10BE2B4C, then original
    //   0x494530 (__cdecl(event, b), 6 bytes)        0x10276440: likewise with list 0x10BE2B2C
    //   0x73F660 (__thiscall(obj, a, b), 6, ret 8)   0x10275FC0: original, then list 0x10BE2BCC with the object
    //   0x80FEE0 (__cdecl(param, op, a, store), 9)   0x102778B0: list 0x10BE2D58 with the four, then original
    // 0x494490 / 0x494530 hold the original's one registrant, the empty stub 0x1008D890.
    void EmptyStub(const uint32_t*) {}
    std::vector<void (*)()> g_after402B20;
    std::vector<void (*)(const uint32_t*)> g_before494490{&EmptyStub}, g_before494530{&EmptyStub};
    std::vector<void (*)(void*)> g_after73F660;
    std::vector<void (*)(void*, uint32_t, uint32_t, void*)> g_before80FEE0;

    typedef int(__cdecl* Fn402B20_t)();
    Fn402B20_t g_402B20 = nullptr;
    int __cdecl Detour402B20()
    {
        const int r = g_402B20();
        for (auto cb : g_after402B20)
            cb();
        return r;
    }

    typedef int(__cdecl* Fn494490_t)(const uint32_t*, uint32_t);
    Fn494490_t g_494490 = nullptr, g_494530 = nullptr;
    int __cdecl Detour494490(const uint32_t* event, uint32_t b)
    {
        if (*reinterpret_cast<const uint8_t*>(0xBD0792))
            for (auto cb : g_before494490)
                cb(event);
        return g_494490(event, b);
    }
    int __cdecl Detour494530(const uint32_t* event, uint32_t b)
    {
        if (*reinterpret_cast<const uint8_t*>(0xBD0792))
            for (auto cb : g_before494530)
                cb(event);
        return g_494530(event, b);
    }

    typedef int(__fastcall* Fn73F660_t)(void*, void*, uint32_t, uint32_t);
    Fn73F660_t g_73F660 = nullptr;
    int __fastcall Detour73F660(void* object, void* edx, uint32_t a, uint32_t b)
    {
        const int r = g_73F660(object, edx, a, b);
        for (auto cb : g_after73F660)
            cb(object);
        return r;
    }

    typedef int(__cdecl* Fn80FEE0_t)(void*, uint32_t, uint32_t, void*);
    Fn80FEE0_t g_80FEE0 = nullptr;
    int __cdecl Detour80FEE0(void* param, uint32_t opcode, uint32_t a, void* store)
    {
        for (auto cb : g_before80FEE0)
            cb(param, opcode, a, store);
        return g_80FEE0(param, opcode, a, store);
    }

    std::vector<void (*)(const uint32_t*)>& Before4943C0List() { static std::vector<void (*)(const uint32_t*)> v; return v; }
    typedef int(__cdecl* Fn4943C0_t)(const uint32_t*, uint32_t);
    Fn4943C0_t g_4943C0Trampoline = nullptr;
    int __cdecl Detour4943C0(const uint32_t* event, uint32_t b)
    {
        if (*reinterpret_cast<const uint8_t*>(0xBD0792))
            for (auto cb : Before4943C0List())
                cb(event);
        return g_4943C0Trampoline(event, b);
    }

    std::vector<void (*)(uint32_t)>& AfterAddSpellList() { static std::vector<void (*)(uint32_t)> v; return v; }   // 0x10BE2AEC

    void WriteCallTarget(uint32_t call, const void* to)   // rewrite an E8 rel32
    {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(call + 1), 4, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint32_t*>(call + 1) = reinterpret_cast<uint32_t>(to) - (call + 5);
        VirtualProtect(reinterpret_cast<void*>(call + 1), 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    }
}

const char* const* CustomEventNames(uint32_t& count)
{
    count = static_cast<uint32_t>(sizeof(kEventNames) / sizeof(kEventNames[0]));
    return kEventNames;
}

void RegisterEventNames()
{
    for (const char* n : kEventNames)
        sDC.RegisterCustomEvent(n);
    AscLog::Printf("AscRuntime: %u custom event names queued for the client event table",
                   static_cast<unsigned>(sizeof(kEventNames) / sizeof(kEventNames[0])));
}

// FUN_10278c90: hash first (0x76F640 is __stdcall, ret 4), name compare to confirm.
int EventId(const char* name)
{
    const uint8_t* table = reinterpret_cast<const uint8_t*>(0xD3F7D0);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(table + 4);
    const uint8_t* const* entries = *reinterpret_cast<const uint8_t* const* const*>(table + 8);
    if (!count || !entries)
        return -1;
    const uint32_t hash = reinterpret_cast<uint32_t(__stdcall*)(const char*)>(0x76F640)(name);
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint8_t* e = entries[i];
        if (!e)
            continue;
        if (*reinterpret_cast<const uint32_t*>(e) == hash
            || strcmp(*reinterpret_cast<const char* const*>(e + 0x14), name) == 0)
            return static_cast<int>(i);
    }
    return -1;
}

uint32_t Schedule(uint32_t ms, TimerFn fn, void* param)
{
    return reinterpret_cast<uint32_t(__cdecl*)(uint32_t, TimerFn, void*)>(0x403370)(ms, fn, param);
}

void Cancel(uint32_t handle, TimerFn fn, void* param)
{
    reinterpret_cast<void(__cdecl*)(uint32_t, TimerFn, void*)>(0x4033B0)(handle, fn, param);
}

void* Detour(uint32_t target, uint32_t prologue, void* fn)
{
    void* trampoline = MakeTrampoline(target, prologue);
    WriteJmp(target, fn, prologue);
    AscLog::Printf("AscRuntime: 0x%06X detoured to %p", target, fn);
    return trampoline;
}

void ReplaceFunction(uint32_t target, void* fn)
{
    WriteJmp(target, fn, 5);
    AscLog::Printf("AscRuntime: 0x%06X replaced by %p", target, fn);
}

void OnStockEvent(int eventId, StockEventCallback cb) { StockEventList().emplace_back(eventId, cb); }
void OnAfter403340(Callback cb) { After403340List().push_back(cb); }
void OnLeaveWorld(Callback cb) { LeaveWorldList().push_back(cb); }
void OnBefore528F00(Callback cb) { Before528F00List().push_back(cb); }
void OnAfter6E7F50(ObjectCallback cb) { After6E7F50List().push_back(cb); }
void OnBefore4943C0(void (*cb)(const uint32_t*)) { Before4943C0List().push_back(cb); }
void OnAfterAddSpell(void (*cb)(uint32_t)) { AfterAddSpellList().push_back(cb); }
void RunAfterAddSpell(uint32_t spell)
{
    for (auto cb : AfterAddSpellList())
        cb(spell);
}
void OnAfter402910(Callback cb) { After402910List().push_back(cb); }
void OnAfter5204C0(void (*cb)(void* first)) { After5204C0List().push_back(cb); }
void OnAfter4F6F90(Callback cb) { After4F6F90List().push_back(cb); }
void OnAfter495810(Callback cb) { After495810List().push_back(cb); }

const char* EventName(int eventId)
{
    const uint8_t* table = reinterpret_cast<const uint8_t*>(0xD3F7D0);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(table + 4);
    const uint8_t* const* entries = *reinterpret_cast<const uint8_t* const* const*>(table + 8);
    if (eventId < 0 || static_cast<uint32_t>(eventId) >= count || !entries || !entries[eventId])
        return nullptr;
    return *reinterpret_cast<const char* const*>(entries[eventId] + 0x14);
}

void OnEnterWorld(Callback cb) { EnterWorldList().push_back(cb); }
void OnGlueScreen(Callback cb) { GlueScreenList().push_back(cb); }
void OnAfterEnterWorld(Callback cb) { AfterEnterWorldList().push_back(cb); }
void OnObjectFree(ObjectCallback cb) { ObjectFreeList().push_back(cb); }
void OnAlternate6DF050(Callback cb) { Alternate6DF050List().push_back(cb); }
void OnGameObjectUse(ObjectCallback cb) { GameObjectUseList().push_back(cb); }
void OnItemUse(ObjectCallback cb) { ItemUseList().push_back(cb); }
void OnAfter743EC0(ObjectCallback cb) { After743EC0List().push_back(cb); }
void OnBeforeTargetChange(Callback cb) { BeforeTargetList().push_back(cb); }
void OnAfter52A980(Callback cb) { After52A980List().push_back(cb); }
void OnAfterTargetChange(Callback cb) { AfterTargetList().push_back(cb); }
void OnBefore6ECF80(void (*cb)(uint32_t)) { Before6ECF80List().push_back(cb); }
void OnBefore724820(UnitSpellVeto cb, uint32_t site)
{
    // Closed native list 0x10BE2B6C: AscAura137::BeforeApply, AscSpellVisuals::BeforeAuraApply,
    // AscSpellMods::AuraFlagOn, AscAppearance::BeforeAuraApply, AscSpawnVisibility::BeforeAuraApply;
    // an original-only sixth callback that is not reconstructed runs last. Registration execution
    // order differs from the call-site order, so the captured order is authoritative.
    static const uint32_t order[] = {0x10A683D0, 0x10323151, 0x10324D9A, 0x100C5B88, 0x101A2025};
    InsertByCapturedOrder(Before724820List(), cb, site, order);
}
void OnBefore71E930(UnitSpellVeto cb, uint32_t site)
{
    // Closed native list 0x10BE2B8C: AscAura137::BeforeRemove, AscSpellMods::AuraFlagOff.
    static const uint32_t order[] = {0x10A683DA, 0x10324DA4};
    InsertByCapturedOrder(Before71E930List(), cb, site, order);
}
void OnAfter80B5D0(CastVeto cb, uint32_t site)
{
    // Closed native list 0x10BE2DD8: AuraRequirement, StackRequirement, CastRequirements.
    static const uint32_t order[] = {0x10324DB8, 0x10324DAE, 0x10171F65};
    InsertByCapturedOrder(After80B5D0List(), cb, site, order);
}
void OnEffectFilter(EffectFilter cb, uint32_t site)
{
    // Closed native list 0x10BE2D78: AscSpellVisuals::RemoveEffect, AscQuestSpellHide::RemoveEffect.
    static const uint32_t order[] = {0x1032313D, 0x10112625};
    InsertByCapturedOrder(EffectFilterList(), cb, site, order);
}
void OnVisualHide(VisualHide cb, uint32_t site)
{
    // Closed native list 0x10BE2D98: AscSpellVisuals::HideVisual, AscQuestSpellHide::HideVisual.
    static const uint32_t order[] = {0x10323147, 0x1011262F};
    InsertByCapturedOrder(VisualHideList(), cb, site, order);
}
bool VisualHidden(uint32_t kind, uint32_t guidLo, uint32_t guidHi, uint32_t spell)
{
    for (auto& cb : VisualHideList())
        if (cb.second(kind, guidLo, guidHi, spell))
            return true;
    return false;
}

void HookPacketHandler(uint32_t address, uint32_t prologue, PacketHook hook)
{
    if (address == 0x6DF050)
    {
        g_6DF050Hook = hook;
        return;
    }
    for (HookSlot& slot : g_slots)
        if (!slot.address)
        {
            slot.address = address;
            slot.prologue = prologue;
            slot.hook = hook;
            slot.trampoline = reinterpret_cast<PacketHandler>(MakeTrampoline(address, prologue));
            WriteJmp(address, reinterpret_cast<void*>(kSlotDetours[&slot - g_slots]), prologue);
            return;
        }
    AscLog::Printf("AscRuntime: no free packet-hook slot for 0x%X", address);
}

void InstallHooks()
{
    g_402B20 = reinterpret_cast<Fn402B20_t>(Detour(0x402B20, 9, reinterpret_cast<void*>(&Detour402B20)));
    g_494490 = reinterpret_cast<Fn494490_t>(Detour(0x494490, 6, reinterpret_cast<void*>(&Detour494490)));
    g_494530 = reinterpret_cast<Fn494490_t>(Detour(0x494530, 6, reinterpret_cast<void*>(&Detour494530)));
    g_73F660 = reinterpret_cast<Fn73F660_t>(Detour(0x73F660, 6, reinterpret_cast<void*>(&Detour73F660)));
    g_80FEE0 = reinterpret_cast<Fn80FEE0_t>(Detour(0x80FEE0, 9, reinterpret_cast<void*>(&Detour80FEE0)));
    g_registerEvents = reinterpret_cast<RegisterEvents_t>(Detour(0x81B5F0, 5, reinterpret_cast<void*>(&RegisterEventsDetour)));

    uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memcpy(tramp, reinterpret_cast<void*>(kEnterWorld), kEnterWorldPrologue);
    tramp[kEnterWorldPrologue] = 0xE9;
    *reinterpret_cast<uint32_t*>(tramp + kEnterWorldPrologue + 1) =
        (kEnterWorld + kEnterWorldPrologue) - reinterpret_cast<uint32_t>(tramp + kEnterWorldPrologue + 5);
    g_enterWorldTrampoline = tramp;
    WriteJmp(kEnterWorld, reinterpret_cast<void*>(&EnterWorldDetour), kEnterWorldPrologue);

    WriteJmp(kGlueEpilogue, reinterpret_cast<void*>(&GlueEpilogueCave), 5);

    g_dispatchTrampoline = reinterpret_cast<Dispatch_t>(MakeTrampoline(0x81AA00, 6));
    WriteJmp(0x81AA00, reinterpret_cast<void*>(&DispatchDetour), 6);
    g_403340Trampoline = reinterpret_cast<Fn403340_t>(MakeTrampoline(0x403340, 7));
    WriteJmp(0x403340, reinterpret_cast<void*>(&Detour403340), 7);
    g_495810Trampoline = reinterpret_cast<Fn495810_t>(MakeTrampoline(0x495810, 6));
    WriteJmp(0x495810, reinterpret_cast<void*>(&Detour495810), 6);
    g_6FC0F0Trampoline = reinterpret_cast<Fn6FC0F0_t>(MakeTrampoline(0x6FC0F0, 9));
    WriteJmp(0x6FC0F0, reinterpret_cast<void*>(&Detour6FC0F0), 9);
    g_6DF050Trampoline = reinterpret_cast<Fn6DF050_t>(MakeTrampoline(0x6DF050, 6));
    WriteJmp(0x6DF050, reinterpret_cast<void*>(&Detour6DF050), 6);
    g_70F680Trampoline = reinterpret_cast<Fn70F680_t>(MakeTrampoline(0x70F680, 6));
    WriteJmp(0x70F680, reinterpret_cast<void*>(&Detour70F680), 6);
    g_708C20Trampoline = reinterpret_cast<Fn708C20_t>(MakeTrampoline(0x708C20, 9));
    WriteJmp(0x708C20, reinterpret_cast<void*>(&Detour708C20), 9);
    g_743EC0Trampoline = reinterpret_cast<Fn743EC0_t>(MakeTrampoline(0x743EC0, 6));
    WriteJmp(0x743EC0, reinterpret_cast<void*>(&Detour743EC0), 6);
    g_5204C0Trampoline = reinterpret_cast<Fn5204C0_t>(MakeTrampoline(0x5204C0, 8));
    WriteJmp(0x5204C0, reinterpret_cast<void*>(&Detour5204C0), 8);
    g_4F6F90Trampoline = reinterpret_cast<Fn4F6F90_t>(MakeTrampoline(0x4F6F90, 9));
    WriteJmp(0x4F6F90, reinterpret_cast<void*>(&Detour4F6F90), 9);
    g_52A980Trampoline = reinterpret_cast<Fn52A980_t>(MakeTrampoline(0x52A980, 9));
    WriteJmp(0x52A980, reinterpret_cast<void*>(&Detour52A980), 9);
    g_524BF0Trampoline = reinterpret_cast<Fn524BF0_t>(MakeTrampoline(0x524BF0, 9));
    WriteJmp(0x524BF0, reinterpret_cast<void*>(&Detour524BF0), 9);
    g_6ECF80Trampoline = reinterpret_cast<Fn6ECF80_t>(MakeTrampoline(0x6ECF80, 6));
    WriteJmp(0x6ECF80, reinterpret_cast<void*>(&Detour6ECF80), 6);
    g_724820Trampoline = reinterpret_cast<FnUnitAB_t>(MakeTrampoline(0x724820, 9));
    WriteJmp(0x724820, reinterpret_cast<void*>(&Detour724820), 9);
    g_71E930Trampoline = reinterpret_cast<FnUnitAB_t>(MakeTrampoline(0x71E930, 6));
    WriteJmp(0x71E930, reinterpret_cast<void*>(&Detour71E930), 6);
    g_80B5D0Trampoline = reinterpret_cast<Fn80B5D0_t>(MakeTrampoline(0x80B5D0, 6));
    WriteJmp(0x80B5D0, reinterpret_cast<void*>(&Detour80B5D0), 6);
    WriteJmp(0x744AC0, reinterpret_cast<void*>(&Replace744AC0), 9);
    OnAfter403340(&Sweep);
    g_528C30Trampoline = MakeTrampoline(0x528C30, 6);
    WriteJmp(0x528C30, reinterpret_cast<void*>(&Detour528C30), 6);
    g_528F00Trampoline = MakeTrampoline(0x528F00, 9);
    WriteJmp(0x528F00, reinterpret_cast<void*>(&Detour528F00), 9);
    WriteJmp(0x402910, reinterpret_cast<void*>(&Detour402910), 5);
    WriteJmp(0x727E70, reinterpret_cast<void*>(&Replace727E70), 5);
    g_6E7F50Trampoline = reinterpret_cast<Fn6E7F50_t>(MakeTrampoline(0x6E7F50, 6));
    WriteJmp(0x6E7F50, reinterpret_cast<void*>(&Detour6E7F50), 6);
    g_4943C0Trampoline = reinterpret_cast<Fn4943C0_t>(MakeTrampoline(0x4943C0, 6));
    WriteJmp(0x4943C0, reinterpret_cast<void*>(&Detour4943C0), 6);
    g_730290Trampoline = reinterpret_cast<Fn730290_t>(MakeTrampoline(0x730290, 9));
    WriteJmp(0x730290, reinterpret_cast<void*>(&Detour730290), 9);
    OnObjectFree(&EraseAuraTypes);
    OnLeaveWorld(&ClearAuraTypes);
    OnBefore528F00(&ClearAuraTypes);
    OnAfter402910(&ClearAuraTypes);
    AscLog::Printf("AscRuntime: enter-world detour on 0x528010, glue-screen cave at 0x4DA9AC");
}
}
