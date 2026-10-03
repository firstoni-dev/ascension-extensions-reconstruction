// CharacterAdvancementMgr (0x10bde440) -- the per-player CA builds, the entry index over
// CharacterAdvancement.dbc, the establishment packets and the C_CharacterAdvancement bindings that read
// them. Transcribed from the decompile; function names in comments are the original's.
//
// The learn rules and the reorder pass are in AscCARules.cpp. Not here yet (next): the unlearn / purge /
// other rule sets (+0x1D0..+0x224), the pending build's change hooks
// (ResetPendingBuild lambdas 1..4), the known-spell sets (+0x3E8/+0x408/+0x428, fed by the learned-spell
// hooks FUN_10172660 / FUN_101729d0) and the suggestion objects (+0x28, +0x190, +0x2F8). Bindings that
// depend on any of those are not registered from here until they land.
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscBuildCreator.hpp>
#include <Ascension/AscCAFilter.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellRank.hpp>
#include <Ascension/AscToken.hpp>
#include <Ascension/AscWildcardSets.hpp>
#include <Ascension/AscWildcardRapid.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <Windows.h>

using namespace AscScript;

namespace AscCA
{
namespace
{
#include <Ascension/AscCAEnums.generated.inc>
}
bool ClassTypeAdmits(Row r, uint8_t cls);
namespace
{

    AscDbc::Table& Rows()       { return AscDbc::Get("DBFilesClient\\CharacterAdvancement.dbc"); }
    AscDbc::Table& Essence()    { return AscDbc::Get("DBFilesClient\\CharacterAdvancementEssence.dbc"); }
    AscDbc::Table& ClassTypes() { return AscDbc::Get("DBFilesClient\\CharacterAdvancementClassTypes.dbc"); }
    AscDbc::Table& TabTypes()   { return AscDbc::Get("DBFilesClient\\CharacterAdvancementTabTypes.dbc"); }
    AscDbc::Table& ChrSpecs()   { return AscDbc::Get("DBFilesClient\\ChrSpecs.dbc"); }

    const char* RowStr(Row r, uint32_t off) { return Rows().Str(r, off); }
    float RowF32(Row r, uint32_t off) { return *reinterpret_cast<const float*>(r + off); }

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    std::string ReadCString(CDataStore* p)
    {
        const char* s = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        std::string out(s);
        p->m_read += static_cast<uint32_t>(out.size()) + 1;
        return out;
    }

    // ---- the active unit ---------------------------------------------------------------------------
    // A unit object's type is at +0x14 (3 unit, 4 player); its class is UNIT_FIELD_BYTES_0 byte 1
    // (descriptor +0x5C) and its level UNIT_FIELD_LEVEL (descriptor +0xD8).
    uint8_t UnitClass(const uint8_t* unit)
    {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(unit + 0x14);
        if (type != 3 && type != 4)
            return 0;
        return static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0x5C) >> 8);
    }

    // UNIT_FIELD_LEVEL (descriptor +0xD8): the level an essence row - and therewith every budget,
    // every remaining point and every gate - is read at.
    uint32_t UnitLevel(const uint8_t* unit)
    {
        return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0xD8);
    }

    // ---- row predicates ---------------------------------------------------------------------------
    const uint8_t* Gates() { return RealmInfoSvc::Get().gates; }   // realm object +0x40..+0x47

    // FUN_100c63d0: the stock classes (1..9 and 11).
    bool StockClass(uint8_t c) { return (c >= 1 && c <= 9) || c == 11; }

    // FUN_101c6cc0: the row is available on this realm at all.
    bool Visible(Row r)
    {
        const uint32_t flags = RowU32(r, 0x124);
        if ((flags >> 3) & 1 || flags & 1)
            return false;
        const uint8_t* g = Gates();
        const bool live = g[0] != 0, seasonal = g[1] != 0, league = g[2] != 0, ptr = g[3] != 0,
                   dev = g[4] != 0, g46 = g[6] != 0, g47 = g[7] != 0;
        const bool heroRealm = dev || !(g46 || g47);
        const uint8_t* ct = ClassTypes().Row(RowU32(r, 0x80));
        if (!ct)
            return false;
        if ((RowU32(ct, 0xC) != 0) == heroRealm || (RowU32(ct, 0x10) != 0) == g46 || (RowU32(ct, 0x14) != 0) == g47)
        {
            if ((r[0x128] && live) || (r[0x129] && seasonal) || (r[0x12A] && league) || (r[0x12B] && ptr))
                return true;
            if (r[0x12C])
                return dev;
        }
        return false;
    }

    // FUN_101c6af0: the row's class type admits the active player's class.
    bool ClassAllowed(Row r)
    {
        const uint8_t* player = ActivePlayer();
        if (!player)
            return false;
        return ClassTypeAdmits(r, UnitClass(player));
    }

    uint32_t MaxRank(Row r)   // FUN_101c6030: spells set in the 9-slot chain, up to the first zero
    {
        uint32_t n = 0;
        while (n < 9 && RowU32(r, 0x14 + n * 4) != 0)
            ++n;
        return n;
    }

    // Mode-dependent columns (FUN_1031ef10 flags): bit 3 draft, bit 6 wildcard.
    uint32_t ModeColumn(uint32_t normal, uint32_t draft, uint32_t wildcard)
    {
        const uint32_t mode = AscGameMode::Mode();
        if ((mode >> 3) & 1)
            return draft;
        return ((mode >> 6) & 1) ? wildcard : normal;
    }

    // ---- the entry index (FUN_1014a530) -----------------------------------------------------------
    struct ClassLists { std::vector<Row> other, ability, talent, talentAbility; };   // +0xC/+0x18/+0x24/+0x30

    std::unordered_set<uint32_t> g_allSpells;                       // 0x10bdd540
    std::unordered_map<uint32_t, std::vector<Row>> g_bySpell;       // 0x10bdd4c8
    std::unordered_map<uint32_t, std::vector<Row>> g_byMastery;     // 0x10bdd4e8
    std::vector<Row> g_visible;                                     // 0x10bdd514
    std::unordered_map<uint32_t, ClassLists> g_byClass;             // 0x10bdd560

    std::vector<Row>& ListFor(ClassLists& l, Row r)
    {
        switch (RowType(r))
        {
        case 1: return l.ability;
        case 2: return l.talent;
        case 4: return l.talentAbility;
        default: return l.other;
        }
    }

    void AddToClass(Row r, uint32_t cls)   // FUN_10148850
    {
        std::vector<Row>& list = ListFor(g_byClass[cls], r);
        for (Row x : list)
            if (RowU32(x, 0) == RowU32(r, 0))
                return;
        list.push_back(r);
    }

    template <class Fn> void ForEachRow(Fn fn)
    {
        AscDbc::Table& t = Rows();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (Row r = t.Row(id))
                fn(r);
    }

    void UpdateAllPointers(bool keepMissing);

    void BuildIndex()
    {
        g_allSpells.clear();
        g_byMastery.clear();
        g_visible.clear();
        g_byClass.clear();
        g_bySpell.clear();
        ForEachRow([](Row r) {
            if (Visible(r) && ClassAllowed(r))
            {
                g_visible.push_back(r);
                AddToClass(r, 0);
                if (RowU32(r, 0x80) != 0)
                    AddToClass(r, RowU32(r, 0x80));
            }
        });
        ForEachRow([](Row r) {
            if (!Visible(r))
                return;
            for (uint32_t i = 0; i < 9; ++i)
                if (const uint32_t spell = RowU32(r, 0x14 + i * 4))
                    g_bySpell[spell].push_back(r);
        });
        // FUN_10147dd0: rows the player's class may use first, then by id.
        for (auto& kv : g_bySpell)
            std::sort(kv.second.begin(), kv.second.end(), [](Row a, Row b) {
                const bool ca = ClassAllowed(a), cb = ClassAllowed(b);
                if (ca != cb)
                    return ca;
                return RowU32(a, 0) < RowU32(b, 0);
            });
        ForEachRow([](Row r) {
            if (!Visible(r))
                return;
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t m = RowU32(r, 0x1B0 + i * 4);
                Row mr = m ? FindRow(m) : nullptr;
                if (mr && Visible(mr))
                    g_byMastery[m].push_back(r);
            }
        });
        ForEachRow([](Row r) {
            if (!Visible(r))
                return;
            for (uint32_t i = 0; i < 9; ++i)
                if (const uint32_t spell = RowU32(r, 0x14 + i * 4))
                    g_allSpells.insert(spell);
        });
        UpdateAllPointers(true);
    }


    // ---- known spells: mgr +0x3E8 known, +0x408 rune spells, +0x428 known tags ------------------------
    std::unordered_set<uint32_t> g_knownSpells, g_runeSpells, g_knownTags;

    // FUN_10217450 (map 0x10be05d8, built by FUN_10217670 from SpellTags.dbc's records in file order):
    // spell (+4) -> its tags (+8).
    std::unordered_map<uint32_t, std::vector<uint32_t>>& SpellTagMap()
    {
        static std::unordered_map<uint32_t, std::vector<uint32_t>> tags;
        static bool built = false;
        if (!built)
        {
            built = true;
            AscDbc::Table& t = AscDbc::Get("DBFilesClient/SpellTags.dbc");
            for (uint32_t i = 0; i < t.Count(); ++i)
                if (Row r = t.RowAt(i))
                    tags[RowU32(r, 4)].push_back(RowU32(r, 8));
        }
        return tags;
    }
    const std::vector<uint32_t>& SpellTagsOf(uint32_t spell)
    {
        static const std::vector<uint32_t> none;
        auto it = SpellTagMap().find(spell);
        return it == SpellTagMap().end() ? none : it->second;
    }

    // FUN_102a7f10 over the map FUN_1020b080 builds from SpellCustomAttr.dbc (spell +4 -> row). Its loop
    // reads index slot i for i in [min, max), i.e. ids 2*min .. max+min-1, and that quirk is kept.
    std::unordered_map<uint32_t, Row>& CustomAttrMap() { static std::unordered_map<uint32_t, Row> rows; return rows; }   // 0x10be0618
    bool g_customAttrBuilt = false;
    void BuildCustomAttr()   // FUN_1020b080: the map is cleared, then refilled
    {
        g_customAttrBuilt = true;
        CustomAttrMap().clear();
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\SpellCustomAttr.dbc");
        for (uint32_t i = t.MinId(); t.Loaded() && i < t.MaxId(); ++i)
            if (Row r = t.Row(i + t.MinId()))
                CustomAttrMap()[RowU32(r, 4)] = r;
    }
    Row CustomAttr(uint32_t spell)
    {
        if (!g_customAttrBuilt)
            BuildCustomAttr();
        auto it = CustomAttrMap().find(spell);
        return it == CustomAttrMap().end() ? nullptr : it->second;
    }
    bool CustomAttrA(uint32_t spell, uint32_t mask) { Row r = CustomAttr(spell); return r && (RowU32(r, 0x10) & mask); }   // FUN_103249d0
    bool CustomAttrB(uint32_t spell, uint32_t mask) { Row r = CustomAttr(spell); return r && (RowU32(r, 0x14) & mask); }   // FUN_10324a10
    bool CustomAttrC(uint32_t spell, uint32_t mask) { Row r = CustomAttr(spell); return r && (RowU32(r, 0x18) & mask); }   // +0x18 (FUN_10330f60)

    void WriteCode(uint32_t at, const void* bytes, size_t n)
    {
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old);
        memcpy(reinterpret_cast<void*>(at), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    }

    // The client regenerates runes (0x728A20) only for the active player whose display power type
    // (UNIT_FIELD_BYTES_0 byte 3) matches the operand of `cmp ebx, 6` at 0x728AA1. The original writes 10
    // there while a rune spell is known (FUN_1016d5b0) and 6 when none is (FUN_10173330). No power type 10
    // exists (10 is the Hero class ID), so the original never regenerates runes once any rune spell is
    // known, Death Knights included, and no server packet sets the ready mask (0xC24388) instead.
    // Deliberate departure from the original (IMPROVEMENTS.md): while a rune spell is known the check
    // becomes `cmp ebx, ebx` and the runes regenerate whatever the display power is.
    void SetRuneRegeneration(bool runeSpellKnown)
    {
        static const uint8_t kAlways[3] = {0x39, 0xDB, 0x90};       // cmp ebx, ebx; nop
        static const uint8_t kRunicPower[3] = {0x83, 0xFB, 0x06};   // cmp ebx, 6
        WriteCode(0x728AA1, runeSpellKnown ? kAlways : kRunicPower, 3);
    }

    // FUN_10308830: while > 0, three client calls (0x5216F0 at 0x542679 / 0x5427BE / 0x54284E) are NOPed.
    void SuppressLearnCalls(bool on)
    {
        static int count = 0;
        static uint8_t saved[3][5];
        static const uint32_t kSites[3] = {0x542679, 0x5427BE, 0x54284E};
        static const uint8_t kNops[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        if (on)
        {
            if (count++ == 0)
                for (int i = 0; i < 3; ++i)
                {
                    memcpy(saved[i], reinterpret_cast<const void*>(kSites[i]), 5);
                    WriteCode(kSites[i], kNops, 5);
                }
        }
        else if (count && --count == 0)
            for (int i = 0; i < 3; ++i)
                WriteCode(kSites[i], saved[i], 5);
    }

    // FUN_1016d7c0
    void OnSpellLearned(uint32_t spell, bool silent)
    {
        g_knownSpells.insert(spell);
        uint8_t rec[0x2A8];
        if (FetchSpell(spell, rec) && *reinterpret_cast<const uint32_t*>(rec + 0x288))
        {
            g_runeSpells.insert(spell);
            SetRuneRegeneration(true);
        }
        for (uint32_t tag : SpellTagsOf(spell))
            g_knownTags.insert(tag);
        if (silent)
            return;
        if (CustomAttrA(spell, 0x400))
            AscRuntime::Signal("NOTABLE_SPELL_LEARNED", "%u", spell);
        if (FetchSpell(spell, rec) && (*reinterpret_cast<const uint32_t*>(rec + 0x10) >> 7) & 1)
            AscRuntime::Signal("ASCENSION_HIDDEN_SPELL_LEARNED", "%u", spell);
        AscRuntime::Signal("ASCENSION_SPELLS_UPDATED");
    }

    // FUN_10173550
    void OnSpellUnlearned(uint32_t spell)
    {
        g_knownSpells.erase(spell);
        uint8_t rec[0x2A8];
        if (FetchSpell(spell, rec))
        {
            if ((*reinterpret_cast<const uint32_t*>(rec + 0x10) >> 7) & 1)
                AscRuntime::Signal("ASCENSION_HIDDEN_SPELL_UNLEARNED", "%u", spell);
            if (*reinterpret_cast<const uint32_t*>(rec + 0x288))
            {
                g_runeSpells.erase(spell);
                if (g_runeSpells.empty())
                    SetRuneRegeneration(false);
            }
        }
        std::unordered_set<uint32_t> stillTagged;
        for (uint32_t s : g_knownSpells)
            for (uint32_t tag : SpellTagsOf(s))
                stillTagged.insert(tag);
        for (uint32_t tag : SpellTagsOf(spell))
            if (!stillTagged.count(tag))
                g_knownTags.erase(tag);
        AscRuntime::Signal("ASCENSION_SPELLS_UPDATED");
    }

    const uint8_t* PacketAt(void* packet, uint32_t offset)
    {
        CDataStore* p = static_cast<CDataStore*>(packet);
        return reinterpret_cast<const uint8_t*>(p->m_buffer) + p->m_read + offset;
    }

    // FUN_10172660 on 0x6DF050 (SMSG_INITIAL_SPELLS: u8, u16 count, count x {u32 spell, u16}).
    int __cdecl OnInitialSpells(AscRuntime::PacketHandler original, void* a, uint32_t opcode, uint32_t time, void* packet)
    {
        const uint16_t count = *reinterpret_cast<const uint16_t*>(PacketAt(packet, 1));
        std::vector<uint32_t> spells;
        for (uint32_t i = 0; i < count; ++i)
            spells.push_back(*reinterpret_cast<const uint32_t*>(PacketAt(packet, 3 + i * 6)));
        const int r = original(a, opcode, time, packet);
        for (uint32_t spell : spells)
            OnSpellLearned(spell, true);
        if (!spells.empty())
            AscRuntime::Signal("ASCENSION_SPELLS_UPDATED");
        return r;
    }

    // FUN_10172850 on 0x6E7D60 (a learned spell: u32 spell).
    int __cdecl OnLearnedSpell(AscRuntime::PacketHandler original, void* a, uint32_t opcode, uint32_t time, void* packet)
    {
        const uint32_t spell = *reinterpret_cast<const uint32_t*>(PacketAt(packet, 0));
        const bool quiet = CustomAttrB(spell, 0x40000) || RowBySpell(AscSpellRank::FirstRank(spell)) != nullptr;
        if (quiet)
            SuppressLearnCalls(true);
        const int r = original(a, opcode, time, packet);
        if (quiet)
            SuppressLearnCalls(false);
        OnSpellLearned(spell, false);
        return r;
    }

    // FUN_101729d0 on 0x6E2240 (u32 count, count x u32 spell).
    int __cdecl OnRemovedSpells(AscRuntime::PacketHandler original, void* a, uint32_t opcode, uint32_t time, void* packet)
    {
        const uint32_t count = *reinterpret_cast<const uint32_t*>(PacketAt(packet, 0));
        std::vector<uint32_t> spells;
        for (uint32_t i = 0; i < count; ++i)
            spells.push_back(*reinterpret_cast<const uint32_t*>(PacketAt(packet, 4 + i * 4)));
        const int r = original(a, opcode, time, packet);
        for (uint32_t spell : spells)
            OnSpellUnlearned(spell);
        return r;
    }

    // FUN_10308e40: like SuppressLearnCalls, for the two 0x5216F0 calls at 0x6E7346 / 0x6E7356.
    void SuppressSupersedeCalls(bool on)
    {
        static int count = 0;
        static uint8_t saved[2][5];
        static const uint32_t kSites[2] = {0x6E7346, 0x6E7356};
        static const uint8_t kNops[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        if (on)
        {
            if (count++ == 0)
                for (int i = 0; i < 2; ++i)
                {
                    memcpy(saved[i], reinterpret_cast<const void*>(kSites[i]), 5);
                    WriteCode(kSites[i], kNops, 5);
                }
        }
        else if (count && --count == 0)
            for (int i = 0; i < 2; ++i)
                WriteCode(kSites[i], saved[i], 5);
    }

    bool Wildcard() { return (AscGameMode::Mode() >> 6) & 1; }   // FUN_1031ef10 bit 6

    // FUN_101728e0 on 0x6E7840 (u32 spell [, u8]): a spell removed outside CharacterAdvancement leaves
    // the known set first; a trailing 0 byte marks it at 0x10BDEBC4 while the client handles it (read by
    // the hook on 0x5AAB90). Always 1.
    uint32_t g_removingSpell = 0;   // 0x10BDEBC4
    int __cdecl OnRemovedSpell(AscRuntime::PacketHandler original, void* a, uint32_t opcode, uint32_t time, void* packet)
    {
        CDataStore* p = static_cast<CDataStore*>(packet);
        const uint32_t spell = *reinterpret_cast<const uint32_t*>(PacketAt(packet, 0));
        if (p->m_size > 4 && *PacketAt(packet, 4) == 0)
            g_removingSpell = spell;
        const bool flag = CustomAttrB(spell, 0x40000);
        const bool entry = RowBySpell(AscSpellRank::FirstRank(spell)) != nullptr;
        if (!Wildcard() && !flag && !entry)
            OnSpellUnlearned(spell);
        original(a, opcode, time, packet);
        g_removingSpell = 0;
        return 1;
    }

    // FUN_10193a60 on 0x5AAB90 (__cdecl(spell)): nothing for the spell 0x6E7840 is removing; otherwise the
    // client function runs with its branch at 0x5A8C70 made a jmp (0xEB, the stock jne 0x75 after).
    typedef void(__cdecl* Fn5AAB90_t)(uint32_t);
    Fn5AAB90_t g_5AAB90 = nullptr;
    void __cdecl Hook5AAB90(uint32_t spell)
    {
        if (g_removingSpell == spell)
            return;
        const uint8_t jmp = 0xEB, jne = 0x75;
        WriteCode(0x5A8C70, &jmp, 1);
        g_5AAB90(spell);
        WriteCode(0x5A8C70, &jne, 1);
    }

    // LAB_10193f70 (0x10193F70), IsItemAction(slot): true when action slot 1..144 (0xC1E358, the client's
    // action table) holds an item action (top nibble 8). A non-number or an out-of-range slot is false.
    int IsItemAction(lua_State* L)
    {
        bool item = false;
        if (reinterpret_cast<int(__cdecl*)(lua_State*, int)>(0x84DF20)(L, 1))
        {
            // __dtoul3 (0x10AE6370): double -> unsigned 64-bit, low dword kept
            const uint32_t slot = static_cast<uint32_t>(static_cast<unsigned long long>(CheckNumber(L, 1))) - 1;
            if (slot < 0x90)
            {
                const uint32_t action = reinterpret_cast<const uint32_t*>(0xC1E358)[slot];
                item = action != 0 && (action & 0xF0000000) == 0x80000000;
            }
        }
        reinterpret_cast<void(__cdecl*)(lua_State*, int)>(0x84E4D0)(L, item ? 1 : 0);
        return 1;
    }

    // FUN_10193e40 (the installer of the 0x5AAB90 hook above, called from the attach init at 0x10A662FE):
    // IsItemAction goes into the live Lua state (0x817DB0) through FUN_100a08e0, a lua_pushcclosure +
    // lua_setfield(LUA_GLOBALSINDEX) of a fresh `jmp` thunk. Registered once, never again: no later state
    // has it. Our module init runs in DllMain, before FrameScript has made any state (0x817DB0 is null
    // there -- pushing onto it was the 2026-09-28 load crash), so the one registration is made into the
    // first glue state instead, once its natives are in.
    bool g_isItemActionDone = false;
    bool TryRegisterIsItemAction()
    {
        if (g_isItemActionDone)
            return true;
        lua_State* L = reinterpret_cast<lua_State*(__cdecl*)()>(0x817DB0)();
        if (!L)
            return false;
        g_isItemActionDone = true;
        reinterpret_cast<void(__cdecl*)(lua_State*, lua_CFunction, int)>(0x84E400)(L, &IsItemAction, 0);
        reinterpret_cast<void(__cdecl*)(lua_State*, int, const char*)>(0x84E900)(L, -10002, "IsItemAction");
        return true;
    }
    void RegisterIsItemActionAtGlue() { TryRegisterIsItemAction(); }
    void RegisterIsItemAction()
    {
        if (!TryRegisterIsItemAction())
            AscBindings::OnGlueRegistered(&RegisterIsItemActionAtGlue);
    }

    // FUN_10172b50 on 0x6E7E00 (u32 old, u32 new): a rank replaced -- quietly when either rank is a
    // CharacterAdvancement entry or on a wildcard realm.
    int __cdecl OnSupersededSpell(AscRuntime::PacketHandler original, void* a, uint32_t opcode, uint32_t time, void* packet)
    {
        const uint32_t oldSpell = *reinterpret_cast<const uint32_t*>(PacketAt(packet, 0));
        const uint32_t newSpell = *reinterpret_cast<const uint32_t*>(PacketAt(packet, 4));
        const bool entry = RowBySpell(AscSpellRank::FirstRank(oldSpell)) != nullptr ||
                           RowBySpell(AscSpellRank::FirstRank(newSpell)) != nullptr;
        int r;
        if (!(Wildcard() || entry))
            r = original(a, opcode, time, packet);
        else
        {
            SuppressLearnCalls(true);
            SuppressSupersedeCalls(true);
            r = original(a, opcode, time, packet);
            SuppressLearnCalls(false);
            SuppressSupersedeCalls(false);
        }
        OnSpellUnlearned(oldSpell);
        OnSpellLearned(newSpell, false);
        return r;
    }

    // FUN_10172380: the spell's first rank, or any rank of that chain, is known.
    bool ChainKnown(uint32_t spell)
    {
        const uint32_t first = AscSpellRank::FirstRank(spell);
        if (g_knownSpells.count(first))
            return true;
        for (uint32_t s : AscSpellRank::Chain(first))
            if (g_knownSpells.count(s))
                return true;
        return false;
    }

    // ---- live builds (DAT_10bdd5c0: serial -> build) ----------------------------------------------
    uint64_t g_serial = 0;                  // DAT_10bdd5e8
    std::set<Build*> g_builds;

    void UpdateAllPointers(bool keepMissing)   // FUN_10158750
    {
        for (Build* b : g_builds)
            b->UpdatePointers(keepMissing);
    }

    // ---- the manager ------------------------------------------------------------------------------
    struct Player   // FUN_10148650
    {
        uint64_t guid = 0;            // +0x00
        std::vector<Build*> builds;   // +0x08
        uint32_t spec = 0xFFFFFFFF;   // +0x14
        uint32_t count = 0xFFFFFFFF;  // +0x18

        explicit Player(uint64_t g) : guid(g) {}
        ~Player() { for (Build* b : builds) delete b; }   // FUN_1016d170

        Build* Get(uint32_t i) const   // FUN_10149480
        {
            return (i < builds.size() && i < count) ? builds[i] : nullptr;
        }

        void Ensure(uint32_t i)   // FUN_10149380
        {
            if (i >= 0x14)
                return;
            if (builds.size() <= i)
                builds.resize(i + 1, nullptr);
            if (!builds[i])
                builds[i] = new Build();
        }
    };

    struct Mgr
    {
        Player* player = nullptr;                             // +0x00
        std::unordered_map<uint64_t, Player*> inspect;        // +0x04
        Build* pending = nullptr;                             // +0x24
        std::vector<uint32_t> suggestionOverrides;            // +0x44C
        bool autoLearn = false;                               // +0x458, set by the pending hooks
    } g_mgr;

    uint64_t g_pendingVersion = 0;       // DAT_10bde3d8
    uint64_t g_pendingSeen = 0;          // DAT_10bde430
    bool g_pendingSignal = false;        // DAT_10bde424

    /* True from the moment the window's save hands the pending build to the realm until the realm
     * answers it (0x726, or the 0x72C result for a refusal). Until then that build is the
     * character's - the one being committed - and not a browse, whatever archetype it names. Without
     * this, the window's own readers during the flight (the commit buttons, the chooser) take the
     * build for a browse nobody is waiting on and drop it back to the old archetype, so the trees
     * repoint to the old one and repoint again when the push lands: the "applying a change snaps
     * through the old archetype" report. Set by `AscCACompat::ApplyPendingBuild`, which is where the
     * save leaves the window. */
    bool g_saveInFlight = false;
    /* The switch price the realm quoted with the last preview it answered (its `SendPreviewState`),
     * and the archetype it was quoted for. Which rows a departure bills is the realm's policy - it is
     * the side that owns the archetype defaults - so the price of a switch is asked for rather than
     * derived twice, and the dialog the window raises and the fee the realm takes are one number.
     * Nonzero only while a browse is staged: `ResetPendingBuild` drops it with the staging that asked
     * for it, so it can never be spent on a build the realm did not price. */
    uint32_t g_quoteSpec = 0, g_quoteMarks = 0, g_quoteCopper = 0;
    /* A spec and its build are one state, and the realm pushes them as two packets in step (0x725,
     * then the 0x726 that writes the build). Every read of the active spec answers from its build -
     * a node's rank is `ActiveBuild()->RankOf` - so a spec without its build is rank 0 everywhere:
     * the invested class-tree talents grey for as long as the two packets are apart, then come back
     * (the "left tree flickers grey as it saves" report). The spec is held here until its build
     * exists and applied by OnKnownEntries before it writes it, so the active spec never names an
     * absent build. The announcement goes out with the write, so the window still redraws once, on
     * real data. */
    bool g_specHeld = false;
    uint32_t g_heldSpec = 0, g_heldCount = 0;
    bool g_specSignalDeferred = false;
    time_t g_lastUpdate = 0;             // DAT_10bde428
    std::map<uint8_t, uint32_t> g_credits;   // 0x10bded78 (FUN_101a1c90), category -> amount

    // FUN_10a315d0 +0x198: learn/unlearn requests sent and not yet answered ({id, ..., rank at +0xC}).
    // Only the apply flow fills it, so it stays empty until that lands.
    struct Request { uint32_t id = 0, u4 = 0, u8 = 0, rank = 0; };
    std::vector<Request> g_requests;

    Player* FindPlayer(uint64_t guid)   // FUN_1016f700
    {
        if (!guid)
            return nullptr;
        if (guid == ActivePlayerGuid())
            return g_mgr.player;
        auto it = g_mgr.inspect.find(guid);
        return it == g_mgr.inspect.end() ? nullptr : it->second;
    }

    Build* DefaultBuild()   // 0x10bde8a8, built once on first use (FUN_1014f820(1))
    {
        static Build* b = new Build();
        return b;
    }

    // Callback lists fired by 0x725 and 0x726 (FUN_102780f0 / 10278090 / 10278060 / 102780c0). The
    // subsystems that register into them land with their own transcriptions.
    typedef void (*PairCallback)(uint32_t, uint32_t);
    std::vector<PairCallback> g_onSpecChanged, g_onLearned, g_onRankChanged, g_onUnlearned;

    void Fire(const std::vector<PairCallback>& list, uint32_t a, uint32_t b)
    {
        for (PairCallback cb : list)
            cb(a, b);
    }
}

// ---- rows -----------------------------------------------------------------------------------------
Row FindRow(uint32_t id) { return Rows().Row(id); }

int RowType(Row r)
{
    const char* s = RowStr(r, 4);
    for (int i = 0; i < 5; ++i)
        if (strcmp(s, kEntryTypes[i]) == 0)
            return i;
    return -1;
}

Row RowBySpell(uint32_t spellId)
{
    auto it = g_bySpell.find(spellId);
    if (it == g_bySpell.end() || it->second.empty())
        return nullptr;
    return it->second.front();
}

bool RowVisible(Row r) { return Visible(r); }
uint32_t RowMaxRank(Row r) { return MaxRank(r); }
uint8_t UnitClassOf(const uint8_t* unit) { return UnitClass(unit); }
bool ClassAllowedForPlayer(Row r) { return ClassAllowed(r); }
bool IsCASpell(uint32_t spellId) { return g_allSpells.count(spellId) != 0; }

const std::unordered_set<uint32_t>& KnownSpells() { return g_knownSpells; }

bool SpellKnown(uint32_t spellId, bool orBuild)
{
    typedef char(__thiscall * Knows_t)(void*, uint32_t);
    uint8_t* player = AscScript::ActivePlayer();
    auto knows = [&](uint32_t s) {
        return (player && reinterpret_cast<Knows_t>(0x7260E0)(player, s) != 0) || g_knownSpells.count(s) != 0;
    };
    const std::vector<uint32_t>& chain = AscSpellRank::Chain(spellId);
    if (chain.empty())
    {
        if (knows(spellId))
            return true;
    }
    else
        for (uint32_t s : chain)
            if (knows(s))
                return true;
    if (orBuild)
        if (Row r = RowBySpell(spellId))
            if (const Build* b = ActiveBuild())
                return b->Has(RowU32(r, 0));
    return false;
}

bool ClassTypeAdmits(Row r, uint8_t cls)
{
    const uint8_t* ct = ClassTypes().Row(RowU32(r, 0x80));
    if (!ct)
        return false;
    if (RowU32(ct, 8) != 0)
        return cls == RowU32(ct, 8);
    if (RowU32(ct, 0xC) != 0)
        return cls == 10;
    if (RowU32(ct, 0x10) != 0)
        return cls >= 12 && cls <= 32;
    if (RowU32(ct, 0x14) != 0)
        return StockClass(cls);
    return false;
}

Build* BuildOf(uint64_t guid)
{
    Player* p = FindPlayer(guid);
    return p ? p->Get(p->spec) : nullptr;
}

// ---- Build ------------------------------------------------------------------------------------------
void Build::Register()
{
    serial = ++g_serial;
    g_builds.insert(this);
}

Build::Build()
{
    Register();
    const bool* v = AscConfig::Bool("CONFIG_ENABLE_FULL_DRAFT_MODE");
    draft = v ? *v : 0;
    v = AscConfig::Bool("CONFIG_ENABLE_FULL_WILDCARD_MODE");
    wildcard = v ? *v : 0;
    v = AscConfig::Bool("CONFIG_CHARACTER_ADVANCEMENT_QUALITIES_FORCED");
    qualities = v ? *v : 0;
    Prepare();
}

Build::Build(const Build& src, bool full)
{
    Register();
    guid = src.guid;
    spec = -1;
    classKey = src.classKey;
    draft = src.draft;
    wildcard = src.wildcard;
    specDraft = src.specDraft;
    qualities = src.qualities;
    hero = src.hero;
    coa = src.coa;
    stock = src.stock;
    ready = src.ready;
    u2c = src.u2c;
    level = src.level;
    memcpy(credit, src.credit, sizeof(credit));
    u44 = src.u44;
    u48 = src.u48;
    name = src.name;
    entries = src.entries;
    if (full)
    {
        learnCacheOn = src.learnCacheOn;
        learnCache = src.learnCache;
        Prepare();
    }
}

Build::Build(const BuildSeed& seed, uint32_t lvl, bool full)
{
    Register();
    classKey = seed.classKey;
    const bool* v = AscConfig::Bool("CONFIG_ENABLE_FULL_DRAFT_MODE");
    draft = v ? *v : 0;
    v = AscConfig::Bool("CONFIG_ENABLE_FULL_WILDCARD_MODE");
    wildcard = v ? *v : 0;
    specDraft = seed.category == 8 || seed.category == 10 || seed.category == 11;
    v = AscConfig::Bool("CONFIG_CHARACTER_ADVANCEMENT_QUALITIES_FORCED");
    qualities = v ? *v : 0;
    const uint8_t cls = static_cast<uint8_t>(seed.classKey);
    hero = cls == 10;
    coa = cls >= 12 && cls <= 32;
    stock = StockClass(cls);
    level = lvl ? lvl : AscGameEvents::ServerMaxLevel();
    name = seed.id;   // FUN_10089ef0
    entries.reserve(200);
    for (const auto& s : seed.spells)
        if (s.second <= level)
            if (Row r = RowBySpell(s.first))
                AddRank(RowU32(r, 0));
    if (hero)
    {
        uint32_t id = 0;
        switch (seed.primaryStat)
        {
        case 0: id = 0x47D; break;
        case 1: id = 0x47E; break;
        case 3: id = 0x47F; break;
        case 4: id = 0x480; break;
        }
        if (id)
            AddEntry(id, 1);
    }
    if (full)
        Prepare();
}

Build::~Build() { g_builds.erase(this); }

uint32_t Build::RankOf(uint32_t id) const
{
    for (const Entry& e : entries)
        if (e.id == id)
            return e.rank;
    return 0;
}

bool Build::Has(uint32_t id) const
{
    for (const Entry& e : entries)
        if (e.id == id)
            return true;
    return false;
}

bool Build::LockedOf(uint32_t id) const
{
    for (const Entry& e : entries)
        if (e.id == id)
            return e.locked != 0;
    return false;
}

bool Build::HasGroup(uint32_t group, uint32_t exceptId) const
{
    if (!group)
        return false;
    for (const Entry& e : entries)
        if (RowU32(e.row, 0x74) == group && e.id != exceptId)
            return true;
    return false;
}

namespace
{
    // FUN_10153190 & co: the Essence.dbc row for (level, class, the build's mode flags).
    Row EssenceRow(const Build& b, uint32_t lvl)
    {
        AscDbc::Table& t = Essence();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            Row r = t.Row(id);
            if (!r || RowU32(r, 4) != lvl || RowU32(r, 8) != b.classKey)
                continue;
            if ((RowU32(r, 0xC) != 0) == (b.draft != 0) && (RowU32(r, 0x10) != 0) == (b.wildcard != 0)
                && (RowU32(r, 0x14) != 0) == (b.specDraft != 0) && (RowU32(r, 0x18) != 0) == (b.u2c != 0))
                return r;
        }
        return nullptr;
    }

    // An unowned, named wildcard build (a build-creator build) is never short of essence.
    bool Unlimited(const Build& b) { return b.guid == 0 && b.wildcard && !b.name.empty(); }

    // The investment sums re-add each counted entry into an emptied copy (FUN_1014eb80(this, 0) +
    // FUN_10157d30(1), then FUN_101510d0 per entry), so a wildcard choice group (+0x74) is charged only
    // for its first entry.
    enum Cost { AE, TE };

    uint32_t Sum(const Build& b, Cost cost, uint32_t minOff, uint32_t minimum, int cls, int tab)
    {
        std::vector<uint32_t> seenGroups, seenIds;
        uint32_t total = 0;
        for (const Entry& e : b.entries)
        {
            if (minimum && RowU32(e.row, minOff) >= minimum)
                continue;
            const uint32_t group = RowU32(e.row, 0x74);
            const bool match = (cls < 0 || RowU32(e.row, 0x80) == static_cast<uint32_t>(cls))
                            && (tab < 0 || RowU32(e.row, 0x84) == static_cast<uint32_t>(tab));
            if (match)
            {
                const bool charged = !b.wildcard || !group
                                  || std::find(seenGroups.begin(), seenGroups.end(), group) == seenGroups.end();
                if (charged)
                    total += cost == AE ? b.AEColumn(e.row, e.rank) : b.TEColumn(e.row, e.rank);
            }
            if (std::find(seenIds.begin(), seenIds.end(), e.id) == seenIds.end())
            {
                seenIds.push_back(e.id);
                seenGroups.push_back(group);
            }
        }
        return total;
    }
}

uint32_t Build::AEBudget(uint32_t lvl) const
{
    if (Unlimited(*this))
        return 999;
    Row r = EssenceRow(*this, lvl);
    return r ? RowU32(r, 0x1C) : 0;
}

uint32_t Build::TEBudget(uint32_t lvl) const
{
    if (Unlimited(*this))
        return 999;
    Row r = EssenceRow(*this, lvl);
    return r ? RowU32(r, 0x20) : 0;
}

uint32_t Build::AEColumn(Row r, uint32_t ranks) const
{
    if (draft)
        return RowU32(r, 0x48) * ranks;
    return (wildcard ? RowU32(r, 0x58) : RowU32(r, 0x38)) * ranks;
}

uint32_t Build::TEColumn(Row r, uint32_t ranks) const   // the wildcard TE cost is flat
{
    if (draft)
        return RowU32(r, 0x4C) * ranks;
    return wildcard ? RowU32(r, 0x5C) : RowU32(r, 0x3C) * ranks;
}

uint32_t Build::AECost(Row r, uint32_t ranks) const
{
    if (wildcard && RowU32(r, 0x74) && HasGroup(RowU32(r, 0x74), 0))
        return 0;
    return AEColumn(r, ranks);
}

uint32_t Build::TECost(Row r, uint32_t ranks) const
{
    if (wildcard && RowU32(r, 0x74) && HasGroup(RowU32(r, 0x74), 0))
        return 0;
    return TEColumn(r, ranks);
}

uint32_t Build::GlobalAE(uint32_t minimum) const { return Sum(*this, AE, 0x88, minimum, -1, -1); }
uint32_t Build::GlobalTE(uint32_t minimum) const { return Sum(*this, TE, 0x8C, minimum, -1, -1); }
uint32_t Build::ClassAE(uint32_t cls, uint32_t minimum) const { return Sum(*this, AE, 0x90, minimum, static_cast<int>(cls), -1); }
uint32_t Build::ClassTE(uint32_t cls, uint32_t minimum) const { return Sum(*this, TE, 0x94, minimum, static_cast<int>(cls), -1); }
uint32_t Build::TabAE(uint32_t cls, uint32_t tab, uint32_t minimum) const { return Sum(*this, AE, 0x98, minimum, static_cast<int>(cls), static_cast<int>(tab)); }
uint32_t Build::TabTE(uint32_t cls, uint32_t tab, uint32_t minimum) const { return Sum(*this, TE, 0x9C, minimum, static_cast<int>(cls), static_cast<int>(tab)); }

uint32_t Build::ClassPoints(uint32_t cls, uint32_t minimum) const   // FUN_10153e90
{
    uint32_t total = 0;
    for (const Entry& e : entries)
        if ((!minimum || RowU32(e.row, 0xA4) < minimum) && RowU32(e.row, 0x80) == cls)
            total += RowU32(e.row, 0xA0) * e.rank;
    return total;
}

uint32_t Build::RemainingAE() const
{
    const uint32_t spent = GlobalAE(0), budget = AEBudget(level);
    return spent < budget ? budget - spent : 0;
}

uint32_t Build::RemainingTE() const
{
    const uint32_t spent = GlobalTE(0), budget = TEBudget(level);
    return spent < budget ? budget - spent : 0;
}

// FUN_101552e0: a tab of 1 means "any tab of the class"; no requirement is always met.
bool Build::MeetsPoints(uint32_t cls, uint32_t tab, uint32_t points, uint32_t minimum) const
{
    if (!points)
        return true;
    uint32_t total = 0;
    for (const Entry& e : entries)
    {
        if (minimum && RowU32(e.row, 0xA4) >= minimum)
            continue;
        if (RowU32(e.row, 0x80) == cls && (tab == 1 || RowU32(e.row, 0x84) == tab))
            total += RowU32(e.row, 0xA0) * e.rank;
        if (points <= total)
            return true;
    }
    return false;
}

void Build::UpdatePointers(bool keepMissing)
{
    for (size_t i = 0; i < entries.size();)
    {
        Row r = FindRow(entries[i].id);
        if (!r && !(Gates()[4] && keepMissing))
            AscLog::Printf("CharacterAdvancementBuildEntry::UpdatePointers: entry %u not found", entries[i].id);
        entries[i].row = r;
        if (r)
            ++i;
        else
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---- manager ------------------------------------------------------------------------------------------
int ActiveSpecIndex()
{
    Player* p = FindPlayer(ActivePlayerGuid());
    return p ? static_cast<int>(p->spec) : -1;
}

Build* ActiveBuild()
{
    const uint64_t guid = ActivePlayerGuid();
    if (Player* p = FindPlayer(guid))
        if (Build* b = p->Get(p->spec))
            return b;
    // Every read of the window's state - a node's rank (`PushTalentRank`), whether a row is known
    // (`IsKnownID`), the prices - answers from this build. The spec's own build is absent only
    // between the spec and its build (0x725, then 0x726), and only on an archetype's first visit:
    // a visited one keeps its build cached. Answering the *empty* default there is what greys every
    // node for as long as the two packets are apart - the "first switch per archetype flickers grey"
    // report. The staged build is the one the window is drawing and the one being committed, so it
    // is the honest answer for that window.
    if (g_mgr.pending)
        return g_mgr.pending;
    return DefaultBuild();
}

Build* PendingBuild() { return g_mgr.pending; }

void RequestAutoLearn() { g_mgr.autoLearn = true; }

    /* The pass every pending-build edit asks for (`Build::Add`/`SetRank`/`Remove` call
     * `RequestAutoLearn`): every free 0x100000 row the pending build can take, one rank at a
     * time. It is the client's own rule for the rows the window draws at rank 1 without the
     * player buying anything - an archetype's passives - and it is what the manager's tick runs
     * once the flag is up. Returns true when it added anything. Run here rather than only on the
     * tick so a staged archetype comes up complete in the same frame it is staged, instead of
     * growing its passives a second later.
     *
     * Swept to a fixpoint, because the rows it takes are a *chain*: an archetype's level passives
     * gate on each other (9311 at 20, then 4715 at 30, 4733 at 40 and 13133 at 50, each requiring
     * the one before it through the connected-entry rule), and a row whose prerequisite sorts
     * *after* it in entry-id order - 4715 before 9311 - is refused on the sweep that reaches it and
     * can only be taken by a later one. One sweep therefore learned the level-20 node alone and
     * left the rest to whatever happened to run the pass again, which on a staged preview was the
     * manager's next tick, whose result nothing announced: the engine held the archetype's passives
     * while the window went on drawing the build it had, and they appeared only once a state push
     * (a save, a relog) replaced it. Each sweep adds at least one rank and ranks only grow, so the
     * fixpoint is reached, in one call, in at most as many sweeps as the chain has links. */
    bool RunAutoLearnPass()
    {
        g_mgr.autoLearn = false;
        Build* p = g_mgr.pending;
        if (!p)
            return false;
        bool learned = false;
        // The held-rank total is what decides whether a sweep progressed, not "did it add one":
        // taking a group-1 row evicts the build's other group-1 rows on the way in (`Build::AddEntry`),
        // so a sweep can add a rank and drop one, and a pair like that would otherwise keep the
        // sweeps going round for ever. The total only grows, is bounded by the candidate rows' max
        // ranks, and every real sweep of a chain raises it (see above).
        auto ranksHeld = [p]() {
            size_t total = 0;
            for (const Entry& e : p->entries)
                total += e.rank;
            return total;
        };
        for (size_t held = ranksHeld();;)
        {
            ForEachRow([&](Row r) {
                if (!RowHasFlag(r, 0x100000))
                    return;
                const uint32_t id = RowU32(r, 0);
                while (p->AECost(r, 1) == 0 && p->TECost(r, 1) == 0 && p->ValidateLearn(nullptr, id, {}, {}) == 0)
                {
                    p->AddRank(id);
                    learned = true;
                }
            });
            if (size_t const now = ranksHeld(); now > held)
                held = now;
            else
                break;
        }
        // `AddRank` re-raises the flag the whole way down, and the sweeps above have now taken
        // everything it was asking for: leaving it up would ask the tick for a sweep that can only
        // find nothing.
        g_mgr.autoLearn = false;
        if (learned)
        {
            // The engine's own convention after a pending-build write (`AddRanks`, `ResetPendingBuild`,
            // the staged preview): leave the entries in the order the rules accept. This pass appends
            // in table order, which is not dependency order - 4715 is written ahead of 9311, the row
            // it needs - and the in-order validation behind Save Changes and its cost popup reads the
            // entries in the order they are stored.
            p->Reorder(false);
            // The version moves for the ranks gained, so the pass's own work is announced (see Update).
            BumpPendingVersion();
        }
        return learned;
    }

    // The pending-build change the window's trees listen for, run where the change actually
    // happened instead of waiting for the manager's next tick. `Update()` fires the same signal
    // once a second, so a state push or a staged preview left both trees drawing the build they
    // had before it until that tick came round - the "the nodes show up a moment later" report.
    // The work is the tick's own: the entry counts on both builds and the recent-set rebuild,
    // then the signal, deferred only if a learn/unlearn request is still in flight (the tick
    // would otherwise deliver it mid-request, which is what `g_pendingSignal` is for).
    //
    // `immediate` is for a build that is complete the moment it is written - a staged archetype's
    // defaults, or the pending build coming back to the character's own. There the request gate
    // protects nothing and only costs the window a second of drawing the build it just replaced,
    // so the signal goes out now; the tick keeps waiting, because there the build is mid-change.
    // Set by the glue's install, once the Lua helper the tree mark calls exists.
    bool g_windowTrees = false;

    namespace
    {
        // Defined with `SpecOf` below, which it needs: marks the window's two trees for the build the
        // pending one now holds. A whole build written at once is announced to the window, and an
        // announcement cannot reach a tree that is hidden at the time - this is the half that can. See
        // the definition.
        void MarkWindowTreesFromPending();
        // Defined with `MarkWindowTrees` below: installs the window's own ends of a browse (see
        // AscCACoAGlue). Called from the staging that starts one, which is the moment the window is
        // certain to exist - the glue's events have both passed by the time the realm installs.
        void EnsureBrowseEnds();
        // Defined with `ParamsFromUnit` below, which it needs: completes a build a state push wrote
        // with the parameters the unit implies, so nothing visible waits for the manager's tick.
        bool CompleteBuildFromUnit(Build& b);
    }

    /* The browse/preview diagnostic, on only when the client was asked for it: `COA_COMPAT_TRACE=1`
     * in its environment. It writes Logs\CoACompatBrowse.log without the `-extlog` switch, so the
     * ordering of a staged preview against the reads the window makes of it can be read straight off
     * disk whatever a client was launched with - and it writes a line per read the window makes while
     * drawing (`GetActiveChrSpec`, `CanApplyPendingBuild`), which is why it is off unless asked for: a
     * file write and a flush inside a UI read path is not something a player should pay for the
     * diagnostic of. The call sites stay where they are; the switch is what makes them free. */
    bool CoATraceEnabled()
    {
        static bool const enabled = []
        {
            char value[8] = {};
            return GetEnvironmentVariableA("COA_COMPAT_TRACE", value, sizeof(value)) != 0 &&
                value[0] != '\0' && value[0] != '0';
        }();
        return enabled;
    }

    void CoADbg(const char* fmt, ...)
    {
        if (!CoATraceEnabled())
            return;
        static bool opened = false;
        static FILE* file = nullptr;
        if (!opened)
        {
            opened = true;
            CreateDirectoryA("Logs", nullptr);
            file = fopen("Logs\\CoACompatBrowse.log", "a");
            if (file)
                fputs("---- session ----\n", file);
        }
        if (!file)
            return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(file, fmt, ap);
        va_end(ap);
        fputc('\n', file);
        fflush(file);
    }

    /* "id:rank,..." for a build's entries - the one place every rank the window draws is read from.
     * Empty while the trace is off, because this is only ever an argument to a line that is not going
     * to be written: walking and formatting every rank of a build for a discarded line is the cost the
     * switch above exists to avoid, and the argument is evaluated before the call that would drop it. */
    std::string CoADbgEntries(const Build* b)
    {
        if (!CoATraceEnabled())
            return {};
        if (!b)
            return "<none>";
        std::string out;
        char one[32];
        for (const Entry& e : b->entries)
        {
            _snprintf(one, sizeof(one), "%s%u:%u", out.empty() ? "" : ",", e.id, e.rank);
            out += one;
        }
        return out.empty() ? "<empty>" : out;
    }

    void FlushPendingBuildSignal(bool immediate = false)
    {
        if (!g_mgr.pending || g_pendingVersion == g_pendingSeen)
            return;
        g_pendingSeen = g_pendingVersion;
        auto count = [](Build* b) {
            b->u44 = 0;
            b->u48 = 0;
            for (const Entry& e : b->entries)
                if (Row r = FindRow(e.id))
                {
                    const int type = RowType(r);
                    if (type == 1 || type == 4)
                        ++b->u44;
                    else if (type == 2)
                        ++b->u48;
                }
        };
        if (Build* active = ActiveBuild())
            count(active);
        count(g_mgr.pending);
        AscCAFilter::RebuildRecentSets();   // FUN_101ca1d0, FUN_101c4360, FUN_101ca500 on mgr +0x190
        if (immediate || g_requests.empty())
        {
            g_pendingSignal = false;
            CoADbg("signal immediate=%d pendingRows=%u [%s]", immediate ? 1 : 0,
                   unsigned(g_mgr.pending->entries.size()), CoADbgEntries(g_mgr.pending).c_str());
            // The trees the announcement may not reach, marked where they are - and before it goes
            // out, so the spec the window is drawing is still the one the mark is compared against.
            if (immediate)
                MarkWindowTreesFromPending();
            AscRuntime::Signal("CHARACTER_ADVANCEMENT_PENDING_BUILD_UPDATED");
        }
        else
            g_pendingSignal = true;
    }

uint64_t BumpPendingVersion() { return ++g_pendingVersion; }

uint32_t AbilityCount(const Build& b)
{
    uint32_t count = 0;
    for (const Entry& e : b.entries)
        if (Row r = FindRow(e.id))
        {
            const int type = RowType(r);
            count += type == 1 || type == 4;
        }
    return count;
}

void ResetPendingBuild()
{
    Build* next = new Build(*ActiveBuild(), true);
    Build* old = g_mgr.pending;
    g_mgr.pending = next;
    delete old;
    next->Reorder(false);   // FUN_101557d0(0)
    next->pendingHooks = true;   // lambdas 1..4 (FUN_10157fb0 / 10157e60 / 10157f40 / 10157ed0)
    ++g_pendingVersion;
    // The staging that asked for a price is gone, so the price goes with it: a quote is only ever
    // spent on the browse it was quoted for (see CanApplyPendingBuild).
    g_quoteSpec = g_quoteMarks = g_quoteCopper = 0;
}

namespace
{
    // ---- packets ------------------------------------------------------------------------------------
    void EnsurePlayer()
    {
        if (g_mgr.player)
            return;
        Player* p = new Player(ActivePlayerGuid());
        Player* old = g_mgr.player;
        g_mgr.player = p;
        delete old;
    }

    // FUN_10171d90: SMSG 0x725 -- u32 active spec, u32 spec count.
    void __cdecl OnActiveSpec(void* a, uint32_t opcode, uint32_t b, CDataStore* p)
    {
        if (!g_mgr.player)
        {
            EnsurePlayer();
            ResetPendingBuild();
        }
        const uint32_t spec = Read<uint32_t>(p);
        const uint32_t count = Read<uint32_t>(p);
        const uint32_t old = g_mgr.player->spec;
        if (!g_mgr.player->Get(spec))
        {
            // Held, not applied: the build for it rides the 0x726 (see g_specHeld). Nothing is
            // announced either - the window redraws on that announcement, and a redraw now would
            // read rank 0 for every node. OnKnownEntries applies it as it writes the build.
            g_specHeld = true;
            g_heldSpec = spec;
            g_heldCount = count;
            g_specSignalDeferred = true;
            AscLog::Printf("CoACompat: active spec %u -> %u held for its build (0x726)", old, spec);
            return;
        }
        g_mgr.player->spec = spec;
        g_mgr.player->count = count;
        // Reached only with the build in hand (the absent case returns above): the spec, the build it
        // names and the announcement are one transition, so no reader - and no redraw - can land
        // between them. Logged so a report of a flicker has a timeline.
        AscLog::Printf("CoACompat: active spec %u -> %u (%u specs), build present", old, spec, count);
        if (old != spec)
            Fire(g_onSpecChanged, old & 0xFF, spec & 0xFF);
        AscRuntime::Signal("ASCENSION_CA_SPECIALIZATION_ACTIVE_ID_CHANGED", "%u", spec + 1);
    }

    // FUN_101a1c00: SMSG 0x926 -- u8 category, u32 amount; the active build picks the four up.
    void RefreshCredits()   // FUN_10180520
    {
        if (!g_mgr.player)
            return;
        if (Build* b = g_mgr.player->Get(g_mgr.player->spec))
        {
            for (uint8_t c = 1; c <= 4; ++c)
            {
                auto it = g_credits.find(c);
                b->credit[c - 1] = it == g_credits.end() ? 0 : it->second;
            }
        }
    }

    // FUN_101a1b70 (every glue screen): the credit map emptied (FUN_101a1c90 -> FUN_101092d0).
    void ClearCredits() { g_credits.clear(); }

    void __cdecl OnCredit(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint8_t category = Read<uint8_t>(p);
        g_credits[category] = Read<uint32_t>(p);
        RefreshCredits();
    }

    void ParamsFromUnit(Build& out, const uint8_t* unit, int32_t spec);
    bool SyncParams(Build& b, const Build& p);

    // FUN_10171260: SMSG 0x726 -- the known entries of the active spec's build, diffed into events.
    void __cdecl OnKnownEntries(void* a, uint32_t opcode, uint32_t b, CDataStore* p)
    {
        bool created = false;
        if (!g_mgr.player)
        {
            EnsurePlayer();
            created = true;
        }
        // The spec 0x725 held back, applied here and not before: the build it names is written into
        // it below, so this is the moment both halves of the transition exist together (g_specHeld).
        if (g_specHeld)
        {
            g_specHeld = false;
            const uint32_t previous = g_mgr.player->spec;
            g_mgr.player->spec = g_heldSpec;
            g_mgr.player->count = g_heldCount;
            if (previous != g_heldSpec)
                Fire(g_onSpecChanged, previous & 0xFF, g_heldSpec & 0xFF);
        }
        if (!g_mgr.player->Get(g_mgr.player->spec))
        {
            g_mgr.player->Ensure(g_mgr.player->spec);
            created = true;
        }
        Build* build = g_mgr.player->Get(g_mgr.player->spec);
        if (!build)
        {
            // The packet is left unread here, as the original does.
            if (created)
                ResetPendingBuild();
            return;
        }

        if (const uint8_t* unit = ActivePlayer())
        {
            Build params;
            ParamsFromUnit(params, unit, ActiveSpecIndex());
            if (SyncParams(*build, params))
                build->Prepare();
        }

        Build old(*build, true);
        std::vector<Entry> fresh;   // FUN_10166760
        const uint32_t n = Read<uint32_t>(p);
        fresh.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            Entry e;
            e.id = Read<uint32_t>(p);
            e.rank = Read<uint32_t>(p);
            e.u0c = Read<uint32_t>(p);
            e.locked = Read<uint8_t>(p);
            e.u18 = Read<uint32_t>(p);
            e.u1c = Read<uint32_t>(p);
            fresh.push_back(e);
        }

        std::vector<std::pair<uint32_t, uint32_t>> changed, learned, rankChanged, unlearned;
        std::vector<uint32_t> added, removed;
        for (const Entry& e : fresh)
        {
            if (!old.Has(e.id))
            {
                added.push_back(e.id);
                learned.emplace_back(e.id, e.rank);
            }
            else if (e.rank != old.RankOf(e.id) || (e.locked != 0) != old.LockedOf(e.id))
            {
                changed.emplace_back(e.id, e.rank);
                if (e.rank != old.RankOf(e.id))
                    rankChanged.emplace_back(e.id, e.rank);
            }
        }
        for (const Entry& e : old.entries)
        {
            bool still = false;
            for (const Entry& f : fresh)
                still = still || f.id == e.id;
            if (!still)
            {
                removed.push_back(e.id);
                unlearned.emplace_back(e.id, e.rank);
            }
        }

        build->entries = fresh;   // FUN_10158010 + FUN_10158e40(0)
        build->UpdatePointers(false);
        // The build is written complete: the parameters its budget, its point totals and every gate
        // that reads one are computed from are filled here, where the realm's build lands, instead of
        // being left to the manager's tick (see CompleteBuildFromUnit). On an archetype's first
        // switch this is what kept the window's counters at a grey 0/0 until the tick ran.
        CompleteBuildFromUnit(*build);

        for (auto& c : changed)
            AscRuntime::Signal("ASCENSION_KNOWN_ENTRY_UPDATED", "%u%u", c.first, c.second);
        for (uint32_t id : added)
            AscRuntime::Signal("ASCENSION_ENTRY_UPDATED", "%u", id);
        for (uint32_t id : removed)
            AscRuntime::Signal("ASCENSION_KNOWN_ENTRY_REMOVED", "%u", id);
        for (auto& x : learned)
            Fire(g_onLearned, x.first, x.second);
        for (auto& x : rankChanged)
            Fire(g_onRankChanged, x.first, x.second);
        for (auto& x : unlearned)
            Fire(g_onUnlearned, x.first, x.second);
        if (!changed.empty() || !added.empty() || !removed.empty())
            AscRuntime::Signal("ASCENSION_KNOWN_ENTRIES_UPDATED");

        RefreshCredits();
        ResetPendingBuild();
        g_saveInFlight = false;   // the save, if this answered one, has landed
        AscLog::Printf("CoACompat: state push for spec %u, %u entries, active build %s", g_mgr.player->spec,
                       static_cast<unsigned>(fresh.size()),
                       g_mgr.player->Get(g_mgr.player->spec) ? "present" : "absent");
        // The spec change this push answers, announced now that the build it names is written (see
        // OnActiveSpec). Ahead of the flush, so the window has repointed before the trees are marked.
        if (g_specSignalDeferred)
        {
            g_specSignalDeferred = false;
            AscRuntime::Signal("ASCENSION_CA_SPECIALIZATION_ACTIVE_ID_CHANGED", "%u", g_mgr.player->spec + 1);
        }
        // Handed over whole, so it is announced the moment it is written: the window draws the
        // realm's build instead of the one it is holding, without waiting for the tick.
        FlushPendingBuildSignal(true);
    }

    // FUN_10170360 / FUN_10170b50 / FUN_10170750: a result name, looked up in its enum only to decide
    // success (index 0); the event carries the name as sent.
    uint32_t ResultIndex(const std::string& s, const char* const* names, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (s == names[i])
                return i;
        return 1;
    }

    // CoA staged preview (SMSG 0x7B4, our realm's own opcode): the archetype's
    // authoritative defaults, applied to the *pending* build. SMSG 0x726 cannot do this -
    // its handler writes the active build and resets pending, so a preview sent that way
    // becomes the character's own build and Save Changes has nothing to commit. The realm
    // untouched means the true active build never changes here; the pending build simply
    // starts holding the previewed archetype's baseline, and the player builds on from it
    // exactly as the shipped window expects. specId 0 means the archetype asked for is the
    // character's own: the pending build returns to it.
    void __cdecl OnPreviewState(void*, uint32_t, uint32_t, CDataStore* p)
    {
        if (!g_mgr.player)
            EnsurePlayer();
        if (!g_mgr.player->Get(g_mgr.player->spec))
            g_mgr.player->Ensure(g_mgr.player->spec);
        Build* active = g_mgr.player->Get(g_mgr.player->spec);
        if (!active)
            return;

        const uint32_t n = Read<uint32_t>(p);
        std::vector<Entry> fresh;
        fresh.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            Entry e;
            e.id = Read<uint32_t>(p);
            e.rank = Read<uint32_t>(p);
            e.u0c = Read<uint32_t>(p);
            e.locked = Read<uint8_t>(p);
            e.u18 = Read<uint32_t>(p);
            e.u1c = Read<uint32_t>(p);
            fresh.push_back(e);
        }
        const uint32_t specId = Read<uint32_t>(p);
        const uint32_t aeCredit = Read<uint32_t>(p);
        const uint32_t teCredit = Read<uint32_t>(p);
        // The realm's own price for the switch this preview offers (see `g_quoteSpec`). A realm that
        // sends none leaves the price to the window's own computation. Stored after the staging below,
        // which resets the quote along with the build it belonged to.
        uint32_t quoteMarks = 0, quoteCopper = 0;
        if (p->m_size - p->m_read >= 8)
        {
            quoteMarks = Read<uint32_t>(p);
            quoteCopper = Read<uint32_t>(p);
        }

        if (!specId)
        {
            // The archetype asked for is the character's own: the pending build returns to it, and
            // the window is told in the same breath, so the trees move back onto the character's
            // archetype now instead of on the manager's next tick.
            CoADbg("stage spec=0 -> own: pending reset to the character's build");
            ResetPendingBuild();
            FlushPendingBuildSignal(true);
            return;
        }

        ResetPendingBuild();
        AscLog::Printf("CoACompat: staged preview of spec %u, %u entries, credit %u|%u", specId,
                       static_cast<unsigned>(fresh.size()), aeCredit, teCredit);
        if (Build* b = g_mgr.pending)
        {
            b->entries = fresh;
            b->UpdatePointers(false);
            b->Reorder(false);
            // The previewed archetype's own credit is the realm's, and the staged build never
            // takes away the credit the player already holds: the credit is what the realm says
            // the character has, so the staging raises it to what the previewed baseline needs
            // and never lowers it below what is already there.
            b->credit[0] = std::max(b->credit[0], aeCredit);
            b->credit[1] = std::max(b->credit[1], teCredit);
            // Staging writes the rows directly, which skips the hook an edit goes through, so the
            // client's own auto-learn pass is run here instead of waiting for the tick to find a
            // flag nobody set. The realm's baseline is the same set, so this is normally silent;
            // when the two disagree, the client's own rule wins for its own build and the save
            // that follows carries the rows it drew.
            RunAutoLearnPass();
            CoADbg("stage spec=%u realmSent=%u afterAutoLearnRows=%u quote=%u/%u [%s]", specId,
                   unsigned(fresh.size()), unsigned(b->entries.size()), quoteMarks, quoteCopper,
                   CoADbgEntries(b).c_str());
            g_quoteSpec = specId;
            g_quoteMarks = quoteMarks;
            g_quoteCopper = quoteCopper;
            BumpPendingVersion();
            // The browse starts here, so its ends have to exist from here: see `EnsureBrowseEnds`.
            EnsureBrowseEnds();
        }
        // One signal, not two. The window moves onto the previewed archetype off the pending-build
        // event: `CoATalentFrameMixin` spends its `expectingNewSpec` flag on it, repoints the two
        // trees at `GetActiveChrSpec` (the staged build), redraws every node from the ranks it
        // holds and refreshes the commit buttons and the point counters. A second, spec-changed
        // signal only buys a second `UpdateActiveSpec` - and by then the trees are shown, so that
        // pass marks the *previous* archetype's nodes dirty and the player watches them repaint
        // before the rebuild replaces them. The character's archetype has not changed either: only
        // a save changes that, which is what `OnActiveSpec` (0x725) announces. `true`: the staged
        // build is complete state, so it is announced now rather than behind an apply in flight.
        FlushPendingBuildSignal(true);
    }

    void __cdecl OnLearnResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x728
    {
        const std::string result = ReadCString(p);
        const uint32_t a = Read<uint32_t>(p);
        const uint32_t b = Read<uint32_t>(p);
        const bool ok = ResultIndex(result, kLearnResults, 0x2E) == 0;
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_LEARN_RESULT", "%b%s%u%u", ok, result.c_str(), a, b);
    }

    void __cdecl OnUnlearnResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x729
    {
        const std::string result = ReadCString(p);
        const uint32_t a = Read<uint32_t>(p);
        const bool ok = ResultIndex(result, kUnlearnResults, 0x17) == 0;
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_UNLEARN_RESULT", "%b%s%u", ok, result.c_str(), a);
    }

    void __cdecl OnPurgeResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x72A
    {
        const std::string result = ReadCString(p);
        const bool ok = ResultIndex(result, kPurgeResults, 10) == 0;
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_PURGE_ABILITIES_RESULT", "%b%s", ok, result.c_str());
    }

    // ---- CharacterAdvancementMgr::Update (FUN_10182780), once a second -----------------------------
    // FUN_1014e4d0: the build parameters a unit implies, as FUN_10158810 compares and copies them.
    void ParamsFromUnit(Build& out, const uint8_t* unit, int32_t spec)
    {
        const uint8_t* desc = *reinterpret_cast<uint8_t* const*>(unit + 8);
        out.guid = *reinterpret_cast<const uint64_t*>(desc);
        out.spec = spec;
        const uint8_t cls = UnitClass(unit);
        out.classKey = cls;
        const uint32_t mode = AscGameMode::Mode();
        out.draft = (mode >> 3) & 1;
        out.wildcard = (mode >> 6) & 1;
        out.specDraft = AscGameMode::SpecBuildDraft(spec);
        const bool* q = AscConfig::Bool("CONFIG_CHARACTER_ADVANCEMENT_QUALITIES_ENABLED");
        out.qualities = (q && *q && !((mode >> 3) & 1) && !((mode >> 6) & 1) && (mode & 0x400)) ? 1 : 0;
        out.hero = cls == 10;
        out.coa = cls >= 12 && cls <= 32;
        out.stock = StockClass(cls);
        out.u2c = AscObjectAddon::Unit(ActivePlayerGuid())[2].value;   // FUN_102d9f00
        out.level = UnitLevel(unit);
        out.name.clear();
        out.u64 = out.u65 = 0;
    }

    // FUN_10158810: copy the parameters that differ; true when any did.
    bool SyncParams(Build& b, const Build& p)
    {
        bool changed = false;
        auto sync = [&changed](auto& dst, const auto& src) {
            if (dst != src)
            {
                dst = src;
                changed = true;
            }
        };
        sync(b.guid, p.guid);
        sync(b.spec, p.spec);
        sync(b.classKey, p.classKey);
        sync(b.draft, p.draft);
        sync(b.wildcard, p.wildcard);
        sync(b.specDraft, p.specDraft);
        sync(b.qualities, p.qualities);
        sync(b.hero, p.hero);
        sync(b.coa, p.coa);
        sync(b.stock, p.stock);
        sync(b.u2c, p.u2c);
        sync(b.level, p.level);
        sync(b.name, p.name);
        sync(b.u64, p.u64);
        sync(b.u65, p.u65);
        return changed;
    }

    /* FUN_1014e4d0 + FUN_10158810, run where a build is *written* and not only on the manager's tick.
     * Every number a build reports is computed from its parameters - its class and level (the essence
     * row that holds the level's budget), the mode flags that pick a cost column, `u2c` - and a build
     * the manager had not seen yet carried none of them: the state push creates it, writes its entries
     * and leaves `classKey`/`level` at 0 until the tick repairs them, up to a second later. In that
     * second the build's budget, its remaining points and every gate that reads one answered zero, and
     * the window - whose point counters read the pending copy of that build - drew them grey 0/0 on
     * the first switch into each archetype, recovering when the tick caught up. Completing the build
     * where it is written makes the written state whole, so nothing the player sees depends on when
     * the tick happens to run. True when anything differed. */
    bool CompleteBuildFromUnit(Build& b)
    {
        const uint8_t* unit = ActivePlayer();
        if (!unit)
            return false;
        Build params;
        ParamsFromUnit(params, unit, ActiveSpecIndex());
        if (!SyncParams(b, params))
            return false;
        b.Prepare();   // FUN_10157450, as the tick's own path does
        return true;
    }

    /* The one parameter the unit moves while the window is open, watched per frame instead of once a
     * second. Every point total, gate and rule is read at the build's own level, and the build
     * followed the unit only inside the throttled sync below - so a window opened in the second after
     * a level-up still answered with the previous level's essence row: the points stayed at the value
     * the player had a moment ago until something repainted them ("level up, press N, the points are
     * the old ones"). The check is one descriptor read; the sync runs the moment the level differs,
     * the staged build comes with it, and the window is told in the same breath - what it holds is
     * complete state, so the announcement is immediate. True when the level moved. */
    bool FollowLevelChange(const uint8_t* unit)
    {
        if (!g_mgr.player || !unit)
            return false;
        Build* b = g_mgr.player->Get(g_mgr.player->spec);
        if (!b || b->level == UnitLevel(unit))
            return false;
        if (!CompleteBuildFromUnit(*b))
            return false;
        if (g_mgr.pending)
        {
            g_mgr.pending->level = b->level;
            g_mgr.pending->u2c = b->u2c;
        }
        BumpPendingVersion();
        FlushPendingBuildSignal(true);
        // The throttled sync below is where a level change used to be noticed, and it announced it a
        // second time (CHARACTER_ADVANCEMENT_BUILD_LEVEL_UPDATED, which the classic window's currency
        // bars follow). Syncing here first would leave that sync seeing no change and silence it, so
        // the announcement is made here, where the change is actually seen.
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_BUILD_LEVEL_UPDATED");
        return true;
    }

    void Update()
    {
        bool resetPending = false, levelUpdated = false;
        const time_t now = time(nullptr);
        const uint8_t* unit = ActivePlayer();
        // A level-up is the one change that must not wait for the tick below (see FollowLevelChange).
        FollowLevelChange(unit);
        if (now != g_lastUpdate && unit)
        {
            g_lastUpdate = now;
            Player* player = FindPlayer(ActivePlayerGuid());
            if (player)
            {
                if (player->guid != ActivePlayerGuid())
                    player->guid = ActivePlayerGuid();
                Build* b = player->Get(player->spec);
                if (b)
                {
                    Build old(*b, true);
                    Build params;
                    ParamsFromUnit(params, unit, ActiveSpecIndex());
                    if (SyncParams(*b, params))
                    {
                        b->Prepare();   // FUN_10157450 (SyncParams' second argument)
                        if (g_mgr.pending)
                        {
                            g_mgr.pending->level = b->level;
                            g_mgr.pending->u2c = b->u2c;
                        }
                        resetPending = b->classKey != old.classKey || b->draft != old.draft || b->wildcard != old.wildcard
                                    || b->specDraft != old.specDraft || b->qualities != old.qualities || b->hero != old.hero
                                    || b->coa != old.coa || b->stock != old.stock || b->name != old.name
                                    || b->u64 != old.u64 || b->u65 != old.u65;
                        levelUpdated = b->level != old.level || b->u2c != old.u2c;
                    }
                }
            }
        }
        // A pass that learned something must be announced here like any other change: the flag it
        // was asked for is cleared by the pass itself now that a call reaches its fixpoint, so this
        // block is the only place left that can tell the window about the rows it took. It used to
        // be skipped for exactly that case (`!autoLearned`), and the rows the pass had just gained -
        // an archetype's level passives, one chain link per sweep - then stayed in the engine while
        // the window kept drawing the build it already had.
        if (!resetPending && g_mgr.autoLearn)
            RunAutoLearnPass();
        if (!resetPending && g_pendingVersion != g_pendingSeen && g_mgr.pending)
        {
            g_pendingSeen = g_pendingVersion;
            // Update(void)::`48'::<lambda_1> (FUN_10183220), on the active build then the pending one:
            // +0x44 counts the ability rows it holds (FUN_101c6770), +0x48 the talents (FUN_101c6e80).
            auto count = [](Build* b) {
                b->u44 = 0;
                b->u48 = 0;
                for (const Entry& e : b->entries)
                    if (Row r = FindRow(e.id))
                    {
                        const int type = RowType(r);
                        if (type == 1 || type == 4)
                            ++b->u44;
                        else if (type == 2)
                            ++b->u48;
                    }
            };
            if (Build* active = ActiveBuild())
                count(active);
            count(g_mgr.pending);
            AscCAFilter::RebuildRecentSets();   // FUN_101ca1d0, FUN_101c4360, FUN_101ca500 on mgr +0x190
            g_pendingSignal = true;
        }
        // CHARACTER_ADVANCEMENT_PENDING_BUILD_UPDATED waits until no learn/unlearn request is in flight.
        if (g_pendingSignal && g_requests.empty())
        {
            g_pendingSignal = false;
            AscRuntime::Signal("CHARACTER_ADVANCEMENT_PENDING_BUILD_UPDATED");
        }
        if (resetPending)
            ResetPendingBuild();
        if (levelUpdated)
            AscRuntime::Signal("CHARACTER_ADVANCEMENT_BUILD_LEVEL_UPDATED");
    }

    void OnGlueScreen()   // FUN_1016ec70 (the parts this file owns)
    {
        for (auto& kv : g_mgr.inspect)
            delete kv.second;
        g_mgr.inspect.clear();
        g_knownSpells.clear();   // +0x3E8 (FUN_100df440)
        g_runeSpells.clear();    // +0x408
        SetRuneRegeneration(false);   // 0x728AA3 = 6 (missing until 2026-09-27; the live audit showed 6 in the original)
        g_knownTags.clear();     // +0x428
        g_mgr.suggestionOverrides.clear();   // +0x44C
        g_pendingSignal = false;             // DAT_10bde424
        g_mgr.autoLearn = false;             // +0x458
        ++g_pendingVersion;                  // DAT_10bde3d8
        // +0x00 (FUN_1016d170): the player's record goes too, so the next character's first 0x725 makes a
        // fresh one. Kept, it handed that character the previous one's builds until its own arrived.
        Player* old = g_mgr.player;
        g_mgr.player = nullptr;
        delete old;
    }

    // ---- argument readers -------------------------------------------------------------------------
    // FUN_1016b250 / FUN_1016b650: a class / tab type given by name (the enum value is index + 1).
    uint32_t EnumArg(lua_State* L, int idx, const char* const* names, uint32_t count)
    {
        const char* s = CheckString(L, idx);
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(s, names[i]) == 0)
                return i + 1;
        AscLog::Printf("lua_tovalue<enum>: Unknown string '%s'", s);
        return 0;
    }

    // FUN_10166520: (classType, minInvestment).
    bool ClassMinArgs(lua_State* L, uint32_t& cls, uint32_t& minimum)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return false;
        cls = EnumArg(L, 1, kClassTypes, 0x2F);
        minimum = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        return true;
    }

    // FUN_10166680: (classType, tabType, minInvestment).
    bool ClassTabMinArgs(lua_State* L, uint32_t& cls, uint32_t& tab, uint32_t& minimum)
    {
        if (!ValidateInput(L, {STRING, STRING, NUMBER}))
            return false;
        cls = EnumArg(L, 1, kClassTypes, 0x2F);
        tab = EnumArg(L, 2, kTabTypes, 0x5F);
        minimum = static_cast<uint32_t>(ToInt(CheckNumber(L, 3)));
        return true;
    }

    int Usage(lua_State* L, const char* msg) { return AscLua::luaL_error(L, msg); }

    // ---- the entry table (FUN_10172cf0: FUN_101c03c0 builds it, FUN_1016c910 serialises it) --------
    void SetInt(lua_State* L, const char* k, int32_t v) { AscLua::lua_pushstring(L, k); AscLua::lua_pushinteger(L, v); AscLua::lua_settable(L, -3); }
    void SetNum(lua_State* L, const char* k, float v)   { AscLua::lua_pushstring(L, k); AscLua::lua_pushnumber(L, v); AscLua::lua_settable(L, -3); }
    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushstring(L, v.c_str());
        AscLua::lua_settable(L, -3);
    }
    void SetInts(lua_State* L, const char* k, const std::vector<uint32_t>& v)   // FUN_1009a460
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int32_t>(v[i]));
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    std::vector<uint32_t> NonZero(Row r, uint32_t off, uint32_t count)
    {
        std::vector<uint32_t> v;
        for (uint32_t i = 0; i < count; ++i)
            if (const uint32_t x = RowU32(r, off + i * 4))
                v.push_back(x);
        return v;
    }

    std::string ClassName(uint32_t cls)
    {
        if (const uint8_t* ct = ClassTypes().Row(cls))
            return ClassTypes().Str(ct, 4);
        return "INVALID_CLASS_TYPE_" + std::to_string(cls);
    }

    std::string TabName(uint32_t tab)
    {
        if (const uint8_t* tt = TabTypes().Row(tab))
            return TabTypes().Str(tt, 4);
        return "INVALID_TAB_TYPE_" + std::to_string(tab);
    }

    void PushEntry(lua_State* L, Row r)
    {
        if (!r)
        {
            AscLua::lua_pushnil(L);
            return;
        }
        AscLua::lua_createtable(L, 0, 0x25);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "ID", static_cast<int32_t>(RowU32(r, 0)));
        SetStr(L, "Type", RowStr(r, 4));
        SetInts(L, "Spells", NonZero(r, 0x14, 9));
        SetStr(L, "Class", ClassName(RowU32(r, 0x80)));
        SetStr(L, "Name", RowStr(r, 0xBC));
        SetInt(L, "RequiredLevel", static_cast<int32_t>(RowU32(r, ModeColumn(0x68, 0x6C, 0x70))));
        SetStr(L, "Icon", RowStr(r, 0xC0));
        SetInt(L, "AECost", static_cast<int32_t>(RowU32(r, ModeColumn(0x38, 0x48, 0x58))));
        SetInt(L, "TECost", static_cast<int32_t>(RowU32(r, ModeColumn(0x3C, 0x4C, 0x5C))));
        SetStr(L, "Quality", RowStr(r, ModeColumn(0x40, 0x50, 0x60)));
        SetInt(L, "QualityCost", static_cast<int32_t>(RowU32(r, ModeColumn(0x44, 0x54, 0x64))));
        SetInt(L, "Column", static_cast<int32_t>(RowU32(r, 0x78)));
        SetInt(L, "Row", static_cast<int32_t>(RowU32(r, 0x7C)));
        SetStr(L, "Tab", TabName(RowU32(r, 0x84)));
        SetInt(L, "RequiredAEInvestment", static_cast<int32_t>(RowU32(r, 0x88)));
        SetInt(L, "RequiredTEInvestment", static_cast<int32_t>(RowU32(r, 0x8C)));
        SetInt(L, "RequiredClassAEInvestment", static_cast<int32_t>(RowU32(r, 0x90)));
        SetInt(L, "RequiredClassTEInvestment", static_cast<int32_t>(RowU32(r, 0x94)));
        SetInt(L, "RequiredTabAEInvestment", static_cast<int32_t>(RowU32(r, 0x98)));
        SetInt(L, "RequiredTabTEInvestment", static_cast<int32_t>(RowU32(r, 0x9C)));
        SetInt(L, "Points", static_cast<int32_t>(RowU32(r, 0xA0)));
        SetInt(L, "RequiredClassPoints", static_cast<int32_t>(RowU32(r, 0xA4)));
        SetInt(L, "Flags", static_cast<int32_t>(RowU32(r, 0x124)));
        SetInts(L, "Masteries", NonZero(r, 0x1B0, 3));
        SetInts(L, "ConnectedNodes", NonZero(r, 0xE4, 15));
        SetInt(L, "Group", static_cast<int32_t>(RowU32(r, 0x74)));
        SetNum(L, "PositionX", RowF32(r, 0xD0));
        SetNum(L, "PositionY", RowF32(r, 0xD4));
        SetInts(L, "RequiredIDs", NonZero(r, 0x08, 3));
        SetStr(L, "NodeType", RowStr(r, 0x1AC));
        SetInt(L, "ParentNode", static_cast<int32_t>(RowU32(r, 0x19C)));
        SetNum(L, "SizeX", RowF32(r, 0xD8));
        SetNum(L, "SizeY", RowF32(r, 0xDC));
        SetNum(L, "Distance", RowF32(r, 0x1A0));
        SetStr(L, "Anchor", RowStr(r, 0x1A4));
        SetStr(L, "Color", RowStr(r, 0x1A8));

        // SpellCastReq: up to three {class, tab, points} requirements, each checked on the active build.
        AscLua::lua_pushstring(L, "SpellCastReq");
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        Build* active = ActiveBuild();
        for (uint32_t i = 0, n = 1; i < 3; ++i)
        {
            const uint32_t cls = RowU32(r, 0x1D8 + i * 4), tab = RowU32(r, 0x1E4 + i * 4), points = RowU32(r, 0x1F0 + i * 4);
            if (!cls && !tab)
                break;
            AscLua::lua_pushnumber(L, static_cast<double>(n++));
            AscLua::lua_createtable(L, 0, 4);
            AscLua::lua_checkstack(L, 2);
            SetInt(L, "ClassType", static_cast<int32_t>(cls));
            SetInt(L, "TabType", static_cast<int32_t>(tab));
            SetInt(L, "Points", static_cast<int32_t>(points));
            AscLua::lua_pushstring(L, "IsMet");
            AscLua::lua_pushboolean(L, active->MeetsPoints(cls, tab, points, 0) ? 1 : 0);
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }


    // ---- suggestions (manager 0x10bcc618, FUN_103338f0) ------------------------------------------------
    // Weighted "what to learn next" lists, computed on the client from SpellSpellSuggestions.dbc (source
    // spell +4 -> suggested spell +8, weight +0xC) and SpellStatSuggestions.dbc (source spell +4 -> stat
    // +8, weight +0xC) over the entries the player has (or the context overrides when any are set).
    struct Suggestions
    {
        std::vector<std::pair<uint32_t, uint32_t>> entries;   // +0x00 {entry id, weight}
        std::vector<std::pair<uint32_t, uint32_t>> stats;     // +0x0C {stat, weight}
        bool statsDirty = true, spellsDirty = true, dirty = true;   // +0x18 / +0x19 / +0x1A
    } g_sugg;

    // FUN_10148bc0 (DAT_10bdd508, rebuilt at most every five seconds): spell -> the first row granting
    // it among the visible, class-allowed rows the active build has. Spells past Spell.dbc's max id
    // (0xAD49DC) are left out.
    std::vector<std::pair<uint32_t, Row>> g_knownSpellIndex;   // 0x10bdd508
    __time64_t g_knownSpellNext = 0;                           // 0x10bdd580

    const std::vector<std::pair<uint32_t, Row>>& KnownSpellIndex()
    {
        auto& index = g_knownSpellIndex;
        auto& next = g_knownSpellNext;
        if (next <= _time64(nullptr))
        {
            index.clear();
            const uint32_t maxSpell = *reinterpret_cast<const uint32_t*>(0xAD49DC);
            const Build* active = ActiveBuild();
            ForEachRow([&](Row r) {
                if (!Visible(r) || !ClassAllowed(r) || !active->Has(RowU32(r, 0)))
                    return;
                for (uint32_t off = 0x14; off < 0x38; off += 4)
                {
                    const uint32_t spell = RowU32(r, off);
                    if (!spell || spell > maxSpell)
                        continue;
                    auto it = std::lower_bound(index.begin(), index.end(), spell,
                                               [](const std::pair<uint32_t, Row>& p, uint32_t s) { return p.first < s; });
                    if (it == index.end() || spell < it->first)
                        index.insert(it, {spell, r});
                }
            });
            next = _time64(nullptr) + 5;
        }
        return index;
    }

    // ---- one row in / out of the entry index (SMSG 0x64A) ----------------------------------------------
    // Unlike BuildIndex these do not check Visible for the spell sets, the masteries or the spell lists,
    // and the all-spells set follows the row's 0x1000 flag instead; kept as the original has it.
    template <class Pred> void EraseIf(std::vector<Row>& v, Pred pred) { v.erase(std::remove_if(v.begin(), v.end(), pred), v.end()); }

    void UnindexRow(Row r)   // FUN_10148fd0
    {
        const uint32_t id = RowU32(r, 0);
        auto same = [id](Row x) { return RowU32(x, 0) == id; };
        if (RowHasFlag(r, 0x1000))
            for (uint32_t off = 0x14; off < 0x38; off += 4)
                if (const uint32_t spell = RowU32(r, off))
                    g_allSpells.erase(spell);
        for (uint32_t off = 0x1B0; off < 0x1BC; off += 4)
            if (const uint32_t m = RowU32(r, off))
                EraseIf(g_byMastery[m], same);
        EraseIf(g_visible, same);
        for (auto it = g_byClass.begin(); it != g_byClass.end();)   // FUN_101473c0
        {
            ClassLists& l = it->second;
            for (std::vector<Row>* v : {&l.other, &l.ability, &l.talent, &l.talentAbility})
                EraseIf(*v, same);
            if (l.other.empty() && l.ability.empty() && l.talent.empty() && l.talentAbility.empty())
                it = g_byClass.erase(it);
            else
                ++it;
        }
        for (uint32_t off = 0x14; off < 0x38; off += 4)
        {
            const uint32_t spell = RowU32(r, off);
            if (!spell)
                continue;
            EraseIf(g_bySpell[spell], same);
            auto it = std::lower_bound(g_knownSpellIndex.begin(), g_knownSpellIndex.end(), spell,
                                       [](const std::pair<uint32_t, Row>& p, uint32_t s) { return p.first < s; });
            if (it != g_knownSpellIndex.end() && it->first == spell && RowU32(it->second, 0) == id)
                g_knownSpellIndex.erase(it);
        }
    }

    void IndexRow(Row r)   // FUN_1014a330(row, 1)
    {
        if (RowHasFlag(r, 0x1000))
            for (uint32_t off = 0x14; off < 0x38; off += 4)
                if (const uint32_t spell = RowU32(r, off))
                    g_allSpells.insert(spell);
        for (uint32_t off = 0x1B0; off < 0x1BC; off += 4)
            if (const uint32_t m = RowU32(r, off))
                g_byMastery[m].push_back(r);
        if (Visible(r) && ClassAllowed(r))
        {
            const uint32_t id = RowU32(r, 0);
            if (std::none_of(g_visible.begin(), g_visible.end(), [id](Row x) { return RowU32(x, 0) == id; }))
                g_visible.push_back(r);
            AddToClass(r, 0);
            if (RowU32(r, 0x80) != 0)
                AddToClass(r, RowU32(r, 0x80));
        }
        for (uint32_t off = 0x14; off < 0x38; off += 4)
            if (const uint32_t spell = RowU32(r, off))
                g_bySpell[spell].push_back(r);
        if (Visible(r) && ClassAllowed(r))
        {
            const uint32_t maxSpell = *reinterpret_cast<const uint32_t*>(0xAD49DC);   // FUN_100b1020
            for (uint32_t off = 0x14; off < 0x38; off += 4)
            {
                const uint32_t spell = RowU32(r, off);
                if (!spell || !g_knownSpellNext || spell > maxSpell)
                    continue;
                auto it = std::lower_bound(g_knownSpellIndex.begin(), g_knownSpellIndex.end(), spell,
                                           [](const std::pair<uint32_t, Row>& p, uint32_t s) { return p.first < s; });
                if (it == g_knownSpellIndex.end() || it->first != spell)
                    g_knownSpellIndex.insert(it, {spell, r});
                else
                    it->second = r;   // FUN_10148520: operator[]
            }
        }
    }

    // FUN_10216a60 (map 0x10be20f0) / FUN_10216c60 (map 0x10be2124): the suggestion rows whose source
    // spell the player has, keyed by the entry that grants it.
    std::unordered_map<uint32_t, std::vector<Row>> g_spellSuggestions, g_statSuggestions;

    void BuildSuggestionMap(std::unordered_map<uint32_t, std::vector<Row>>& map, const char* path)
    {
        const std::vector<std::pair<uint32_t, Row>>& index = KnownSpellIndex();
        map.clear();
        AscDbc::Table& t = AscDbc::Get(path);
        if (!t.Loaded())
            return;
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
            if (Row s = t.Row(id))
            {
                const uint32_t spell = RowU32(s, 4);
                auto it = std::lower_bound(index.begin(), index.end(), spell,
                                           [](const std::pair<uint32_t, Row>& p, uint32_t x) { return p.first < x; });
                if (it != index.end() && !(spell < it->first) && it->second)
                    map[RowU32(it->second, 0)].push_back(s);
            }
    }

    // FUN_10333260: the suggestion context of an entry type name; logs and falls back otherwise.
    const char* const kSuggestionContexts[7] = {"None", "Ability", "Talent", "Trait", "TalentAbility", "Tag", "Suggestion"};
    uint8_t SuggestionContext(const char* name, uint8_t fallback)
    {
        for (uint8_t i = 0; i < 7; ++i)
            if (strcmp(name, kSuggestionContexts[i]) == 0)
                return i;
        AscLog::Printf("Unexpected Value: %s", name);
        return fallback;
    }

    // Doubled for a locked source entry, a tenth for one the wildcard roller lists as undesired
    // (single-precision, truncated through __ftol like the original).
    uint32_t Weigh(Row src, uint32_t w)
    {
        if (ActiveBuild()->LockedOf(RowU32(src, 0)))   // FUN_10155640
        {
            const float f = static_cast<float>(static_cast<double>(w));
            w = static_cast<uint32_t>(static_cast<int64_t>(f + f));
        }
        if (AscWildcardSets::IsUndesired(RowU32(src, 0), SuggestionContext(RowStr(src, 4), 0)))
            w = static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(static_cast<double>(w)) * 0.1f));
        return w;
    }

    void AddWeight(std::vector<std::pair<uint32_t, uint32_t>>& list, uint32_t key, uint32_t w)
    {
        for (auto& p : list)
            if (p.first == key)
            {
                p.second += w;
                return;
            }
        list.push_back({key, w});
    }

    // FUN_10333a00.
    void RecomputeSuggestions()
    {
        g_sugg.dirty = false;
        g_sugg.entries.clear();
        g_sugg.stats.clear();
        std::vector<uint32_t> ids;
        if (g_mgr.suggestionOverrides.empty())   // FUN_10171f50
            for (const Entry& e : ActiveBuild()->entries)
                ids.push_back(e.id);
        else
            ids = g_mgr.suggestionOverrides;
        for (uint32_t id : ids)
        {
            auto sp = g_spellSuggestions.find(id);
            if (sp != g_spellSuggestions.end())
                for (Row s : sp->second)
                {
                    Row target = RowBySpell(RowU32(s, 8));
                    if (!target)
                        continue;
                    Row src = RowBySpell(RowU32(s, 4));
                    if (!src)
                        continue;
                    AddWeight(g_sugg.entries, RowU32(target, 0), Weigh(src, RowU32(s, 0xC)));
                }
            auto st = g_statSuggestions.find(id);
            if (st != g_statSuggestions.end())
                for (Row s : st->second)
                {
                    Row src = RowBySpell(RowU32(s, 4));
                    if (!src)
                        continue;
                    AddWeight(g_sugg.stats, RowU32(s, 8), Weigh(src, RowU32(s, 0xC)));
                }
        }
        auto heavier = [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) { return a.second > b.second; };
        std::sort(g_sugg.entries.begin(), g_sugg.entries.end(), heavier);   // FUN_10332ea0
        std::sort(g_sugg.stats.begin(), g_sugg.stats.end(), heavier);
        AscLog::Printf("[CA] suggestions: %u ids, maps %u/%u, index %u -> %u entries, %u stats", static_cast<unsigned>(ids.size()),
                       static_cast<unsigned>(g_spellSuggestions.size()), static_cast<unsigned>(g_statSuggestions.size()),
                       static_cast<unsigned>(KnownSpellIndex().size()), static_cast<unsigned>(g_sugg.entries.size()),
                       static_cast<unsigned>(g_sugg.stats.size()));
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_SUGGESTIONS_UPDATED");
        AscRuntime::Signal("STAT_SUGGESTIONS_UPDATED");
    }

    // 0x10333960, after 0x403340. Its one-second gate compares against DAT_10d3c180, which nothing
    // writes, so it always passes.
    void SuggestionTick()
    {
        if (g_sugg.statsDirty)
        {
            g_sugg.statsDirty = false;
            BuildSuggestionMap(g_statSuggestions, "DBFilesClient\\SpellStatSuggestions.dbc");
        }
        if (g_sugg.spellsDirty)
        {
            g_sugg.spellsDirty = false;
            BuildSuggestionMap(g_spellSuggestions, "DBFilesClient\\SpellSpellSuggestions.dbc");
        }
        if (g_sugg.dirty)
            RecomputeSuggestions();
    }

    void SuggestionsOnGlue()   // 0x10333560
    {
        g_sugg.statsDirty = g_sugg.spellsDirty = g_sugg.dirty = true;
        g_sugg.entries.clear();
    }

    void SuggestionsDirty(uint32_t, uint32_t) { g_sugg.dirty = true; }   // 0x10333950

    // SMSG 0x6CE (FUN_101e3a70) / 0x6D2 (FUN_101e3b20): one SpellSpellSuggestions / SpellStatSuggestions
    // row {u32 id, u32, u32, u32}, patched into the table.
    void UpsertSuggestionRow(const char* path, CDataStore* p)
    {
        uint32_t row[4];
        for (uint32_t& v : row)
            v = Read<uint32_t>(p);
        AscDbc::Get(path).Upsert(row[0], std::vector<uint8_t>(reinterpret_cast<uint8_t*>(row), reinterpret_cast<uint8_t*>(row) + 16));
    }
    void __cdecl OnSpellSuggestion(void*, uint32_t, uint32_t, CDataStore* p)
    {
        UpsertSuggestionRow("DBFilesClient\\SpellSpellSuggestions.dbc", p);
        g_sugg.spellsDirty = true;
        g_sugg.dirty = true;
    }
    void __cdecl OnStatSuggestion(void*, uint32_t, uint32_t, CDataStore* p)
    {
        UpsertSuggestionRow("DBFilesClient\\SpellStatSuggestions.dbc", p);
        g_sugg.statsDirty = true;
        g_sugg.dirty = true;
    }

    // SMSG 0x64A (FUN_101dd650): one CharacterAdvancement.dbc row. The wire follows the row's field
    // order with each string field as a C string in place and the byte fields (+0xCC/+0xCD, +0xE0,
    // +0x128..+0x12C) as single bytes; the rest of those dwords is whatever the stack held (zero here).
    // An existing row is rewritten in place and re-indexed; a new one is inserted, the whole index
    // rebuilt and every browser filter reset.
    void __cdecl OnPatchEntry(void*, uint32_t, uint32_t, CDataStore* p)
    {
        AscDbc::Table& t = Rows();
        std::vector<uint8_t> row(0x1FC, 0);
        auto u32s = [&](uint32_t from, uint32_t to) {
            for (uint32_t off = from; off <= to; off += 4)
            {
                const uint32_t v = Read<uint32_t>(p);
                memcpy(&row[off], &v, 4);
            }
        };
        auto str = [&](uint32_t off) {
            const uint32_t v = t.AddString(ReadCString(p).c_str());
            memcpy(&row[off], &v, 4);
        };
        auto u8 = [&](uint32_t off) { row[off] = Read<uint8_t>(p); };
        u32s(0x00, 0x00);
        str(0x04);
        u32s(0x08, 0x3C);
        str(0x40);
        u32s(0x44, 0x4C);
        str(0x50);
        u32s(0x54, 0x5C);
        str(0x60);
        u32s(0x64, 0xB8);
        for (uint32_t off = 0xBC; off <= 0xC8; off += 4)
            str(off);
        u8(0xCC);
        u8(0xCD);
        u32s(0xD0, 0xDC);
        u8(0xE0);
        u32s(0xE4, 0x11C);
        str(0x120);
        u32s(0x124, 0x124);
        for (uint32_t off = 0x128; off <= 0x12C; ++off)
            u8(off);
        u32s(0x130, 0x1A0);
        for (uint32_t off = 0x1A4; off <= 0x1AC; off += 4)
            str(off);
        u32s(0x1B0, 0x1F8);

        uint32_t id, cls, tab;
        memcpy(&id, &row[0], 4);
        memcpy(&cls, &row[0x80], 4);
        memcpy(&tab, &row[0x84], 4);
        if (cls - 1 > 0x2E)
            cls = 1;
        if (tab - 1 > 0x5E)
            tab = 1;

        const bool inserted = !FindRow(id);
        if (!inserted)
        {
            Row r = FindRow(id);
            UnindexRow(r);                                            // FUN_1014a330(row, 0)
            UpdateAllPointers(true);
            memcpy(const_cast<uint8_t*>(r), row.data(), row.size());  // rewritten where it lies
            IndexRow(r);                                              // FUN_1014a330(row, 1)
            UpdateAllPointers(true);
            g_sugg.statsDirty = g_sugg.spellsDirty = g_sugg.dirty = true;   // FUN_103339e0/d0/f0(1)
        }
        else
        {
            t.Upsert(id, row);                                        // FUN_10133ac0
            BuildIndex();                                             // FUN_1014a530
            AscCAFilter::ResetEditor();                               // BuildCreator +0x228
            g_sugg.statsDirty = g_sugg.spellsDirty = g_sugg.dirty = true;
            AscCAFilter::ResetEntries();                              // mgr +0x2F8
            AscCAFilter::ResetCategories(false);                      // mgr +0x28
            AscCAFilter::ResetCategories(true);                       // mgr +0x190
            AscCAFilter::ResetEditor();                               // a second time, as the original does
        }
        Build* active = ActiveBuild();
        if (active->Has(id))   // FUN_10155060
            AscRuntime::Signal("ASCENSION_KNOWN_ENTRY_UPDATED", "%u%u", id, active->RankOf(id));
        else
            AscRuntime::Signal("ASCENSION_ENTRY_UPDATED", "%u", id);
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_ENTRY_PATCHED", "%b%s%s%u", inserted, kClassTypes[cls - 1],
                           kTabTypes[tab - 1], id);
    }

    // FUN_10333650: up to ten suggested entries the active build lacks, in weight order, limited to a
    // category (row +0x1C8..+0x1D0) unless it is 0.
    std::vector<uint32_t> SuggestedForCategory(uint32_t category)
    {
        std::vector<uint32_t> out;
        for (const auto& s : g_sugg.entries)
        {
            Row r = FindRow(s.first);
            if (!r)
                continue;
            bool inCategory = category == 0;
            for (uint32_t off = 0x1C8; off < 0x1D4 && !inCategory; off += 4)
                inCategory = RowU32(r, off) == category;
            if (!inCategory || ActiveBuild()->Has(RowU32(r, 0)))
                continue;
            out.push_back(s.first);
            if (out.size() >= 10)
                break;
        }
        return out;
    }

    // FUN_10333770: up to `max` suggested entries whose row is (or is not) an ability, optionally
    // skipping known ones.
    std::vector<uint32_t> SuggestedEntries(bool abilities, uint32_t max, bool skipKnown)
    {
        std::vector<uint32_t> out;
        for (const auto& s : g_sugg.entries)
        {
            Row r = FindRow(s.first);
            if (!r)
                continue;
            const int type = RowType(r);
            if ((type == 1 || type == 4) != abilities)   // FUN_101c6770
                continue;
            if (skipKnown && ActiveBuild()->Has(RowU32(r, 0)))
                continue;
            out.push_back(s.first);
            if (max <= out.size())
                break;
        }
        return out;
    }

    std::string StatName(uint32_t s) { return s < 6 ? kStats[s] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(s); }   // FUN_10169fc0

    // FUN_1017b0d0: (first stat, {stats}) with Strength, Agility, Intellect and Spirit appended when
    // missing; nil, nil before anything is suggested.
    int GetSuggestedStats(lua_State* L)
    {
        std::vector<uint32_t> stats;
        for (const auto& s : g_sugg.stats)   // FUN_10333590
            stats.push_back(s.first);
        if (stats.empty())
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 2;
        }
        for (uint32_t s : {0u, 1u, 3u, 4u})
            if (std::find(stats.begin(), stats.end(), s) == stats.end())
                stats.push_back(s);
        AscLua::lua_pushstring(L, StatName(stats[0]).c_str());
        AscLua::lua_createtable(L, 0, static_cast<int>(stats.size()));   // FUN_10169890
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < stats.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushstring(L, StatName(stats[i]).c_str());
            AscLua::lua_settable(L, -3);
        }
        return 2;
    }

    // ---- bindings -----------------------------------------------------------------------------------
    int UnlearnAllSpells(lua_State* L)  { Packet(0x528).Send(); PushBool(L, true); return 1; }
    int UnlearnAllTalents(lua_State* L) { Packet(0x213).Send(); PushBool(L, true); return 1; }

    int HasAnySuggestionContextOverrides(lua_State* L)
    {
        PushBool(L, !g_mgr.suggestionOverrides.empty());
        return 1;
    }

    int IsSuggestionContextOverride(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.IsSuggestionContextOverride(entryId)");
        auto& v = g_mgr.suggestionOverrides;
        PushBool(L, std::find(v.begin(), v.end(), id) != v.end());
        return 1;
    }

    int AddSuggestionContextOverride(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.AddSuggestionContextOverride(entryId)");
        auto& v = g_mgr.suggestionOverrides;
        if (std::find(v.begin(), v.end(), id) == v.end())
            v.push_back(id);
        g_sugg.dirty = true;   // FUN_103339f0(1)
        return 0;
    }

    int RemoveSuggestionContextOverride(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.RemoveSuggestionContextOverride(entryId)");
        auto& v = g_mgr.suggestionOverrides;
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
        g_sugg.dirty = true;   // FUN_103339f0(1)
        return 0;
    }

    int ClearSuggestionContextOverrides(lua_State*)
    {
        g_mgr.suggestionOverrides.clear();
        g_sugg.dirty = true;   // FUN_103339f0(1)
        return 0;
    }

    int IsPendingBuildAvailable(lua_State* L) { PushBool(L, g_mgr.pending != nullptr); return 1; }
    int IsActiveBuildAvailable(lua_State* L)  { PushBool(L, ActiveBuild() != nullptr); return 1; }

    int GetGlobalAEInvestment(lua_State* L) { PushInt(L, static_cast<int32_t>(ActiveBuild()->GlobalAE(0))); return 1; }
    int GetGlobalTEInvestment(lua_State* L) { PushInt(L, static_cast<int32_t>(ActiveBuild()->GlobalTE(0))); return 1; }
    int GetRemainingAE(lua_State* L)        { PushInt(L, static_cast<int32_t>(ActiveBuild()->RemainingAE())); return 1; }
    int GetRemainingTE(lua_State* L)        { PushInt(L, static_cast<int32_t>(ActiveBuild()->RemainingTE())); return 1; }

    int GetExpectedAE(lua_State* L)
    {
        uint32_t level;
        if (!ReadNumber(L, level))
            return Usage(L, "Usage: C_CharacterAdvancement.GetExpectedAE(level)");
        PushInt(L, static_cast<int32_t>(ActiveBuild()->AEBudget(level)));
        return 1;
    }

    int GetExpectedTE(lua_State* L)
    {
        uint32_t level;
        if (!ReadNumber(L, level))
            return Usage(L, "Usage: C_CharacterAdvancement.GetExpectedTE(level)");
        PushInt(L, static_cast<int32_t>(ActiveBuild()->TEBudget(level)));
        return 1;
    }

    int GetClassAEInvestment(lua_State* L)
    {
        uint32_t cls, minimum;
        if (!ClassMinArgs(L, cls, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetClassAEInvestment(classType, minInvestment)");
        PushInt(L, static_cast<int32_t>(ActiveBuild()->ClassAE(cls, minimum)));
        return 1;
    }

    int GetClassTEInvestment(lua_State* L)
    {
        uint32_t cls, minimum;
        if (!ClassMinArgs(L, cls, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetClassTEInvestment(classType, minInvestment)");
        PushInt(L, static_cast<int32_t>(ActiveBuild()->ClassTE(cls, minimum)));
        return 1;
    }

    int GetClassPointInvestment(lua_State* L)
    {
        uint32_t cls, minimum;
        if (!ClassMinArgs(L, cls, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetClassPointInvestment(classType, minInvestment)");
        PushInt(L, static_cast<int32_t>(ActiveBuild()->ClassPoints(cls, minimum)));
        return 1;
    }

    int GetTabAEInvestment(lua_State* L)
    {
        uint32_t cls, tab, minimum;
        if (!ClassTabMinArgs(L, cls, tab, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetTabAEInvestment(classType, tabType, minInvestment)");
        PushInt(L, static_cast<int32_t>(ActiveBuild()->TabAE(cls, tab, minimum)));
        return 1;
    }

    int GetTabTEInvestment(lua_State* L)
    {
        uint32_t cls, tab, minimum;
        if (!ClassTabMinArgs(L, cls, tab, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetTabTEInvestment(classType, tabType, minInvestment)");
        PushInt(L, static_cast<int32_t>(ActiveBuild()->TabTE(cls, tab, minimum)));
        return 1;
    }

    int GetActiveSpecID(lua_State* L)
    {
        Player* p = FindPlayer(ActivePlayerGuid());
        PushInt(L, p ? static_cast<int32_t>(p->spec + 1) : -1);
        return 1;
    }

    int IsLockedID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.IsLockedID(entryId)");
        PushBool(L, ActiveBuild()->LockedOf(id));
        return 1;
    }

    int GetEntryBySpellID(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.GetEntryBySpellID(spellId)");
        PushEntry(L, RowBySpell(spell));
        return 1;
    }

    int GetEntryByInternalID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.GetEntryByInternalID(id)");
        PushEntry(L, FindRow(id));
        return 1;
    }

    int GetInternalID(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.GetInternalID(spellId)");
        if (Row r = RowBySpell(spell))
            PushInt(L, static_cast<int32_t>(RowU32(r, 0)));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // Is*SpellID: the spell's entry type; nil when no entry lists the spell.
    int SpellIsType(lua_State* L, const char* usage, int type)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, usage);
        if (Row r = RowBySpell(spell))
            PushBool(L, RowType(r) == type);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // Is*ID: the entry's type; nil for an unknown entry.
    int EntryIsType(lua_State* L, const char* usage, int type)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, usage);
        if (Row r = FindRow(id))
            PushBool(L, RowType(r) == type);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int IsAbilitySpellID(lua_State* L)       { return SpellIsType(L, "Usage: C_CharacterAdvancement.IsAbilitySpellID(spellId)", 1); }
    int IsTalentSpellID(lua_State* L)        { return SpellIsType(L, "Usage: C_CharacterAdvancement.IsTalentSpellID(spellId)", 2); }
    int IsTalentAbilitySpellID(lua_State* L) { return SpellIsType(L, "Usage: C_CharacterAdvancement.IsTalentAbilitySpellID(spellId)", 4); }
    int IsAbilityID(lua_State* L)            { return EntryIsType(L, "Usage: C_CharacterAdvancement.IsAbilityID(entryId)", 1); }
    int IsTalentID(lua_State* L)             { return EntryIsType(L, "Usage: C_CharacterAdvancement.IsTalentID(entryId)", 2); }
    int IsTalentAbilityID(lua_State* L)      { return EntryIsType(L, "Usage: C_CharacterAdvancement.IsTalentAbilityID(entryId)", 4); }

    int IsMastery(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.IsMastery(spellId)");
        if (Row r = RowBySpell(spell))
            PushBool(L, RowHasFlag(r, 0x1000));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetTalentEssenceCost(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.GetTalentEssenceCost(spellId)");
        if (Row r = RowBySpell(spell))
            PushInt(L, static_cast<int32_t>(ActiveBuild()->TECost(r, 1)));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetAbilityEssenceCost(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.GetAbilityEssenceCost(spellId)");
        if (Row r = RowBySpell(spell))
            PushInt(L, static_cast<int32_t>(ActiveBuild()->AECost(r, 1)));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetClassName(lua_State* L)
    {
        uint32_t cls;
        if (!ReadNumber(L, cls))
            return Usage(L, "Usage: C_CharacterAdvancement.GetClassName(classType)");
        if (const uint8_t* ct = ClassTypes().Row(cls))
            PushStr(L, ClassTypes().Str(ct, 0x18));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetTabName(lua_State* L)
    {
        uint32_t tab;
        if (!ReadNumber(L, tab))
            return Usage(L, "Usage: C_CharacterAdvancement.GetTabName(tabType)");
        if (const uint8_t* tt = TabTypes().Row(tab))
            PushStr(L, TabTypes().Str(tt, 8));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }


    // ---- the pending build ----------------------------------------------------------------------
    Build* PendingOrError(lua_State* L, const char* name)
    {
        if (!g_mgr.pending)
        {
            const std::string msg = std::string(name) + ": pending build is not available";
            AscLua::luaL_error(L, msg.c_str());
        }
        return g_mgr.pending;
    }

    int GetPendingGlobalAEInvestment(lua_State* L)
    {
        Build* b = PendingOrError(L, "GetPendingGlobalAEInvestment");
        if (!b)
            return 0;
        PushInt(L, static_cast<int32_t>(b->GlobalAE(0)));
        return 1;
    }

    int GetPendingGlobalTEInvestment(lua_State* L)
    {
        Build* b = PendingOrError(L, "GetPendingGlobalTEInvestment");
        if (!b)
            return 0;
        PushInt(L, static_cast<int32_t>(b->GlobalTE(0)));
        return 1;
    }    int GetPendingRemainingAE(lua_State* L)
    {
        Build* b = PendingOrError(L, "GetPendingRemainingAE");
        if (!b)
            return 0;
        PushInt(L, static_cast<int32_t>(b->RemainingAE()));
        return 1;
    }
    int GetPendingRemainingTE(lua_State* L)
    {
        Build* b = PendingOrError(L, "GetPendingRemainingTE");
        if (!b)
            return 0;
        PushInt(L, static_cast<int32_t>(b->RemainingTE()));
        return 1;
    }

    int GetPendingExpectedAE(lua_State* L)
    {
        uint32_t level;
        if (!ReadNumber(L, level))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingExpectedAE(level)");
        Build* b = PendingOrError(L, "GetPendingExpectedAE");
        if (!b)
            return 0;
        PushInt(L, static_cast<int32_t>(b->AEBudget(level)));
        return 1;
    }

    int GetPendingExpectedTE(lua_State* L)
    {
        uint32_t level;
        if (!ReadNumber(L, level))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingExpectedTE(level)");
        Build* b = PendingOrError(L, "GetPendingExpectedTE");
        if (!b)
            return 0;
        PushInt(L, static_cast<int32_t>(b->TEBudget(level)));
        return 1;
    }

    // The per-class / per-tab pending sums return nothing (no error) without a pending build.
    int GetPendingClassAEInvestment(lua_State* L)
    {
        uint32_t cls, minimum;
        if (!ClassMinArgs(L, cls, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingClassAEInvestment(classType, minInvestment)");
        if (!g_mgr.pending)
            return 0;
        PushInt(L, static_cast<int32_t>(g_mgr.pending->ClassAE(cls, minimum)));
        return 1;
    }

    int GetPendingClassTEInvestment(lua_State* L)
    {
        uint32_t cls, minimum;
        if (!ClassMinArgs(L, cls, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingClassTEInvestment(classType, minInvestment)");
        if (!g_mgr.pending)
            return 0;
        PushInt(L, static_cast<int32_t>(g_mgr.pending->ClassTE(cls, minimum)));
        return 1;
    }

    int GetPendingClassPointInvestment(lua_State* L)
    {
        uint32_t cls, minimum;
        if (!ClassMinArgs(L, cls, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingClassPointInvestment(classType, minInvestment)");
        if (!g_mgr.pending)
            return 0;
        PushInt(L, static_cast<int32_t>(g_mgr.pending->ClassPoints(cls, minimum)));
        return 1;
    }

    int GetPendingTabTEInvestment(lua_State* L)   // FUN_10179be0
    {
        uint32_t cls, tab, minimum;
        if (!ClassTabMinArgs(L, cls, tab, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingTabTEInvestment(classType, tabType, minInvestment)");
        if (!g_mgr.pending)
            return 0;
        PushInt(L, static_cast<int32_t>(g_mgr.pending->TabTE(cls, tab, minimum)));
        return 1;
    }

    // FUN_10179d90: the quality the active build gives the spell's row and what it costs; nil without a row.
    int GetQualityInfo(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.GetQualityInfo(spellId)");
        Row r = RowBySpell(spell);
        if (!r)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const uint8_t q = static_cast<uint8_t>(QualityIndexOf(*ActiveBuild(), r));   // FUN_101539b0
        const std::string name = q < 9 ? std::string(kQualities[q]) : "UNEXPECTED_ENUM_VALUE_" + std::to_string(q);
        PushStr(L, name.c_str());
        PushInt(L, static_cast<int32_t>(QualityCostOf(*ActiveBuild(), r)));   // FUN_101539f0
        return 2;
    }

    // FUN_10176220: the active build as a build-creator link (FUN_100fc240 + FUN_100fd8f0).
    int ExportBuild(lua_State* L)
    {
        bool includeREs;
        if (!ReadBool(L, includeREs))
            return Usage(L, "Usage: C_CharacterAdvancement.ExportBuild(includeREs)");
        PushStr(L, AscBuildCreator::ExportBuildOf(*ActiveBuild(), includeREs).c_str());
        return 1;
    }

    int GetPendingTabAEInvestment(lua_State* L)
    {
        uint32_t cls, tab, minimum;
        if (!ClassTabMinArgs(L, cls, tab, minimum))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingTabAEInvestment(classType, tabType, minInvestment)");
        if (!g_mgr.pending)
            return 0;
        PushInt(L, static_cast<int32_t>(g_mgr.pending->TabAE(cls, tab, minimum)));
        return 1;
    }

    // A learn/unlearn request in flight (FUN_10a31720 / FUN_10a31bb0 on 0x10d3d0d8 +0x198).
    const Request* InFlight(uint32_t id)
    {
        for (const Request& r : g_requests)
            if (r.id == id)
                return &r;
        return nullptr;
    }

    int IsPendingEntryID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.IsPendingEntryID(entryId)");
        Build* b = PendingOrError(L, "IsPendingEntryID");
        if (!b)
            return 0;
        if (InFlight(id))
        {
            PushBool(L, false);
            return 1;
        }
        PushBool(L, b->RankOf(id) != ActiveBuild()->RankOf(id));
        return 1;
    }

    int GetPendingRankByEntryID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.GetPendingRankByEntryID(entryId)");
        Build* b = PendingOrError(L, "GetPendingRankByEntryID");
        if (!b)
            return 0;
        const Request* q = InFlight(id);
        PushInt(L, static_cast<int32_t>(q ? q->rank : b->RankOf(id)));
        if (Row r = FindRow(id))
            PushInt(L, static_cast<int32_t>(MaxRank(r)));
        else
            AscLua::lua_pushnil(L);
        return 2;
    }

    int GetLowestInvestmentRequired(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, STRING}))
            return Usage(L, "Usage: C_CharacterAdvancement.GetLowestInvestmentRequired(classType, tabType)");
        const uint32_t cls = EnumArg(L, 1, kClassTypes, 0x2F), tab = EnumArg(L, 2, kTabTypes, 0x5F);
        Build* b = PendingOrError(L, "GetLowestInvestmentRequired");
        if (!b)
            return 0;
        uint32_t ae = 0, te = 0;
        for (const Entry& e : b->entries)
            if (RowU32(e.row, 0x80) == cls && RowU32(e.row, 0x84) == tab)
            {
                ae = std::max(ae, RowU32(e.row, 0x98));
                te = std::max(te, RowU32(e.row, 0x9C));
            }
        PushInt(L, static_cast<int32_t>(ae));
        PushInt(L, static_cast<int32_t>(te));
        return 2;
    }

    // FUN_1016f480: the first ChrSpecs.dbc row whose entry (+0x70) the pending build has.
    int GetActiveChrSpec(lua_State* L)
    {
        if (Build* b = g_mgr.pending)
        {
            AscDbc::Table& t = ChrSpecs();
            for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
                if (Row r = t.Row(id))
                    if (b->Has(RowU32(r, 0x70)) && RowU32(r, 0) != 0)
                    {
                        PushInt(L, static_cast<int32_t>(RowU32(r, 0)));
                        return 1;
                    }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }


    // ---- pending build edits ------------------------------------------------------------------------
    int CancelPendingBuild(lua_State*)
    {
        ResetPendingBuild();
        return 0;
    }

    // Clear (FUN_10175ef0) / ClearByTab (FUN_10176030): remove from a snapshot of the entries; with
    // forReset, rows flagged 2 stay.
    int ClearPendingBuild(lua_State* L)
    {
        bool forReset;
        if (!ReadBool(L, forReset))
            return Usage(L, "Usage: C_CharacterAdvancement.ClearPendingBuild(forReset)");
        Build* b = PendingOrError(L, "ClearPendingBuild");
        if (!b)
            return 0;
        const std::vector<Entry> snapshot = b->entries;
        for (const Entry& e : snapshot)
            if (RowU32(e.row, 0x74) != 1 && (!forReset || !RowHasFlag(e.row, 2)))
                b->Remove(e.id);
        // A reset is not an unlearn: it puts the build back to what the archetype grants, and the
        // rows the client's own rule hands out at this level - the free 0x100000 passives, which no
        // flag keeps through the clear - are part of that state. `Build::Remove` deliberately does
        // not ask the pass for a row it removed (that is what keeps an unlearn a choice the player
        // made), so the reset has to ask here. Without it the passives sit at 0/1: the window draws
        // them as learnable, they can be clicked as if they were still to be bought, and the save
        // that follows bills their removal as an unlearn the player never asked for.
        if (forReset)
            RunAutoLearnPass();
        CoADbg("clear forReset=%d pending=%u [%s]", forReset ? 1 : 0, unsigned(b->entries.size()),
               CoADbgEntries(b).c_str());
        BumpPendingVersion();
        return 0;
    }

    int ClearPendingBuildByTab(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, STRING, AscScript::BOOLEAN}))
            return Usage(L, "Usage: C_CharacterAdvancement.ClearPendingBuildByTab(classType, tabType, forReset)");
        const uint32_t cls = EnumArg(L, 1, kClassTypes, 0x2F), tab = EnumArg(L, 2, kTabTypes, 0x5F);
        const bool forReset = AscLua::lua_toboolean(L, 3) != 0;
        Build* b = PendingOrError(L, "ClearPendingBuildByTab");
        if (!b)
            return 0;
        const std::vector<Entry> snapshot = b->entries;
        for (const Entry& e : snapshot)
            if (RowU32(e.row, 0x80) == cls && RowU32(e.row, 0x84) == tab && (!forReset || !RowHasFlag(e.row, 2)))
                b->Remove(e.id);
        // A tree reset is a reset, not an unlearn: the rows the client's own rule grants at this
        // level come back with it. See `ClearPendingBuild` for why the pass has to be asked here.
        if (forReset)
            RunAutoLearnPass();
        CoADbg("clearTab cls=%u tab=%u forReset=%d pending=%u [%s]", cls, tab, forReset ? 1 : 0,
               unsigned(b->entries.size()), CoADbgEntries(b).c_str());
        BumpPendingVersion();
        return 0;
    }

    bool HasMasteries(Row r)
    {
        for (uint32_t i = 0; i < 3; ++i)
            if (RowU32(r, 0x1B0 + i * 4))
                return true;
        return false;
    }

    void PushLearnResult(lua_State* L, uint32_t result)   // (ok, nil) or (false, CA_LEARN_* name)
    {
        PushBool(L, result == 0);
        if (result == 0)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, result < 0x2E ? kLearnResults[result] : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(result)).c_str());
    }

    // FUN_1016d220: add `ranks` ranks, all-or-nothing when more than one.
    void AddRanks(Build& pending, uint32_t id, uint32_t ranks)
    {
        if (ranks > 1)
        {
            Build copy(pending, true);
            for (uint32_t i = 0; i < ranks; ++i)
            {
                if (copy.ValidateLearn(nullptr, id, {}, {}) != 0)
                    return;
                copy.AddRank(id);
            }
            for (uint32_t i = 0; i < ranks; ++i)
                if (pending.ValidateLearn(nullptr, id, {}, {}) == 0)
                    pending.AddRank(id);
        }
        else if (pending.ValidateLearn(nullptr, id, {}, {}) == 0)
            pending.AddRank(id);
        pending.Reorder(false);
        BumpPendingVersion();
    }

    // FUN_1016da10: an entry with masteries may need one of them first.
    void AddRanksWithMastery(Build& pending, uint32_t id, uint32_t ranks, Row r)
    {
        Build copy(pending, true);
        uint32_t mastery = 0;
        if (copy.ValidateLearn(nullptr, id, {}, {}) != 0)
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t m = RowU32(r, 0x1B0 + i * 4);
                if (m && copy.ValidateLearn(nullptr, m, {}, {}) == 0)
                {
                    mastery = m;
                    copy.AddRank(m);
                    break;
                }
            }
        for (uint32_t i = 0; i < ranks; ++i)
        {
            if (copy.ValidateLearn(nullptr, id, {}, {}) != 0)
                return;
            copy.AddRank(id);
        }
        if (mastery)
            pending.AddRank(mastery);
        for (uint32_t i = 0; i < ranks; ++i)
            if (pending.ValidateLearn(nullptr, id, {}, {}) == 0)
                pending.AddRank(id);
        BumpPendingVersion();
    }

    int AddByEntryID(lua_State* L)
    {
        int32_t id, ranks;
        if (!ReadInt2(L, id, ranks))
            return Usage(L, "Usage: C_CharacterAdvancement.AddByEntryID(entryId, numRanks)");
        Build* b = PendingOrError(L, "AddByEntryID");
        if (!b)
            return 0;
        Row r = FindRow(static_cast<uint32_t>(id));
        if (!r)
            return Usage(L, "AddByEntryID: invalid entryId");
        if (HasMasteries(r))
            AddRanksWithMastery(*b, static_cast<uint32_t>(id), static_cast<uint32_t>(ranks), r);
        else
            AddRanks(*b, static_cast<uint32_t>(id), static_cast<uint32_t>(ranks));
        return 0;
    }

    // FUN_1016e050: one rank validates on the pending build itself (MeetsInvestment... passes only the
    // BAD_ABILITY and NOT_ENOUGH_INVESTED_* slots); several ranks validate one by one on a copy.
    int CanAddRanks(lua_State* L, Build& pending, uint32_t id, uint32_t ranks, bool investmentOnly)
    {
        if (ranks < 2)
        {
            static const std::vector<uint32_t> investment = {3, 0x26, 0x27, 0x28};
            PushLearnResult(L, pending.ValidateLearn(nullptr, id, investmentOnly ? investment : std::vector<uint32_t>{}, {}));
            return 2;
        }
        Build copy(pending, true);
        for (uint32_t i = 0; i < ranks; ++i)
        {
            const uint32_t result = copy.ValidateLearn(nullptr, id, {}, {});
            if (result)
            {
                PushLearnResult(L, result);
                return 2;
            }
            copy.AddRank(id);
        }
        PushLearnResult(L, 0);
        return 2;
    }

    // FUN_1016e400: with masteries, on a copy that first takes a mastery when the entry needs one.
    int CanAddRanksWithMastery(lua_State* L, Build& pending, uint32_t id, uint32_t ranks, Row r)
    {
        Build copy(pending, true);
        if (copy.ValidateLearn(nullptr, id, {}, {}) != 0)
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t m = RowU32(r, 0x1B0 + i * 4);
                if (m && copy.ValidateLearn(nullptr, m, {}, {}) == 0)
                {
                    copy.AddRank(m);
                    break;
                }
            }
        for (uint32_t i = 0; i < ranks; ++i)
        {
            const uint32_t result = copy.ValidateLearn(nullptr, id, {}, {});
            if (result)
            {
                PushLearnResult(L, result);
                return 2;
            }
            copy.AddRank(id);
        }
        PushLearnResult(L, 0);
        return 2;
    }

    int CanAddByEntryID(lua_State* L)
    {
        int32_t id, ranks;
        if (!ReadInt2(L, id, ranks))
            return Usage(L, "Usage: C_CharacterAdvancement.CanAddByEntryID(entryId, numRanks)");
        Build* b = PendingOrError(L, "CanAddByEntryID");
        if (!b)
            return 0;
        Row r = FindRow(static_cast<uint32_t>(id));
        if (!r)
            return Usage(L, "CanAddByEntryID: invalid entryId");
        if (HasMasteries(r))
            return CanAddRanksWithMastery(L, *b, static_cast<uint32_t>(id), static_cast<uint32_t>(ranks), r);
        return CanAddRanks(L, *b, static_cast<uint32_t>(id), static_cast<uint32_t>(ranks), false);
    }

    int MeetsInvestmentForAddByEntryID(lua_State* L)
    {
        int32_t id, ranks;
        if (!ReadInt2(L, id, ranks))
            return Usage(L, "Usage: C_CharacterAdvancement.MeetsInvestmentForAddByEntryID(entryId, numRanks)");
        Build* b = PendingOrError(L, "MeetsInvestmentForAddByEntryID");
        if (!b)
            return 0;
        if (!FindRow(static_cast<uint32_t>(id)))
            return Usage(L, "MeetsInvestmentForAddByEntryID: invalid entryId");
        return CanAddRanks(L, *b, static_cast<uint32_t>(id), static_cast<uint32_t>(ranks), true);
    }

    // FUN_101523d0: pending vs active -- {id, rank delta, added, removed, modified}.
    struct Diff { uint32_t id; int32_t delta; bool added, removed, modified; };
    std::vector<Diff> PendingDiff(const Build& pending, const Build& active)
    {
        std::vector<Diff> out;
        for (const Entry& e : pending.entries)
            if (!active.Has(e.id))
                out.push_back({e.id, static_cast<int32_t>(e.rank), true, false, false});
        for (const Entry& e : active.entries)
            if (!pending.Has(e.id))
                out.push_back({e.id, -static_cast<int32_t>(e.rank), false, true, false});
        for (const Entry& e : pending.entries)
        {
            bool listed = false;
            for (const Diff& d : out)
                listed = listed || d.id == e.id;
            if (listed)
                continue;
            const int32_t delta = static_cast<int32_t>(e.rank) - static_cast<int32_t>(active.RankOf(e.id));
            if (delta)
                out.push_back({e.id, delta, false, false, true});
        }
        return out;
    }

    int IsPending(lua_State* L)
    {
        Build* b = PendingOrError(L, "IsPending");
        if (!b)
            return 0;
        PushBool(L, !PendingDiff(*b, *ActiveBuild()).empty());
        return 1;
    }

    int GetPendingSummary(lua_State* L)
    {
        Build* b = PendingOrError(L, "GetPendingSummary");
        if (!b)
            return 0;
        const std::vector<Diff> diff = PendingDiff(*b, *ActiveBuild());
        if (diff.empty())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        // The original writes every diff's fields into the one table (no per-entry subtables).
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const Diff& d : diff)
        {
            SetInt(L, "Entry", static_cast<int32_t>(d.id));
            SetInt(L, "CARankDiff", d.delta);
            AscLua::lua_pushstring(L, "Added");    AscLua::lua_pushboolean(L, d.added);    AscLua::lua_settable(L, -3);
            AscLua::lua_pushstring(L, "Removed");  AscLua::lua_pushboolean(L, d.removed);  AscLua::lua_settable(L, -3);
            AscLua::lua_pushstring(L, "Modified"); AscLua::lua_pushboolean(L, d.modified); AscLua::lua_settable(L, -3);
        }
        return 1;
    }


    // ---- remove / apply -------------------------------------------------------------------------------
    // FUN_10168f80 / FUN_100999d0: CA_UPDATE_ENTRIES_* and CA_UNLEARN_* names.
    std::string UpdateName(uint32_t r)
    {
        return r < 9 ? kUpdateResults[r] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(r);
    }
    std::string LearnName(uint32_t r)
    {
        return r < 0x2E ? kLearnResults[r] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(r);
    }
    std::string UnlearnName(uint32_t r)
    {
        return r < 0x17 ? kUnlearnResults[r] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(r);
    }

    // The mastery a removal is really about: the entry itself when it is one, else the first of its
    // masteries the build has; 0 when neither.
    uint32_t MasteryTarget(Build& b, Row r)
    {
        if (RowHasFlag(r, 0x1000))
            return RowU32(r, 0);
        for (uint32_t i = 0; i < 3; ++i)
            if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
                if (b.Has(m))
                    return m;
        return 0;
    }

    // (ok) then, on failure, (updateResult, learnResult, id, rank); nils when ok.
    void PushApplyResult(lua_State* L, uint32_t result, uint32_t learnResult, uint32_t id, uint32_t rank)
    {
        PushBool(L, result == 0);
        if (result == 0)
        {
            PushNils(L, 4);
            return;
        }
        PushStr(L, UpdateName(result).c_str());
        PushStr(L, LearnName(learnResult).c_str());
        PushInt(L, static_cast<int32_t>(id));
        PushInt(L, static_cast<int32_t>(rank));
    }

    int RemoveByEntryID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.RemoveByEntryID(entryId)");
        Build* b = PendingOrError(L, "RemoveByEntryID");
        if (!b)
            return 0;
        Row r = FindRow(id);
        if (!r)
            return Usage(L, "RemoveByEntryID: invalid entryId");
        if (const uint32_t m = MasteryTarget(*b, r))
        {
            // FUN_10173b10: only when the whole transition would apply.
            Build copy(*b, true);
            copy.RemoveWithMastery(m);
            uint32_t lr, eid, er;
            ApplyCosts costs;
            if (ValidateApply(ActivePlayer(), *b, copy.entries, lr, eid, er, costs) == 0)
            {
                b->RemoveWithMastery(m);
                b->Reorder(false);
                BumpPendingVersion();
            }
            return 0;
        }
        if (b->ValidateUnlearn(nullptr, id, {}, {}) != 0)
            return 0;
        b->Remove(id);
        b->Reorder(false);
        BumpPendingVersion();
        return 0;
    }

    int CanRemoveByEntryID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.CanRemoveByEntryID(entryId)");
        Build* b = PendingOrError(L, "CanRemoveByEntryID");
        if (!b)
            return 0;
        Row r = FindRow(id);
        if (!r)
            return Usage(L, "CanRemoveByEntryID: invalid entryId");
        if (const uint32_t m = MasteryTarget(*b, r))
        {
            // FUN_1016e9f0: the transition's verdict, 5 values.
            Build copy(*b, true);
            copy.RemoveWithMastery(m);
            uint32_t lr, eid, er;
            ApplyCosts costs;
            // Separate statement: as one call's arguments, MSVC copies lr/eid/er (right to left) before
            // ValidateApply has written them.
            const uint32_t result = ValidateApply(ActivePlayer(), *b, copy.entries, lr, eid, er, costs);
            PushApplyResult(L, result, lr, eid, er);
            return 5;
        }
        // FUN_1016e820: the unlearn rules, NOT_WILDCARD excluded.
        const uint32_t result = b->ValidateUnlearn(nullptr, id, {}, {0x15});
        PushBool(L, result == 0);
        if (result == 0)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, UnlearnName(result).c_str());
        return 2;
    }

    // Marks, money, then {MoneyCost, MarkOfAscensionCost, TokenEntry, TokenCost} per token cost.
    void PushCosts(lua_State* L, const ApplyCosts& costs)
    {
        PushInt(L, static_cast<int32_t>(costs.marks));
        PushInt(L, static_cast<int32_t>(costs.money));
        AscLua::lua_createtable(L, 0, static_cast<int>(costs.tokens.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < costs.tokens.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_createtable(L, 0, 4);
            AscLua::lua_checkstack(L, 2);
            SetInt(L, "MoneyCost", static_cast<int32_t>(costs.tokens[i].money));
            SetInt(L, "MarkOfAscensionCost", static_cast<int32_t>(costs.tokens[i].marks));
            SetInt(L, "TokenEntry", static_cast<int32_t>(costs.tokens[i].item));
            SetInt(L, "TokenCost", static_cast<int32_t>(costs.tokens[i].count));
            AscLua::lua_settable(L, -3);
        }
    }

    // The coinage reader is defined with the CA cost helpers it belongs to, far below; the quote's
    // affordability check in `CanApplyPendingBuild` is the first call this far up the file.
    uint32_t Coinage(const uint8_t* unit);

    int CanApplyPendingBuild(lua_State* L)
    {
        Build* b = PendingOrError(L, "CanApplyPendingBuild");
        if (!b)
            return 0;
        uint32_t lr, id, rank;
        ApplyCosts costs;
        uint32_t result = ValidateApply(ActivePlayer(), *ActiveBuild(), b->entries, lr, id, rank, costs);
        /* A switch to another archetype is priced by the realm, whose quote arrived with the preview
         * that staged it (see `g_quoteSpec`): the confirmation the player accepts and the fee the
         * realm takes are one number, whatever the realm's policy is. Ranks inside the archetype's own
         * tree and a reset are priced here, and the realm prices those with the same rule. */
        if (g_quoteSpec)
        {
            costs.marks = g_quoteMarks;
            costs.money = g_quoteCopper;
            costs.tokens.clear();
            /* A price the window can see is a price the window can refuse: the realm would refuse the
             * same switch, so it is refused here rather than offered and then rejected. The quote is
             * the realm's own bill, so covering both of its amounts is exactly what the realm asks;
             * CA_UPDATE_ENTRIES_MISSING_TOKENS is the code the window's own cost check answers with. */
            const uint8_t* unit = static_cast<const uint8_t*>(ActivePlayer());
            if (result == 0 && ((costs.marks && UnitItemCount(unit, kMarkOfAscensionItem) < costs.marks) ||
                                (costs.money && Coinage(unit) < costs.money)))
                result = 7;
            CoADbg("quote applied on the switch to %u: marks=%u money=%u result=%u", g_quoteSpec,
                   costs.marks, costs.money, result);
        }
        PushApplyResult(L, result, lr, id, rank);
        PushCosts(L, costs);
        // TEMPORARY, part of the browse/preview diagnostic: the verdict and the bill the Save button,
        // the cost popup and `CONFIRM_RESET_BUILD` are all drawn from this call, so it is logged (only
        // when something about it changes, to keep a session's log readable).
        {
            static uint32_t lastResult = 0xFFFFFFFFu, lastLearn = 0xFFFFFFFFu, lastMarks = 0xFFFFFFFFu;
            static uint32_t lastMoney = 0xFFFFFFFFu, lastTokens = 0xFFFFFFFFu;
            static size_t lastPending = size_t(-1), lastActive = size_t(-1);
            const size_t pendingRows = b->entries.size();
            const Build* active = ActiveBuild();
            const size_t activeRows = active ? active->entries.size() : 0;
            if (result != lastResult || lr != lastLearn || costs.marks != lastMarks || costs.money != lastMoney ||
                costs.tokens.size() != lastTokens || pendingRows != lastPending || activeRows != lastActive)
            {
                lastResult = result; lastLearn = lr; lastMarks = costs.marks;
                lastMoney = costs.money; lastTokens = uint32_t(costs.tokens.size());
                lastPending = pendingRows; lastActive = activeRows;
                CoADbg("canApply result=%u reason=%s learnResult=%s entry=%u rank=%u marks=%u money=%u tokens=%u "
                       "pending=%u active=%u",
                       result, result ? UpdateName(result).c_str() : "ok",
                       lr ? LearnName(lr).c_str() : "ok", id, rank, costs.marks, costs.money,
                       unsigned(costs.tokens.size()), unsigned(pendingRows), unsigned(activeRows));
                // TEMPORARY, same diagnostic: a refusal names the row it stopped on, and the build
                // it stopped on is what says whether the window is holding something it should not.
                if (result)
                    CoADbg("canApply refused on [%s]", CoADbgEntries(b).c_str());
            }
        }
        return 8;
    }

    int CanClearPendingBuild(lua_State* L)
    {
        bool forReset;
        if (!ReadBool(L, forReset))
            return Usage(L, "Usage: C_CharacterAdvancement.CanClearPendingBuild(forReset)");
        Build* b = PendingOrError(L, "CanClearPendingBuild");
        if (!b)
            return 0;
        Build copy(*b, true);
        const std::vector<Entry> snapshot = copy.entries;
        for (const Entry& e : snapshot)
            if (RowU32(e.row, 0x74) != 1 && (!forReset || !RowHasFlag(e.row, 2)))
                copy.Remove(e.id);
        uint32_t lr, id, rank;
        ApplyCosts costs;
        const uint32_t result = ValidateApply(ActivePlayer(), *b, copy.entries, lr, id, rank, costs);
        PushApplyResult(L, result, lr, id, rank);
        return 5;
    }

    // ApplyPendingBuild (FUN_101742c0): CMSG 0x727 = u32 count, then per entry u32 id, u32 rank, u32 +0xC,
    // u8 +0x10, u32 +0x18, u32 +0x1C (FUN_10166a50).
    int ApplyPendingBuild(lua_State* L)
    {
        Build* b = PendingOrError(L, "ApplyPendingBuild");
        if (!b)
            return 0;
        uint32_t lr, id, rank;
        ApplyCosts costs;
        if (ValidateApply(ActivePlayer(), *ActiveBuild(), b->entries, lr, id, rank, costs) != 0)
            return Usage(L, "ApplyPendingBuild: pending build cannot be applied");
        CoADbg("apply pending=%u marks=%u money=%u tokens=%u [%s]", unsigned(b->entries.size()), costs.marks,
               costs.money, unsigned(costs.tokens.size()), CoADbgEntries(b).c_str());
        Packet pk(0x727);
        pk.U32(static_cast<uint32_t>(b->entries.size()));
        for (const Entry& e : b->entries)
            pk.U32(e.id).U32(e.rank).U32(e.u0c).U8(e.locked).U32(e.u18).U32(e.u1c);
        pk.Send();
        return 0;
    }


    // ---- entry lists -------------------------------------------------------------------------------
    // FUN_101c67c0: not on this realm, or (unless skipped) no player / the player's class can't use it.
    bool HiddenBase(Row r, bool skipClass)
    {
        if (!Visible(r))
            return true;
        if (skipClass)
            return false;
        const uint8_t* player = ActivePlayer();
        return !player || !ClassTypeAdmits(r, UnitClass(player));
    }

    // FUN_101c6bd0: also hidden while the pending build lacks a 0x80000 row, or outside the mode a
    // mode-restricted row belongs to.
    bool Hidden(Row r, bool skipClass)
    {
        if (HiddenBase(r, skipClass))
            return true;
        const uint32_t flags = RowU32(r, 0x124);
        if ((flags >> 19) & 1)
            if (!g_mgr.pending || g_mgr.pending->RankOf(RowU32(r, 0)) == 0)
                return true;
        if (!(flags & 0x1800000))
            return false;
        const Build* p = g_mgr.pending;
        bool allowed = (flags & 0x800000) && p && p->wildcard;
        if ((flags & 0x1000000) && p && p->draft)
            allowed = true;
        return !allowed;
    }

    uint32_t ActiveRequiredLevel(Row r)   // FUN_10153af0 on the active build
    {
        const Build* b = ActiveBuild();
        return b->draft ? RowU32(r, 0x6C) : b->wildcard ? RowU32(r, 0x70) : RowU32(r, 0x68);
    }

    // FUN_101802e0: required level, then the investment requirements, then +0x1D4, then id.
    bool EntryOrder(Row a, Row b)
    {
        const uint32_t la = ActiveRequiredLevel(a), lb = ActiveRequiredLevel(b);
        if (la != lb)
            return la < lb;
        for (uint32_t off : {0xA4u, 0x88u, 0x8Cu, 0x90u, 0x94u, 0x98u, 0x9Cu, 0x1D4u})
            if (RowU32(a, off) != RowU32(b, off))
                return RowU32(a, off) < RowU32(b, off);
        return RowU32(a, 0) < RowU32(b, 0);
    }

    // FUN_101801d0 (talents): required level, then id.
    bool TalentOrder(Row a, Row b)
    {
        const uint32_t la = ActiveRequiredLevel(a), lb = ActiveRequiredLevel(b);
        if (la != lb)
            return la < lb;
        return RowU32(a, 0) < RowU32(b, 0);
    }

    // FUN_10172df0: {[i] = entry table (or nil)}.
    void PushEntryList(lua_State* L, const std::vector<Row>& rows)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < rows.size(); ++i)
        {
            PushInt(L, static_cast<int32_t>(i + 1));
            PushEntry(L, rows[i]);
            AscLua::lua_settable(L, -3);
        }
    }

    // FUN_10168b90 / FUN_10168e40: a class / tab name as its enum value; an unknown name is logged.
    uint32_t EnumByName(const char* name, const char* const* names, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(name, names[i]) == 0)
                return i + 1;
        AscLog::Printf("Unexpected Value: %s", name);
        return 0;
    }

    // FUN_101494b0 (tab 1 = every tab) / FUN_101495a0: realm-visible rows of the class (and tab).
    std::vector<Row> ClassRows(uint32_t cls, uint32_t tab)
    {
        std::vector<Row> rows;
        ForEachRow([&](Row r) {
            if (Visible(r) && RowU32(r, 0x80) == cls && (tab == 1 || RowU32(r, 0x84) == tab))
                rows.push_back(r);
        });
        return rows;
    }

    int ListByClass(lua_State* L, bool withFlag, const char* usage, bool (*keep)(Row, bool), bool (*order)(Row, Row))
    {
        if (!ValidateInput(L, withFlag ? std::initializer_list<int>{STRING, STRING, AscScript::BOOLEAN}
                                       : std::initializer_list<int>{STRING, STRING}))
            return Usage(L, usage);
        const uint32_t cls = EnumByName(CheckString(L, 1), kClassTypes, 0x2F);
        const uint32_t tab = EnumByName(CheckString(L, 2), kTabTypes, 0x5F);
        const bool withMasteries = withFlag && AscLua::lua_toboolean(L, 3) != 0;
        std::vector<Row> rows = ClassRows(cls, tab);
        rows.erase(std::remove_if(rows.begin(), rows.end(), [&](Row r) { return !keep(r, withMasteries); }), rows.end());
        std::sort(rows.begin(), rows.end(), order);
        PushEntryList(L, rows);
        return 1;
    }

    bool KeepEntry(Row r, bool withMasteries)
    {
        return !Hidden(r, true) && (withMasteries || !RowHasFlag(r, 0x1000));
    }
    bool KeepSpell(Row r, bool withMasteries) { return RowType(r) == 1 && KeepEntry(r, withMasteries); }
    bool KeepTalent(Row r, bool withMasteries)
    {
        const int t = RowType(r);
        return !RowHasFlag(r, 0x400000) && (t == 2 || t == 4) && KeepEntry(r, withMasteries);
    }
    bool KeepMastery(Row r, bool) { return RowHasFlag(r, 0x1000) && !Hidden(r, true); }
    bool KeepImplicit(Row r, bool) { return RowHasFlag(r, 0x400000) && !Hidden(r, true); }

    int GetEntriesByClass(lua_State* L)
    {
        return ListByClass(L, true, "Usage: C_CharacterAdvancement.GetEntriesByClass(className, tabName, withMasteries)", KeepEntry, EntryOrder);
    }
    int GetSpellsByClass(lua_State* L)
    {
        return ListByClass(L, true, "Usage: C_CharacterAdvancement.GetSpellsByClass(className, tabName, withMasteries)", KeepSpell, EntryOrder);
    }
    int GetTalentsByClass(lua_State* L)
    {
        return ListByClass(L, true, "Usage: C_CharacterAdvancement.GetTalentsByClass(className, tabName, withMasteries)", KeepTalent, TalentOrder);
    }
    int GetMasteriesByClass(lua_State* L)
    {
        return ListByClass(L, false, "Usage: C_CharacterAdvancement.GetMasteriesByClass(className, tabName)", KeepMastery, EntryOrder);
    }
    int GetImplicitByClass(lua_State* L)
    {
        return ListByClass(L, false, "Usage: C_CharacterAdvancement.GetImplicitByClass(className, tabName)", KeepImplicit, EntryOrder);
    }

    // FUN_1016f5b0: every row not hidden (class check included), in id order.
    int GetAllEntries(lua_State* L)
    {
        std::vector<Row> rows;
        ForEachRow([&](Row r) {
            if (!Hidden(r, false))
                rows.push_back(r);
        });
        PushEntryList(L, rows);
        return 1;
    }


    // ---- known entries and spells ---------------------------------------------------------------------
    int IsSpellIDKnown(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.IsSpellIDKnown(spellId)");
        PushBool(L, g_knownSpells.count(spell) != 0);
        return 1;
    }

    int IsKnownID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.IsKnownID(entryId)");
        if (InFlight(id))
        {
            PushBool(L, false);
            return 1;
        }
        if (Row r = FindRow(id))
            if (RowHasFlag(r, 0x20000) && ChainKnown(RowU32(r, 0x14)))
            {
                PushBool(L, true);
                return 1;
            }
        PushBool(L, ActiveBuild()->Has(id));
        return 1;
    }

    int IsKnownSpellID(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.IsKnownSpellID(spellId)");
        Row r = RowBySpell(spell);
        if (!r)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        if (InFlight(RowU32(r, 0)))
        {
            PushBool(L, false);
            return 1;
        }
        if (RowHasFlag(r, 0x20000) && ChainKnown(RowU32(r, 0x14)))
        {
            PushBool(L, true);
            return 1;
        }
        PushBool(L, AscSpellRank::RankNumber(spell) <= ActiveBuild()->RankOf(RowU32(r, 0)));
        return 1;
    }

    // FUN_10165e80: (unitToken, entryId, specialization) -> the unit's build for that spec, or why not.
    enum UnitBuildResult { UB_OK, UB_USAGE, UB_SPEC, UB_TOKEN, UB_NIL };
    UnitBuildResult UnitBuildArgs(lua_State* L, uint32_t& id, Build*& b)
    {
        b = nullptr;
        if (!ValidateInput(L, {STRING, NUMBER, NUMBER}))
            return UB_USAGE;
        const std::string token = CheckString(L, 1);
        id = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const uint32_t spec = static_cast<uint32_t>(ToInt(CheckNumber(L, 3)));
        if (spec == 0)
            return UB_SPEC;
        uint64_t guid;
        if (!UnitTokenGuid(token.c_str(), guid))
            return UB_TOKEN;
        Player* p = FindPlayer(guid);
        b = p ? p->Get(spec - 1) : nullptr;
        return b ? UB_OK : UB_NIL;
    }

    int UnitKnownID(lua_State* L)
    {
        uint32_t id;
        Build* b;
        switch (UnitBuildArgs(L, id, b))
        {
        case UB_USAGE: return Usage(L, "Usage: C_CharacterAdvancement.UnitKnownID(unitToken, entryId, specialization)");
        case UB_SPEC: return Usage(L, "UnitKnownID: specialization must be >= 1");
        case UB_TOKEN: return Usage(L, "UnitKnownID: invalid unit token");
        case UB_NIL: AscLua::lua_pushnil(L); return 1;
        default: PushBool(L, b->Has(id)); return 1;
        }
    }

    int UnitTalentRankByID(lua_State* L)
    {
        uint32_t id;
        Build* b;
        switch (UnitBuildArgs(L, id, b))
        {
        case UB_USAGE: return Usage(L, "Usage: C_CharacterAdvancement.UnitTalentRankByID(unitToken, entryId, specialization)");
        case UB_SPEC: return Usage(L, "UnitTalentRankByID: specialization must be >= 1");
        case UB_TOKEN: return Usage(L, "UnitTalentRankByID: invalid unit token");
        case UB_NIL: AscLua::lua_pushnil(L); return 1;
        default: break;
        }
        if (const uint32_t rank = b->RankOf(id))
            PushInt(L, static_cast<int32_t>(rank));
        else
            AscLua::lua_pushnil(L);
        if (Row r = FindRow(id))
            PushInt(L, static_cast<int32_t>(MaxRank(r)));
        else
            AscLua::lua_pushnil(L);
        return 2;
    }

    // (rank, max rank) with a talent's max rank or 1 (FUN_10173140), a request in flight overriding the rank.
    int PushTalentRank(lua_State* L, Row r)
    {
        const Request* q = InFlight(RowU32(r, 0));
        PushInt(L, static_cast<int32_t>(q ? q->rank : ActiveBuild()->RankOf(RowU32(r, 0))));
        PushInt(L, RowType(r) == 2 ? static_cast<int32_t>(MaxRank(r)) : 1);
        return 2;
    }

    int GetTalentRankByID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.GetTalentRankByID(entryId)");
        if (Row r = FindRow(id))
            return PushTalentRank(L, r);
        return PushNils(L, 2);
    }

    int GetTalentRankBySpellID(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.GetTalentRankBySpellID(spellId)");
        if (Row r = RowBySpell(spell))
            return PushTalentRank(L, r);
        return PushNils(L, 2);
    }

    // GetLearnedAE / GetLearnedTE: (classType, tabType), (classType) or () -- names must be valid.
    int Learned(lua_State* L, bool ae)
    {
        const char* name = ae ? "GetLearnedAE" : "GetLearnedTE";
        Build* b = ActiveBuild();
        if (ValidateInput(L, {STRING, STRING}))
        {
            const uint32_t cls = EnumByName(CheckString(L, 1), kClassTypes, 0x2F), tab = EnumByName(CheckString(L, 2), kTabTypes, 0x5F);
            if (!cls || !tab)
                return Usage(L, (std::string(name) + "(classType, tabType) expects valid class and tab names").c_str());
            PushInt(L, static_cast<int32_t>(ae ? b->TabAE(cls, tab, 0) : b->TabTE(cls, tab, 0)));
            return 1;
        }
        std::string cname;
        if (ReadString(L, cname))
        {
            const uint32_t cls = EnumByName(cname.c_str(), kClassTypes, 0x2F);
            if (!cls)
                return Usage(L, (std::string(name) + "(classType) expects a valid class name").c_str());
            PushInt(L, static_cast<int32_t>(ae ? b->ClassAE(cls, 0) : b->ClassTE(cls, 0)));
            return 1;
        }
        PushInt(L, static_cast<int32_t>(ae ? b->GlobalAE(0) : b->GlobalTE(0)));
        return 1;
    }
    int GetLearnedAE(lua_State* L) { return Learned(L, true); }
    int GetLearnedTE(lua_State* L) { return Learned(L, false); }

    int KnowsConnectedNodesFor(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.KnowsConnectedNodesFor(entryId)");
        Build* b = PendingOrError(L, "KnowsConnectedNodesFor");
        if (!b)
            return 0;
        if (!FindRow(id))
            return Usage(L, "KnowsConnectedNodesFor: invalid entryId");
        PushBool(L, b->ValidateLearn(nullptr, id, {0xD}, {}) == 0);
        return 1;
    }

    int HasRuneUI(lua_State* L)   // C_Spell.HasRuneUI: FUN_10171f40 (+0x410, the rune-spell count)
    {
        PushBool(L, !g_runeSpells.empty());
        return 1;
    }

    // GetKnown*Entries: the entries of the given kind in the pending and the active build (a set of rows).
    std::vector<Row> KnownRows(bool (*kind)(Row), int cls, int tab, bool skipDropped)
    {
        std::vector<Row> rows;
        auto add = [&](const Build* b) {
            if (!b)
                return;
            for (const Entry& e : b->entries)
            {
                if (!kind(e.row))
                    continue;
                if (cls >= 0 && (RowU32(e.row, 0x80) != static_cast<uint32_t>(cls) || (tab != 1 && RowU32(e.row, 0x84) != static_cast<uint32_t>(tab))))
                    continue;
                if (skipDropped)
                    if (const Request* q = InFlight(e.id))
                        if (q->rank == 0)
                            continue;
                if (std::find(rows.begin(), rows.end(), e.row) == rows.end())
                    rows.push_back(e.row);
            }
        };
        add(g_mgr.pending);
        add(ActiveBuild());
        return rows;
    }
    bool KindSpell(Row r) { const int t = RowType(r); return t == 1 || t == 4; }   // FUN_101c6770
    bool KindTalent(Row r) { return RowType(r) == 2; }                             // FUN_101c6e80

    // LAB_101802b0 orders only "locked before unlocked" (on the active build); the original sorts a set of
    // row pointers, so ties fall in address order -- here, id order.
    std::vector<Row> SortLockedFirst(std::vector<Row> rows)
    {
        std::sort(rows.begin(), rows.end(), [](Row a, Row b) { return RowU32(a, 0) < RowU32(b, 0); });
        const Build* active = ActiveBuild();
        std::stable_sort(rows.begin(), rows.end(), [active](Row a, Row b) {
            return active->LockedOf(RowU32(a, 0)) && !active->LockedOf(RowU32(b, 0));
        });
        return rows;
    }

    // FUN_10180240: required level, then tab, then id.
    bool LevelTabOrder(Row a, Row b)
    {
        const uint32_t la = ActiveRequiredLevel(a), lb = ActiveRequiredLevel(b);
        if (la != lb)
            return la < lb;
        if (RowU32(a, 0x84) != RowU32(b, 0x84))
            return RowU32(a, 0x84) < RowU32(b, 0x84);
        return RowU32(a, 0) < RowU32(b, 0);
    }

    int KnownEntries(lua_State* L, bool (*kind)(Row))
    {
        PushEntryList(L, SortLockedFirst(KnownRows(kind, -1, -1, false)));
        return 1;
    }

    int KnownEntriesForClass(lua_State* L, bool (*kind)(Row), const char* usage)
    {
        if (!ValidateInput(L, {STRING, STRING}))
            return Usage(L, usage);
        const uint32_t cls = EnumArg(L, 1, kClassTypes, 0x2F), tab = EnumArg(L, 2, kTabTypes, 0x5F);
        std::vector<Row> rows = KnownRows(kind, static_cast<int>(cls), static_cast<int>(tab), true);
        std::sort(rows.begin(), rows.end(), LevelTabOrder);
        PushEntryList(L, rows);
        return 1;
    }

    int GetKnownSpellEntries(lua_State* L)  { return KnownEntries(L, KindSpell); }
    int GetKnownTalentEntries(lua_State* L) { return KnownEntries(L, KindTalent); }
    int GetKnownSpellEntriesForClass(lua_State* L)
    {
        return KnownEntriesForClass(L, KindSpell, "Usage: C_CharacterAdvancement.GetKnownSpellEntriesForClass(classType, tabType)");
    }
    int GetKnownTalentEntriesForClass(lua_State* L)
    {
        return KnownEntriesForClass(L, KindTalent, "Usage: C_CharacterAdvancement.GetKnownTalentEntriesForClass(classType, tabType)");
    }


    // ---- direct learn / unlearn ---------------------------------------------------------------------
    // FUN_10166450 (FUN_1011f230 "FrameScript::lua_toarray") or a single number (FUN_1008ba00).
    bool IdsArg(lua_State* L, std::vector<uint32_t>& ids)
    {
        ids.clear();
        if (ValidateInput(L, {TABLE}))
        {
            AscLua::lua_pushnil(L);
            while (AscLua::lua_next(L, 1))
            {
                if (AscLua::lua_isnumber(L, -1))
                    ids.push_back(static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, -1))));
                else
                    AscLog::Printf("FrameScript::lua_toarray Invalid lua value type");
                AscLua::lua_settop(L, -2);
            }
            return true;
        }
        uint32_t id;
        if (!ReadNumber(L, id))
            return false;
        ids.push_back(id);
        return true;
    }

    // FUN_10149a00: an entry with masteries, none of which the build has, brings its last mastery along.
    // Only for the active player with a world object.
    uint32_t MasteryDependency(const Build& b, uint32_t id)
    {
        Row r = FindRow(id);
        if (!r || !g_mgr.player || g_mgr.player->guid != ActivePlayerGuid() || !ObjectPtr(g_mgr.player->guid, 0x10))
            return 0;
        uint32_t last = 0;
        for (uint32_t i = 0; i < 3; ++i)
            if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
            {
                if (b.Has(m))
                    return 0;
                last = m;
            }
        return last;
    }

    void Send0727(const std::vector<Entry>& entries)
    {
        Packet pk(0x727);
        pk.U32(static_cast<uint32_t>(entries.size()));
        for (const Entry& e : entries)
            pk.U32(e.id).U32(e.rank).U32(e.u0c).U8(e.locked).U32(e.u18).U32(e.u1c);
        pk.Send();
    }

    int LearnID(lua_State* L)
    {
        std::vector<uint32_t> ids;
        if (!IdsArg(L, ids))
            return Usage(L, "Usage: C_CharacterAdvancement.LearnID(entryId | {entryId1, ...})");
        Build copy(*ActiveBuild(), true);
        copy.UpdatePointers(false);
        for (uint32_t id : ids)
        {
            if (const uint32_t dep = MasteryDependency(copy, id))
                copy.AddRank(dep);
            copy.AddRank(id);
        }
        // A group-1 entry is exclusive: every other group-1 row's entry goes.
        for (uint32_t id : ids)
            if (Row r = FindRow(id))
                if (RowU32(r, 0x74) == 1)
                    ForEachRow([&](Row other) {
                        if (RowU32(other, 0x74) == 1 && RowU32(other, 0) != id)
                            copy.Remove(RowU32(other, 0));
                    });
        uint32_t result, id, rank;
        if (!copy.Optimize(true, result, id, rank))
        {
            PushBool(L, false);
            PushStr(L, "OPTIMIZE_FOR_TRAVERSAL_FAILED");
            PushStr(L, LearnName(result).c_str());
            PushInt(L, static_cast<int32_t>(id));
            PushInt(L, static_cast<int32_t>(rank));
            return 5;
        }
        uint32_t lr;
        ApplyCosts costs;
        const uint32_t apply = ValidateApply(ActivePlayer(), *ActiveBuild(), copy.entries, lr, id, rank, costs);
        if (apply == 0)
            Send0727(copy.entries);
        PushApplyResult(L, apply, lr, id, rank);
        return 5;
    }

    int CanLearnID(lua_State* L)
    {
        std::vector<uint32_t> ids;
        if (!IdsArg(L, ids))
            return Usage(L, "Usage: C_CharacterAdvancement.CanLearnID(entryId | {entryId1, ...})");
        Build copy(*ActiveBuild(), true);
        for (uint32_t id : ids)
        {
            const uint32_t result = copy.ValidateLearn(nullptr, id, {}, {});
            if (result)
            {
                PushBool(L, false);
                PushStr(L, LearnName(result).c_str());   // FUN_10169de0
                PushInt(L, static_cast<int32_t>(id));
                return 3;
            }
            copy.AddRank(id);
        }
        PushBool(L, true);
        PushStr(L, LearnName(0).c_str());
        PushInt(L, 0);
        return 3;
    }

    int CanUnlearnID(lua_State* L)
    {
        std::vector<uint32_t> ids;
        if (!IdsArg(L, ids))
            return Usage(L, "Usage: C_CharacterAdvancement.CanUnlearnID(entryId | {entryId1, ...})");
        const Build* active = ActiveBuild();
        Build copy(*active, true);
        for (uint32_t id : ids)
        {
            const uint32_t mastery = copy.MasteryTarget(id);
            const bool fusion = copy.ClassFusion() && mastery != 0;
            std::vector<uint32_t> exclude;
            std::vector<Entry> saved;
            if (fusion)
                saved = copy.TakeMasteryBearers();
            else if (copy.MasteryInvolved(id))
                exclude = {10, 9, 11};   // MASTERY_ID, MISSING_CONNECTED_ENTRIES, MISSING_REQUIRED_ID
            const uint32_t result = copy.ValidateUnlearn(nullptr, fusion ? mastery : id, {}, exclude);
            if (result)
            {
                PushBool(L, false);
                PushStr(L, UnlearnName(result).c_str());   // FUN_10169ed0
                PushInt(L, static_cast<int32_t>(id));
                return 3;
            }
            if (fusion)
            {
                copy.Remove(mastery);
                copy.ReaddWhileValid(saved);
            }
            else
                copy.RemoveWithMastery(id);
        }
        uint32_t lr, eid, rank;
        ApplyCosts costs;
        const uint32_t apply = ValidateApply(ActivePlayer(), *active, copy.entries, lr, eid, rank, costs);
        PushBool(L, apply == 0);
        if (apply == 0)
        {
            PushStr(L, UnlearnName(0).c_str());
            PushInt(L, 0);
        }
        else
        {
            PushStr(L, UpdateName(apply).c_str());
            PushInt(L, static_cast<int32_t>(eid));
        }
        return 3;
    }

    // LockID / UnlockID: CMSG 0x658 / 0x65A with the entry id.
    int SendIdPacket(lua_State* L, uint32_t opcode, const char* usage)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, usage);
        Packet(opcode).U32(id).Send();
        PushBool(L, true);
        return 1;
    }
    int LockID(lua_State* L)   { return SendIdPacket(L, 0x658, "Usage: C_CharacterAdvancement.LockID(entryId)"); }
    int UnlockID(lua_State* L) { return SendIdPacket(L, 0x65A, "Usage: C_CharacterAdvancement.UnlockID(entryId)"); }


    // ---- confirmations (CA_CONFIRM_*) ---------------------------------------------------------------
    struct Confirm { uint32_t type, a, b; };

    bool WildcardMode() { return (AscGameMode::Mode() >> 6) & 1; }   // FUN_1031ef10 bit 6

    uint32_t Coinage(const uint8_t* unit) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0x1248); }

    // The record and world object of the active player, which every confirmation check requires.
    const uint8_t* ConfirmUnit()
    {
        Player* p = FindPlayer(ActivePlayerGuid());
        if (!p || p->guid != ActivePlayerGuid())
            return nullptr;
        return static_cast<const uint8_t*>(ObjectPtr(p->guid, 0x10));
    }

    // FUN_10149a00: learning an entry with masteries, none known, brings the last one along.
    bool LearnConfirm(const Build& b, uint32_t id, std::vector<Confirm>& out)
    {
        Row r = FindRow(id);
        if (!r || !ConfirmUnit())
            return false;
        uint32_t last = 0;
        for (uint32_t i = 0; i < 3; ++i)
            if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
            {
                last = m;
                if (b.Has(m))
                    return !out.empty();
            }
        if (last)
            out.push_back({2, last, 0});
        return !out.empty();
    }

    // A cost the unit can pay, as the confirmation to show: the token, else marks, else gold.
    void CostConfirm(const uint8_t* unit, const TokenCost& c, uint32_t token, uint32_t marks, uint32_t gold,
                     std::vector<Confirm>& out)
    {
        if (!((c.item && c.count) || c.marks || c.money))
            return;
        if (c.item && c.count && c.count <= UnitItemCount(unit, c.item))
            out.push_back({token, c.item, c.count});
        else if (c.marks && c.marks <= UnitItemCount(unit, kMarkOfAscensionItem))
            out.push_back({marks, c.marks, 0});
        else if (c.money && c.money <= Coinage(unit))
            out.push_back({gold, c.money, 0});
    }

    // Wildcard Scroll of Fortune tokens per spec (FUN_10a30180/10a300a0 talents, FUN_10a2fd80/10a2fca0
    // abilities, FUN_10a2ffc0/10a2fee0 any); token index 0x3D and item 0 past spec 19.
    const uint32_t kAnyScroll[20] = {0x10CDBC, 0x10F4CC, 0x111BDC, 0x1142EC, 0x1169FC, 0x11910C, 0x11B81C,
                                     0x11DF2C, 0xB799A, 0xB799B, 0xB799C, 0xB799D, 0xB79A7, 0xB79A8,
                                     0xB79A9, 0xB79AA, 0xB79AB, 0xB79AC, 0xB79AD, 0xB79AE};
    bool ScrollConfirm(uint32_t tokenBase, uint32_t itemBase, std::vector<Confirm>& out)
    {
        const uint32_t spec = static_cast<uint32_t>(ActiveSpecIndex());
        const uint32_t token = spec < 20 ? tokenBase + spec : 0x3D;
        if (!AscToken::AtLeast(token, 1, 0))
            return false;
        const uint32_t item = spec >= 20 ? 0 : itemBase ? itemBase + spec : kAnyScroll[spec];
        out.push_back({3, item, 1});
        return true;
    }

    // FUN_10149e00: unlearning drops a mastery nobody else carries, or costs something.
    bool UnlearnConfirm(const Build& b, uint32_t id, std::vector<Confirm>& out)
    {
        Row r = FindRow(id);
        const uint8_t* unit = ConfirmUnit();
        if (!r || !unit)
            return false;
        uint32_t last = 0;
        bool any = false, known = false, shared = false;
        for (uint32_t i = 0; i < 3 && !shared; ++i)
        {
            const uint32_t m = RowU32(r, 0x1B0 + i * 4);
            if (!m)
                continue;
            any = true;
            last = m;
            if (!b.Has(m))
                continue;
            known = true;
            auto it = g_byMastery.find(m);
            if (it != g_byMastery.end())
                for (Row other : it->second)
                    if (RowU32(other, 0) != id && b.Has(RowU32(other, 0)))
                    {
                        shared = true;   // another known entry keeps the mastery
                        break;
                    }
        }
        if (!shared && any && known)
            out.push_back({6, last, 0});
        if (!WildcardMode())
        {
            if (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0xD8) > 9)
                CostConfirm(unit, UnlearnCostOf(b, r), 3, 4, 5, out);
        }
        else
        {
            const int type = RowType(r);
            if (!(type == 2 && ScrollConfirm(0x28, 0x17E8D, out))
                && !((type == 1 || type == 4) && ScrollConfirm(0x14, 0x17E79, out)))
                ScrollConfirm(0, 0, out);
        }
        return !out.empty();
    }

    // FUN_10149b00 / FUN_10149c80. The marks case pushes CA_CONFIRM_UNLEARN_MARKS (4) for both, as the
    // original does.
    bool PurgeConfirm(const Build& b, bool talents, std::vector<Confirm>& out)
    {
        const uint8_t* unit = ConfirmUnit();
        if (!unit)
            return false;
        CostConfirm(unit, PurgeCost(b, talents), talents ? 11 : 7, 4, talents ? 13 : 9, out);
        if (AscBuildCreator::SpecHasActiveBuild(ActiveSpecIndex()))
            out.push_back({talents ? 14u : 10u, 0, 0});
        return !out.empty();
    }

    // FUN_10169740: {{Error = CA_CONFIRM_*, Arg1 = a, Arg2 = b}, ...}.
    void PushConfirms(lua_State* L, const std::vector<Confirm>& list)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(list.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < list.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_createtable(L, 0, 3);
            AscLua::lua_checkstack(L, 2);
            const uint32_t t = list[i].type;
            SetStr(L, "Error", t < 0xF ? kConfirmTypes[t] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(t));   // FUN_10169c90
            SetInt(L, "Arg1", static_cast<int32_t>(list[i].a));
            SetInt(L, "Arg2", static_cast<int32_t>(list[i].b));
            AscLua::lua_settable(L, -3);
        }
    }

    int PushConfirmResult(lua_State* L, const std::vector<Confirm>& list)
    {
        PushBool(L, !list.empty());
        if (list.empty())
            return 1;
        PushConfirms(L, list);
        return 2;
    }

    int ShouldConfirmLearnID(lua_State* L)
    {
        std::vector<uint32_t> ids;
        if (!IdsArg(L, ids))
            return Usage(L, "Usage: C_CharacterAdvancement.ShouldConfirmLearnID(entryId | {entryId1, ...})");
        std::vector<Confirm> list;
        Build copy(*ActiveBuild(), true);
        copy.UpdatePointers(false);
        for (uint32_t id : ids)
        {
            std::vector<Confirm> one;
            if (LearnConfirm(copy, id, one))
                list.insert(list.end(), one.begin(), one.end());
            copy.AddRank(id);
        }
        return PushConfirmResult(L, list);
    }

    int ShouldConfirmUnlearnID(lua_State* L)
    {
        std::vector<uint32_t> ids;
        if (!IdsArg(L, ids))
            return Usage(L, "Usage: C_CharacterAdvancement.ShouldConfirmUnlearnID(entryId | {entryId1, ...})");
        std::vector<Confirm> list;
        Build copy(*ActiveBuild(), true);
        copy.UpdatePointers(false);
        for (uint32_t id : ids)
        {
            const uint32_t mastery = copy.MasteryTarget(id);
            const bool fusion = copy.ClassFusion() && mastery != 0;
            std::vector<Confirm> one;
            if (UnlearnConfirm(copy, id, one))
                for (const Confirm& c : one)
                    if (!fusion || c.type != 6)
                        list.push_back(c);
            if (fusion)
                list.push_back({6, mastery, 0});
            copy.RemoveWithMastery(id);
        }
        return PushConfirmResult(L, list);
    }

    int ShouldConfirmUnlearnAll(lua_State* L, bool talents)
    {
        std::vector<Confirm> list;
        PurgeConfirm(*ActiveBuild(), talents, list);
        return PushConfirmResult(L, list);
    }
    int ShouldConfirmUnlearnAllSpells(lua_State* L)  { return ShouldConfirmUnlearnAll(L, false); }
    int ShouldConfirmUnlearnAllTalents(lua_State* L) { return ShouldConfirmUnlearnAll(L, true); }

    // FUN_10175450 / FUN_101755d0: (ok, CA_PURGE_{ABILITIES,TALENTS}_*).
    int CanUnlearnAll(lua_State* L, bool talents)
    {
        const uint32_t r = ActiveBuild()->ValidatePurge(talents, nullptr);
        PushBool(L, r == 0);
        const char* const* names = talents ? kPurgeTalentResults : kPurgeResults;
        PushStr(L, (r < 10 ? std::string(names[r]) : "UNEXPECTED_ENUM_VALUE_" + std::to_string(r)).c_str());
        return 2;
    }
    int CanUnlearnAllSpells(lua_State* L)  { return CanUnlearnAll(L, false); }
    int CanUnlearnAllTalents(lua_State* L) { return CanUnlearnAll(L, true); }

    // FUN_1017fbe0. Normal modes: remove each (with its mastery bookkeeping), re-validate the order and
    // send the whole build as 0x727. Wildcard: one Scroll of Fortune request (CMSG 0x61D, u32 id) per id,
    // only while no roll is running and no request is outstanding.
    int UnlearnID(lua_State* L)
    {
        std::vector<uint32_t> ids;
        if (!IdsArg(L, ids))
            return Usage(L, "Usage: C_CharacterAdvancement.UnlearnID(entryId | {entryId1, ...})");
        if (!WildcardMode())
        {
            Build copy(*ActiveBuild(), true);
            copy.UpdatePointers(false);
            for (uint32_t id : ids)
                copy.RemoveWithMastery(id);
            if (!copy.Reorder(false))
            {
                PushBool(L, false);
                return 1;
            }
            Send0727(copy.entries);
            PushBool(L, true);
            return 1;
        }
        if (AscWildcardRapid::Active() || AscWildcardRapid::InFlight() || !g_requests.empty())
        {
            PushBool(L, false);
            return 1;
        }
        for (uint32_t id : ids)
            Packet(0x61D).U32(id).Send();
        // FUN_10a31760 re-arms the mode manager's roll-ready poll; FUN_10a2f910 flags the unlearn as
        // outstanding until SMSG 0x61E.
        AscWildcardRolls::ArmPoll();
        AscWildcardRapid::MarkRerollPending();
        PushBool(L, true);
        return 1;
    }


    // ---- qualities, known spells, tags, pickup ------------------------------------------------------
    // FUN_1009a460: a 1-based array of integers.
    void PushIntArray(lua_State* L, const std::vector<uint32_t>& v)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int32_t>(v[i]));
            AscLua::lua_settable(L, -3);
        }
    }

    // FUN_10168cc0 / FUN_1014e100: exact match against the nine quality names.
    bool QualityByName(const std::string& name, int& q)
    {
        for (int i = 0; i < 9; ++i)
            if (name == kQualities[i])
            {
                q = i;
                return true;
            }
        return false;
    }

    int GetQualityCount(lua_State* L)   // FUN_10179c50
    {
        std::string name;
        if (!ReadString(L, name))
            return Usage(L, "Usage: C_CharacterAdvancement.GetQualityCount(qualityName)");
        int q;
        if (!QualityByName(name, q))
            return Usage(L, "GetQualityCount: invalid quality name");
        if (!g_mgr.pending)
            return Usage(L, "GetQualityCount: pending build is not available");
        PushInt(L, static_cast<int32_t>(QualitySumOf(*g_mgr.pending, q)));
        return 1;
    }

    // FUN_10179f50. Normal mode: the realm's CONFIG_..._QUALITIES_MAX_<Q>_ABILITIES_<expansion> (8/8/4/2 by
    // default), 99 for the other qualities. Draft: Epic 6, Legendary 3, else nil. Wildcard: nil.
    int GetQualityLimit(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return Usage(L, "Usage: C_CharacterAdvancement.GetQualityLimit(qualityName)");
        int q;
        if (!QualityByName(name, q))
            return Usage(L, "GetQualityLimit: invalid quality name");
        const uint32_t mode = AscGameMode::Mode();
        if ((mode >> 3) & 1)
        {
            if (q == 4 || q == 5)
                PushInt(L, q == 4 ? 6 : 3);
            else
                AscLua::lua_pushnil(L);
            return 1;
        }
        if ((mode >> 6) & 1)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const char* stem = q == 2 ? "UNCOMMON" : q == 3 ? "RARE" : q == 4 ? "EPIC" : q == 5 ? "LEGENDARY" : nullptr;
        if (!stem)
        {
            PushInt(L, 99);
            return 1;
        }
        const uint32_t ruleset = RealmInfoSvc::Get().ruleset;   // FUN_1008e2d0 +8
        const char* exp = ruleset == 0 ? "CLASSIC" : ruleset == 1 ? "THE_BURNING_CRUSADE" : "WRATH_OF_THE_LICH_KING";
        const std::string key = std::string("CONFIG_CHARACTER_ADVANCEMENT_QUALITIES_MAX_") + stem + "_ABILITIES_" + exp;
        const int32_t* v = AscConfig::Int(key.c_str());   // FUN_10196ab0
        PushInt(L, v ? *v : (q == 4 ? 4 : q == 5 ? 2 : 8));
        return 1;
    }

    // FUN_101c60e0: the wildcard quality of a spell at rank index `i` -- the row's wildcard quality moved
    // up by the rank, 0 outside Poor..Heirloom.
    uint8_t QualityAtRank(Row r, uint32_t i)
    {
        int q = 0;
        const bool valid = QualityByName(RowStr(r, 0x60), q);
        const uint8_t v = static_cast<uint8_t>(static_cast<uint8_t>(i) - 1 + (valid ? q : 0));
        return v > 8 ? 0 : v;
    }

    // LAB_10180010: GetKnownSpells order -- by quality (per rank on a wildcard build), else by spell id.
    bool KnownSpellOrder(uint32_t a, uint32_t b)
    {
        Row ra = RowBySpell(a), rb = RowBySpell(b);
        if (!ra || !rb)
            return a < b;
        const Build* active = ActiveBuild();
        if (active->wildcard)   // FUN_101556b0
        {
            auto at = [](Row r, uint32_t spell) {
                const uint32_t n = RowMaxRank(r);
                for (uint32_t i = 0; i < n; ++i)
                    if (RowU32(r, 0x14 + i * 4) == spell)
                        return QualityAtRank(r, i);
                return static_cast<uint8_t>(0);
            };
            return at(ra, a) < at(rb, b);
        }
        return static_cast<uint8_t>(QualityIndexOf(*active, ra)) < static_cast<uint8_t>(QualityIndexOf(*active, rb));
    }

    // FUN_101c62e0: the spell at a 1-based rank, 0 outside 1..9.
    uint32_t SpellAtRank(Row r, uint32_t rank)
    {
        const uint8_t k = static_cast<uint8_t>(rank);
        return k && static_cast<uint8_t>(k - 1) < 9 ? RowU32(r, 0x14 + (k - 1) * 4) : 0;
    }

    // FUN_101784b0: the active build's spells at their ranks -- abilities sorted, then talents and talent
    // abilities sorted.
    int GetKnownSpells(lua_State* L)
    {
        std::vector<uint32_t> abilities, talents;
        for (const Entry& e : ActiveBuild()->entries)
        {
            Row r = FindRow(e.id);
            if (!r)
                continue;
            const int type = RowType(r);
            if (type == 1)
                abilities.push_back(SpellAtRank(r, e.rank));
            else if (type == 2 || type == 4)
                talents.push_back(SpellAtRank(r, e.rank));
        }
        std::sort(abilities.begin(), abilities.end(), KnownSpellOrder);
        std::sort(talents.begin(), talents.end(), KnownSpellOrder);
        std::vector<uint32_t> all;
        all.reserve(abilities.size() + talents.size());
        all.insert(all.end(), abilities.begin(), abilities.end());
        all.insert(all.end(), talents.begin(), talents.end());
        PushIntArray(L, all);
        return 1;
    }

    // FUN_1017c640: `to` lists `from` among its connections (+0xE4, 15 ids) and the pending build has
    // `from` (at max rank when `from` carries flag 0x200000).
    int IsConnectionAllowed(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER}))
            return Usage(L, "Usage: C_CharacterAdvancement.IsConnectionAllowed(fromEntryId, toEntryId)");
        const uint32_t from = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint32_t to = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        if (!g_mgr.pending)
            return Usage(L, "IsConnectionAllowed: pending build is not available");
        bool ok = false;
        Row rf = FindRow(from), rt = FindRow(to);
        if (rf && rt)
        {
            bool listed = false;
            for (uint32_t off = 0xE4; off < 0x120; off += 4)
                if (RowU32(rt, off) == from)
                {
                    listed = true;
                    break;
                }
            if (listed)
                ok = RowHasFlag(rf, 0x200000) ? g_mgr.pending->RankOf(RowU32(rf, 0)) == RowMaxRank(rf) && g_mgr.pending->Has(RowU32(rf, 0))
                                              : g_mgr.pending->Has(RowU32(rf, 0));
        }
        PushBool(L, ok);
        return 1;
    }

    // C_CharacterAdvancement.GetClassInfo(spellId) -- FUN_10176850: the class-type and tab-type names of
    // the entry listing the spell (FUN_101498d0), "INVALID_CLASS_TYPE_{}" / "INVALID_TAB_TYPE_{}" for an
    // unmapped value; one nil when no entry lists it.
    int GetClassInfoBySpell(lua_State* L)
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return Usage(L, "Usage: C_CharacterAdvancement.GetClassInfo(spellId)");
        Row r = RowBySpell(spell);
        if (!r)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushStr(L, ClassName(RowU32(r, 0x80)).c_str());
        PushStr(L, TabName(RowU32(r, 0x84)).c_str());
        return 2;
    }

    // The global GetClassInfo -- handler_GetClassInfo (0x100D8FA0, registrar FUN_100df760): ChrClasses
    // (container 0xAD3404) name, filename, id; three nils for an unknown class. Not the C_CA binding above.
    int GetClassInfo(lua_State* L)
    {
        uint32_t cls;
        if (!ReadNumber(L, cls))
            return 0;
        const uint8_t* row = ClientDbcRow(0xAD3404, cls);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 3;
        }
        const char* name = *reinterpret_cast<const char* const*>(row + 0x10);
        const char* file = *reinterpret_cast<const char* const*>(row + 0x1C);
        AscLua::lua_pushstring(L, name ? name : "");
        AscLua::lua_pushstring(L, file ? file : "");
        PushInt(L, static_cast<int32_t>(RowU32(row, 0)));
        return 3;
    }

    int CanUseBrowser(lua_State* L)   // FUN_10175d50
    {
        const bool* on = AscConfig::Bool("CONFIG_ABILITY_BROWSER_ENABLED");
        PushBool(L, on && *on && !((AscGameMode::Mode() >> 3) & 1));
        return 1;
    }

    // SpellTagTypes.dbc: +0 id, +4 parent, +8 sort order, +0xC hidden, +0x68/+0x6C/+0x70 strings.
    AscDbc::Table& TagTypes() { return AscDbc::Get("DBFilesClient/SpellTagTypes.dbc"); }

    bool TagOrder(uint32_t a, uint32_t b)   // LAB_101804b0
    {
        const uint8_t* ra = TagTypes().Row(a);
        const uint8_t* rb = TagTypes().Row(b);
        if (!ra || !rb)
            return a < b;
        return RowU32(ra, 8) < RowU32(rb, 8);
    }

    std::vector<uint32_t> TagChildren(uint32_t parent)
    {
        std::vector<uint32_t> ids;
        AscDbc::Table& t = TagTypes();
        for (uint32_t i = 0; i < t.Count(); ++i)
            if (const uint8_t* r = t.RowAt(i))
                if (RowU32(r, 0xC) == 0 && RowU32(r, 4) == parent)
                    ids.push_back(RowU32(r, 0));
        std::sort(ids.begin(), ids.end(), TagOrder);
        return ids;
    }

    int GetRootSpellTagTypes(lua_State* L)   // FUN_1017aa50
    {
        PushIntArray(L, TagChildren(0));
        return 1;
    }

    int GetSpellTagTypes(lua_State* L)   // FUN_1017ad00
    {
        uint32_t parent;
        if (!ReadNumber(L, parent))
            return Usage(L, "Usage: C_CharacterAdvancement.GetSpellTagTypes(parentTypeId)");
        const std::vector<uint32_t> ids = TagChildren(parent);
        if (ids.empty())
            AscLua::lua_pushnil(L);
        else
            PushIntArray(L, ids);
        return 1;
    }

    int GetSpellTagTypeDisplayInfo(lua_State* L)   // FUN_1017ab50
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.GetSpellTagTypeDisplayInfo(typeId)");
        AscDbc::Table& t = TagTypes();
        const uint8_t* r = t.Row(id);
        if (!r || RowU32(r, 0xC) != 0)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 3;
        }
        AscLua::lua_pushstring(L, t.Str(r, 0x6C));
        const char* icon = t.Str(r, 0x68);
        if (*icon)
            AscLua::lua_pushstring(L, icon);
        else
            AscLua::lua_pushnil(L);
        AscLua::lua_pushstring(L, t.Str(r, 0x70));
        return 3;
    }

    // FUN_1008e4f0(spell, 0) on the player: the highest rank of the spell's chain the player knows (the
    // client spellbook 0x7260E0, then the CA known set), else the spell itself if known, else 0.
    uint32_t KnownRankOf(uint8_t* player, uint32_t spell)
    {
        typedef char(__thiscall * Knows_t)(void*, uint32_t);
        auto knows = [&](uint32_t s) { return reinterpret_cast<Knows_t>(0x7260E0)(player, s) != 0 || g_knownSpells.count(s) != 0; };
        const std::vector<uint32_t>& chain = AscSpellRank::Chain(spell);
        if (chain.empty())
            return knows(spell) ? spell : 0;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if (knows(*it))
                return *it;
        return 0;
    }

    int PickupSpell(lua_State* L)   // handler_PickupSpell
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.PickupSpell(entryId)");
        Row r = FindRow(id);
        if (!r)
            return Usage(L, "PickupSpell: invalid entryId");
        uint32_t rank = ActiveBuild()->RankOf(RowU32(r, 0));
        if (!rank)
        {
            if (!RowHasFlag(r, 0x20000))
                return 0;
            rank = 1;
        }
        uint8_t* player = AscScript::ActivePlayer();
        if (!player)
            return Usage(L, "PickupSpell: no active player");
        const uint32_t spell = SpellAtRank(r, rank);
        if (!spell)
            return Usage(L, "PickupSpell: entryId has no spell at current rank");
        if (const uint32_t known = KnownRankOf(player, spell))
        {
            const uint32_t count = *reinterpret_cast<const uint32_t*>(0xBE8D98);
            const uint32_t* book = reinterpret_cast<const uint32_t*>(0xBE6D88);
            for (uint32_t i = 0; i < count; ++i)
                if (book[i] == known)
                {
                    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t)>(0x53BAF0)(i, 0);
                    return 0;
                }
            return Usage(L, "PickupSpell: spell not found in spellbook");
        }
        return 0;
    }


    // ---- categories (CharacterAdvancementCategories.dbc: +4 order, +8 required level, +0xC flag,
    // +0x10/+0x14/+0x18 strings) ----------------------------------------------------------------------
    AscDbc::Table& Categories() { return AscDbc::Get("DBFilesClient\\CharacterAdvancementCategories.dbc"); }

    // FUN_10176450: every category id, by order (LAB_10180110) or by required level then order
    // (LAB_10180170) when the optional argument is true.
    int GetCategories(lua_State* L)
    {
        const int t = AscLua::lua_type(L, 1);
        if (t > 0 && t != 1)   // FUN_10309e50: nothing, nil or a boolean
            return Usage(L, "Usage: C_CharacterAdvancement.GetCategories([sortByRequiredLevel])");
        const bool byLevel = t > 0 && AscLua::lua_toboolean(L, 1) != 0;
        AscDbc::Table& c = Categories();
        std::vector<uint32_t> ids;
        if (c.Loaded())
            for (uint32_t id = c.MinId(); id <= c.MaxId(); ++id)
                if (c.Row(id))
                    ids.push_back(id);
        std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
            const uint8_t* ra = c.Row(a);
            const uint8_t* rb = c.Row(b);
            if (byLevel && RowU32(ra, 8) != RowU32(rb, 8))
                return RowU32(ra, 8) < RowU32(rb, 8);
            return RowU32(ra, 4) < RowU32(rb, 4);
        });
        PushIntArray(L, ids);
        return 1;
    }

    // handler_GetCategoryDisplayInfo: required level, +0x14, +0x10, +0x18, flag; five nils if unknown.
    int GetCategoryDisplayInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return Usage(L, "Usage: C_CharacterAdvancement.GetCategoryDisplayInfo(categoryId)");
        AscDbc::Table& c = Categories();
        const uint8_t* r = c.Row(id);
        if (!r)
        {
            for (int i = 0; i < 5; ++i)
                AscLua::lua_pushnil(L);
            return 5;
        }
        PushInt(L, static_cast<int32_t>(RowU32(r, 8)));
        AscLua::lua_pushstring(L, c.Str(r, 0x14));
        AscLua::lua_pushstring(L, c.Str(r, 0x10));
        AscLua::lua_pushstring(L, c.Str(r, 0x18));
        PushBool(L, RowU32(r, 0xC) != 0);
        return 5;
    }


    // ---- specializations and loadouts ------------------------------------------------------------------
    // FUN_1016f480: the pending build's spec -- the first ChrSpecs.dbc row whose entry (+0x70) it has.
    uint32_t SpecOf(const Build& b)
    {
        AscDbc::Table& t = ChrSpecs();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (Row r = t.Row(id))
                if (b.Has(RowU32(r, 0x70)))
                    return RowU32(r, 0);
        return 0;
    }

    /* Tell the window's two trees which build they must draw - the one the pending build now holds.
     * A whole build written at once (a staged archetype, a state push, a reset) is announced to the
     * window as well, and an announcement alone is not enough: the trees subscribe to
     * `CHARACTER_ADVANCEMENT_PENDING_BUILD_UPDATED` in their OnShow and unsubscribe in OnHide
     * (`TalentTreeBaseMixin`), so a tree that is hidden while the build is written never hears it and
     * goes on drawing the ranks of the visit before - the "the left tree shows the other archetype's
     * talents, and shows nothing after switching back" report. A mark is not an announcement: the
     * client's own `MarkDirty` is a flag on the tree that survives being hidden (its OnUpdate is only
     * spent on the first frame the tree is visible again), so it lands whatever the window is showing
     * at the time, and it costs the tree exactly the pass it needs.
     *
     * The class tree always takes `Nodes`: its tab is the class, fixed for the window's whole life
     * (`CoATreeViewMixin:OnLoad`), so a new build changes only the ranks it draws. The spec tree takes
     * `Nodes` only while it already draws this spec; otherwise the helper repoints it
     * (`CoATreeViewMixin:SetSpecID` -> `SetClassTab` -> `RebuildTree`), because `SetSpecID` is the only
     * call that repoints it at all and the window does not always make it - `ShowTreeView`, the path a
     * click on the character's own card takes, shows the trees without repointing them, so a window
     * whose spec and whose build disagree would draw the previous archetype's tree from then on. Two
     * dirty reasons on one tree would cost two frames (`TalentTreeBaseMixin:Update` spends one per
     * frame): it would repaint the previous archetype's nodes against the new build and only rebuild
     * them on the following frame, which is the nodes visibly repopulating. Mark or repoint, exactly
     * one reason each, whatever the window is doing.
     *
     * Called before the announcement, so `view.specID` still names the spec the window is drawing:
     * that is what decides mark against repoint. The trees live in Lua, so it is asked of the client's
     * own Lua - the helper the glue installs, which does nothing when the window was never built
     * (there are no nodes to be stale then). */
    void MarkWindowTrees(uint32_t specId)
    {
        if (!g_windowTrees)
            return;
        const std::string code = "CoACompatMarkTrees(" + std::to_string(specId) + ")";
        // FrameScript_Execute 0x819210 - the same bridge the glue and AscRealmData::RunLua use.
        reinterpret_cast<void(__cdecl*)(const char*, int, int)>(0x819210)(code.c_str(), 0, 0);
    }

    void MarkWindowTreesFromPending()
    {
        if (!g_windowTrees || !g_mgr.pending)
            return;
        MarkWindowTrees(SpecOf(*g_mgr.pending));
    }

    /* The two ends of a browse the window drives - its close, and the archetype chooser being shown -
     * are installed by the glue, on the window's own frames. Installing them from the glue's events
     * alone is not enough: the frames are built by the archetype addon, and by the time the realm
     * installs its glue both `ADDON_LOADED` and `PLAYER_ENTERING_WORLD` have already fired, so a
     * session that installs before the window exists keeps the hooks uninstalled for good - closing
     * the window then ends nothing, and the browsed archetype goes on being answered as the window's
     * identity (its tab, its trees, its card marked active) until something else ends the browse.
     *
     * The staging that starts a browse is the one moment the window is certain to exist - the player
     * has just clicked a card in it - so the hooks are installed here as well, before the signal that
     * makes the window draw the browse. The installer is idempotent, and a no-op until the window
     * exists, so calling it on every staging is free. */
    void EnsureBrowseEnds()
    {
        if (!g_windowTrees)
            return;
        reinterpret_cast<void(__cdecl*)(const char*, int, int)>(0x819210)(
            "if CoACompatInstallBrowseEnds then CoACompatInstallBrowseEnds() end", 0, 0);
    }

    // FUN_10182500: leave `spec` -- drop every entry outside choice group 1 except the spec-neutral ones
    // (+0x84 == 0x57 with no costs at +0x88/+0x90/+0x98), the spec's entry (+0x70), and its spell's
    // entry (+0x60) when that may be unlearned.
    void LeaveSpec(Build& b, uint32_t spec)
    {
        const std::vector<Entry> snapshot = b.entries;
        for (const Entry& e : snapshot)
            if (e.row && RowU32(e.row, 0x74) != 1
                && (RowU32(e.row, 0x84) != 0x57 || RowU32(e.row, 0x88) || RowU32(e.row, 0x90) || RowU32(e.row, 0x98)))
                b.Remove(e.id);
        Row sp = ChrSpecs().Row(spec);
        if (!sp)
            return;
        if (b.Has(RowU32(sp, 0x70)))
            b.Remove(RowU32(sp, 0x70));
        Row src = RowBySpell(RowU32(sp, 0x60));
        if (src && Visible(src) && b.ValidateUnlearn(nullptr, RowU32(src, 0), {}, {}) == 0)
            b.Remove(RowU32(src, 0));
    }

    // FUN_1016df10: take `spec` -- add its entry and its spell's entry at rank 1, then TraverseOrFix
    // (called with an empty keep set; the ids added are only reported to the caller).
    void EnterSpec(Build& b, uint32_t spec)
    {
        if (Row sp = ChrSpecs().Row(spec))
        {
            if (!b.Has(RowU32(sp, 0x70)))
                b.AddEntry(RowU32(sp, 0x70), 1);
            Row src = RowBySpell(RowU32(sp, 0x60));
            if (src && Visible(src))
                b.AddEntry(RowU32(src, 0), 1);
        }
        b.TraverseOrFix({});
    }

    // FUN_10175190: (false, NO_PENDING_BUILD) / (false, CHR_SPEC_ALREADY_ACTIVE) / (true, nil). The
    // original also runs the switch on a throwaway copy of the pending build; nothing reads it.
    int CanSwitchActiveChrSpec(lua_State* L)
    {
        uint32_t spec;
        if (!ReadNumber(L, spec))
            return Usage(L, "Usage: C_CharacterAdvancement.CanSwitchActiveChrSpec(specID)");
        if (!g_mgr.pending)
        {
            PushBool(L, false);
            PushStr(L, "NO_PENDING_BUILD");
            return 2;
        }
        const uint32_t current = SpecOf(*g_mgr.pending);
        if (current == spec)
        {
            PushBool(L, false);
            PushStr(L, "CHR_SPEC_ALREADY_ACTIVE");
            return 2;
        }
        Build copy(*g_mgr.pending, true);
        LeaveSpec(copy, current);
        EnterSpec(copy, spec);
        copy.Reorder(false);
        PushBool(L, true);
        AscLua::lua_pushnil(L);
        return 2;
    }

    int SwitchActiveChrSpec(lua_State* L)   // FUN_1017f700
    {
        uint32_t spec;
        if (!ReadNumber(L, spec))
            return Usage(L, "Usage: C_CharacterAdvancement.SwitchActiveChrSpec(specID)");
        Build* b = g_mgr.pending;
        if (!b)
            return Usage(L, "SwitchActiveChrSpec: pending build is not available");
        const uint32_t current = SpecOf(*b);
        if (current != spec)
        {
            LeaveSpec(*b, current);
            EnterSpec(*b, spec);
            b->Reorder(false);
            BumpPendingVersion();
        }
        return 0;
    }

    // Loadouts live on the server: CMSG 0x778 activate(uuid), 0x779 rename(uuid, name), 0x77A
    // sort(uuid, u32 order).
    int ActivateLoadout(lua_State* L)   // FUN_10173fd0
    {
        std::string uuid;
        if (!ReadString(L, uuid))
            return Usage(L, "Usage: C_CharacterAdvancement.ActivateLoadout(uuid)");
        Packet(0x778).Str(uuid.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    int SetLoadoutName(lua_State* L)   // FUN_1017eb10
    {
        if (!ValidateInput(L, {STRING, STRING}))
            return Usage(L, "Usage: C_CharacterAdvancement.SetLoadoutName(uuid, name)");
        const std::string uuid = CheckString(L, 1), name = CheckString(L, 2);
        Packet(0x779).Str(uuid.c_str()).Str(name.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    int SetLoadoutSortOrder(lua_State* L)   // FUN_1017ece0
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return Usage(L, "Usage: C_CharacterAdvancement.SetLoadoutSortOrder(uuid, sortOrder)");
        const std::string uuid = CheckString(L, 1);
        const uint32_t order = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        Packet(0x77A).Str(uuid.c_str()).U32(order).Send();
        PushBool(L, true);
        return 1;
    }


    // ---- swap and trade ----------------------------------------------------------------------------------
    // Swap data: the existing entry and the rank it keeps (0 = removed), and the entries it becomes.
    struct SwapEntry { uint32_t entry = 0, rank = 0; };
    struct SwapData { SwapEntry existing; std::vector<SwapEntry> updated; };   // 0x14 bytes

    int AbsIndex(lua_State* L, int idx) { return idx < 0 ? AscLua::lua_gettop(L) + idx + 1 : idx; }

    // FUN_1016b890 / FUN_1016b0e0: {Entry = n, NewCARank = n}; a missing member is logged and left 0.
    SwapEntry ReadSwapEntry(lua_State* L, int idx)
    {
        SwapEntry e;
        idx = AbsIndex(L, idx);
        if (AscLua::lua_type(L, idx) != 5)
        {
            AscLog::Printf("lua_tovalue<CharacterAdvancementSwapEntry>: Value at index %d is not a table!", idx);
            return e;
        }
        auto field = [&](const char* name, uint32_t& out) {
            AscLua::lua_getfield(L, idx, name);
            if (AscLua::lua_isnumber(L, -1))
                out = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, -1)));
            else
                AscLog::Printf("lua_tovalue<CharacterAdvancementSwapEntry>: Missing member '%s' in table at index %d", name, idx);
            AscLua::lua_settop(L, -2);
        };
        field("Entry", e.entry);
        field("NewCARank", e.rank);
        return e;
    }

    // FUN_1016adf0: {ExistingEntry = {...}, UpdatedEntries = {{...}, ...}}.
    SwapData ReadSwapData(lua_State* L, int idx)
    {
        SwapData d;
        idx = AbsIndex(L, idx);
        AscLua::lua_getfield(L, idx, "ExistingEntry");
        if (AscLua::lua_type(L, -1) == 5)
            d.existing = ReadSwapEntry(L, -1);
        else
            AscLog::Printf("lua_tovalue<CharacterAdvancementSwapData>: Missing member 'ExistingEntry' in table at index %d", idx);
        AscLua::lua_settop(L, -2);
        AscLua::lua_getfield(L, idx, "UpdatedEntries");   // FUN_101658e0
        if (AscLua::lua_type(L, -1) == 5)
        {
            const int list = AscLua::lua_gettop(L);
            AscLua::lua_pushnil(L);
            while (AscLua::lua_next(L, list))
            {
                d.updated.push_back(ReadSwapEntry(L, -1));
                AscLua::lua_settop(L, -2);
            }
        }
        else
            AscLog::Printf("lua_tovalue<CharacterAdvancementSwapData>: Missing member 'UpdatedEntries' in table at index %d", idx);
        AscLua::lua_settop(L, -2);
        return d;
    }

    // FUN_10149950: the first of the row's +0x1B0 ids that is a row, else the id itself.
    uint32_t SwapTarget(uint32_t id)
    {
        if (Row r = FindRow(id))
            for (uint32_t i = 0; i < 3; ++i)
                if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
                    if (FindRow(m))
                        return m;
        return id;
    }

    // FUN_10149820: the existing entry is held above the rank it keeps, and every update raises its
    // target's rank.
    bool SwapValid(const SwapData& d, const Build& b)
    {
        if (!FindRow(d.existing.entry) || b.RankOf(d.existing.entry) <= d.existing.rank)
            return false;
        for (const SwapEntry& u : d.updated)
        {
            if (!FindRow(u.entry))
                return false;
            if (b.RankOf(SwapTarget(u.entry)) >= u.rank)
                return false;
        }
        return true;
    }

    // FUN_101488f0: drop the existing entry to the rank it keeps, or remove it together with the mastery
    // it belongs to (itself when it is one, else its first known) and everything listing that mastery.
    void RemoveSwapped(Build& b, const SwapEntry& existing)
    {
        if (existing.rank)
        {
            b.SetRank(existing.entry, existing.rank);
            return;
        }
        Row r = FindRow(existing.entry);
        if (!r)
            return;
        uint32_t mastery = RowHasFlag(r, 0x1000) ? existing.entry : 0;
        for (uint32_t i = 0; i < 3 && !mastery; ++i)
            if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
                if (b.Has(m))
                    mastery = m;
        if (!mastery)
        {
            b.Remove(existing.entry);
            return;
        }
        std::vector<uint32_t> doomed = {mastery};
        for (const Entry& e : b.entries)
            if (e.row)
                for (uint32_t i = 0; i < 3; ++i)
                    if (RowU32(e.row, 0x1B0 + i * 4) == mastery)   // FUN_101c64f0
                    {
                        doomed.push_back(e.id);
                        break;
                    }
        for (uint32_t d : doomed)
            b.Remove(d);
    }

    void ApplySwap(Build& b, const SwapData& d)
    {
        RemoveSwapped(b, d.existing);
        for (const SwapEntry& u : d.updated)
        {
            const uint32_t target = SwapTarget(u.entry);
            if (b.Has(target))
                b.SetRank(target, u.rank);   // FUN_10158940
            else
                b.AddEntry(target, u.rank);  // FUN_10151190
        }
    }

    // FUN_10148dd0: the apply check (FUN_10151fd0) on a copy with the swap applied and reordered.
    uint32_t ValidateSwap(const SwapData& d, const Build& b, uint32_t& learnResult, uint32_t& id, uint32_t& rank)
    {
        Build copy(b, false);
        ApplySwap(copy, d);
        copy.Reorder(false);
        ApplyCosts costs;
        return ValidateApply(ActivePlayer(), b, copy.entries, learnResult, id, rank, costs);
    }

    // FUN_10172f50: {{ExistingEntry = {Entry, NewCARank}, UpdatedEntries = {{Entry, NewCARank}, ...}}, ...}.
    void PushSwapList(lua_State* L, const std::vector<SwapData>& list)
    {
        auto pushEntry = [&](const SwapEntry& e) {   // FUN_101730e0
            AscLua::lua_createtable(L, 0, 2);
            SetInt(L, "Entry", static_cast<int32_t>(e.entry));
            SetInt(L, "NewCARank", static_cast<int32_t>(e.rank));
        };
        AscLua::lua_createtable(L, static_cast<int>(list.size()), 0);
        for (size_t i = 0; i < list.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
            AscLua::lua_createtable(L, 0, 2);
            AscLua::lua_pushstring(L, "ExistingEntry");
            pushEntry(list[i].existing);
            AscLua::lua_settable(L, -3);
            AscLua::lua_pushstring(L, "UpdatedEntries");
            AscLua::lua_createtable(L, static_cast<int>(list[i].updated.size()), 0);
            for (size_t j = 0; j < list[i].updated.size(); ++j)
            {
                AscLua::lua_pushinteger(L, static_cast<int32_t>(j + 1));
                pushEntry(list[i].updated[j]);
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
    }

    int CanSwapEntriesByID(lua_State* L)   // FUN_10174f80
    {
        if (AscLua::lua_type(L, 1) != 5)
            return Usage(L, "Usage: C_CharacterAdvancement.CanSwapEntriesByID(data)");
        const SwapData d = ReadSwapData(L, 1);
        if (!g_mgr.pending)
            return Usage(L, "CanSwapEntriesByID: pending build is not available");
        if (!SwapValid(d, *g_mgr.pending))
            return Usage(L, "CanSwapEntriesByID: invalid swap data");
        uint32_t lr, id, rank;
        const uint32_t result = ValidateSwap(d, *g_mgr.pending, lr, id, rank);
        PushApplyResult(L, result, lr, id, rank);
        return 5;
    }

    int SwapEntriesByID(lua_State* L)   // FUN_1017f590
    {
        if (AscLua::lua_type(L, 1) != 5)
            return Usage(L, "Usage: C_CharacterAdvancement.SwapEntriesByID(data)");
        const SwapData d = ReadSwapData(L, 1);
        Build* b = g_mgr.pending;
        if (!b)
            return Usage(L, "SwapEntriesByID: pending build is not available");
        if (!SwapValid(d, *b))
            return Usage(L, "SwapEntriesByID: invalid swap data");
        uint32_t lr, id, rank;
        if (ValidateSwap(d, *b, lr, id, rank) != 0)
            return Usage(L, "SwapEntriesByID: swap not allowed");
        ApplySwap(*b, d);
        b->Reorder(false);
        BumpPendingVersion();
        return 0;
    }

    // FUN_10183a20: every entry of the existing one's kind in the class list that could replace it, each
    // at the highest rank that passes the apply check.
    int AvailableForSwap(lua_State* L, const SwapEntry& existing, uint32_t cls)
    {
        Build* b = g_mgr.pending;
        if (!b)
            return Usage(L, "GetEntriesAvailableForSwap: pending build is not available");
        SwapData d;
        d.existing = existing;
        if (!SwapValid(d, *b))
            return Usage(L, "GetEntriesAvailableForSwap: invalid swap data");
        d.updated.push_back({});
        std::vector<SwapData> results;
        results.reserve(100);
        Row existingRow = FindRow(existing.entry);
        Build copy(*b, false);
        RemoveSwapped(copy, existing);
        uint32_t lastId = 0, lastRank = 0;
        static const std::vector<Row> none;   // DAT_10bdd49c
        const std::vector<Row>* candidates = &none;
        auto it = g_byClass.find(cls);   // FUN_101496a0
        if (existingRow && it != g_byClass.end())
            candidates = &ListFor(it->second, existingRow);
        for (Row c : *candidates)
        {
            if (lastId)
            {
                if (lastRank == 0)
                    copy.Remove(lastId);
                else
                    copy.SetRank(lastId, lastRank);
            }
            d.updated.back().entry = RowU32(c, 0);
            const uint32_t target = SwapTarget(RowU32(c, 0));
            if (!FindRow(target))
                continue;
            lastId = target;
            lastRank = target == existing.entry ? existing.rank : copy.RankOf(target);
            if (lastRank == 0)
                copy.AddEntry(target, RowMaxRank(c));
            else
                copy.SetRank(target, RowMaxRank(c));
            for (uint32_t r = RowMaxRank(c); r != 0; --r)
            {
                d.updated.back().rank = r;
                if (!SwapValid(d, *b))
                    continue;
                copy.SetRank(target, r);
                copy.Reorder(false);
                uint32_t lr, id, rank;
                ApplyCosts costs;
                if (ValidateApply(ActivePlayer(), *b, copy.entries, lr, id, rank, costs) == 0)
                {
                    results.push_back(d);
                    break;
                }
            }
        }
        PushSwapList(L, results);
        return 1;
    }

    int GetEntriesAvailableForSwap(lua_State* L)   // handler_GetEntriesAvailableForSwap
    {
        if (AscLua::lua_type(L, 1) != 5)
            return Usage(L, "Usage: C_CharacterAdvancement.GetEntriesAvailableForSwap(existingEntry)");
        return AvailableForSwap(L, ReadSwapEntry(L, 1), 0);
    }

    int GetEntriesAvailableForSwapInClass(lua_State* L)   // FUN_10176cb0
    {
        if (!ValidateInput(L, {TABLE, STRING}))
            return Usage(L, "Usage: C_CharacterAdvancement.GetEntriesAvailableForSwapInClass(existingEntry, classType)");
        const SwapEntry existing = ReadSwapEntry(L, 1);
        const uint32_t cls = EnumArg(L, 2, kClassTypes, 0x2F);   // FUN_1016b250
        return AvailableForSwap(L, existing, cls);
    }

    // FUN_10176da0: every pending entry of the desired one's kind that could be traded for it, at the
    // highest workable rank (the requested one, else from its max down to 1).
    int GetEntriesAvailableForTrade(lua_State* L)
    {
        if (AscLua::lua_type(L, 1) != 5)
            return Usage(L, "Usage: C_CharacterAdvancement.GetEntriesAvailableForTrade(desiredEntry)");
        const SwapEntry desired = ReadSwapEntry(L, 1);
        Build* b = g_mgr.pending;
        if (!b)
            return Usage(L, "GetEntriesAvailableForTrade: pending build is not available");
        Row want = FindRow(desired.entry);
        if (!want)
            return Usage(L, "GetEntriesAvailableForTrade: invalid desired entryId");
        SwapData d;
        d.updated.push_back({desired.entry, 0});
        std::vector<SwapData> results;
        results.reserve(100);
        auto kind = [](Row r, int a, int b2) { const int t = RowType(r); return t == a || t == b2; };
        for (const Entry& e : b->entries)
        {
            if (!e.row || RowU32(e.row, 0) == desired.entry)
                continue;
            if (kind(e.row, 1, 4) != kind(want, 1, 4) || kind(e.row, 2, 2) != kind(want, 2, 2) || kind(e.row, 3, 3) != kind(want, 3, 3))
                continue;
            d.existing = {RowU32(e.row, 0), 0};
            uint32_t top = desired.rank ? desired.rank : RowMaxRank(want);
            const uint32_t low = desired.rank ? desired.rank : 1;
            if (e.rank < top)
                top = e.rank;
            for (uint32_t r = top; low <= r; --r)
            {
                d.updated.back().rank = r;
                if (!SwapValid(d, *b))
                    continue;
                uint32_t lr, id, rank;
                if (ValidateSwap(d, *b, lr, id, rank) == 0)
                {
                    results.push_back(d);
                    break;
                }
                if (r == 1)
                    break;
            }
        }
        PushSwapList(L, results);
        return 1;
    }


    // ---- inspect ------------------------------------------------------------------------------------------
    // FUN_1016fe70: SMSG 0x6E2 -- a CA_INSPECT_* string; on CA_INSPECT_OK u64 guid, u32 active spec, u32 spec
    // count and each spec's entries (the 0x726 list). The record goes into the inspect map (or the active
    // player's own); INSPECT_CHARACTER_ADVANCEMENT_RESULT carries the result as sent.
    void __cdecl OnInspectResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadCString(p);
        int index = -1;
        for (int i = 0; i < 6; ++i)
            if (result == kInspectResults[i])
            {
                index = i;
                break;
            }
        if (index < 0)
            AscLog::Printf("Unexpected Value: %s", result.c_str());
        else if (index == 0)
        {
            const uint64_t guid = Read<uint64_t>(p);
            const uint32_t spec = Read<uint32_t>(p);
            const uint32_t specs = Read<uint32_t>(p);
            Player* player = FindPlayer(guid);
            bool ok = true;
            if (!player)
            {
                Player*& slot = g_mgr.inspect[guid];   // FUN_10168540
                delete slot;
                slot = new Player(guid);
                player = FindPlayer(guid);
                ok = player != nullptr;
            }
            if (ok)
            {
                player->spec = spec;   // FUN_101499e0
                for (uint32_t i = 0; i < specs && ok; ++i)
                {
                    Build* b = player->Get(i);
                    if (!b)
                    {
                        player->Ensure(i);
                        b = player->Get(i);
                    }
                    if (!b)
                    {
                        ok = false;
                        break;
                    }
                    std::vector<Entry> fresh;   // FUN_10166760
                    const uint32_t n = Read<uint32_t>(p);
                    for (uint32_t k = 0; k < n; ++k)
                    {
                        Entry e;
                        e.id = Read<uint32_t>(p);
                        e.rank = Read<uint32_t>(p);
                        e.u0c = Read<uint32_t>(p);
                        e.locked = Read<uint8_t>(p);
                        e.u18 = Read<uint32_t>(p);
                        e.u1c = Read<uint32_t>(p);
                        fresh.push_back(e);
                    }
                    b->entries = fresh;   // FUN_10158010
                    b->UpdatePointers(false);
                }
                if (ok && guid == ActivePlayerGuid())
                    ResetPendingBuild();
            }
            if (!ok)
                return;   // the original returns before the event
        }
        AscRuntime::Signal("INSPECT_CHARACTER_ADVANCEMENT_RESULT", "%s", result.c_str());
    }

    int InspectUnit(lua_State* L)   // FUN_1017c380: CMSG 0x6E1 u64 guid
    {
        std::string token;
        if (!ReadString(L, token))
            return Usage(L, "Usage: C_CharacterAdvancement.InspectUnit(unitToken)");
        uint64_t guid;
        UnitTokenGuid(token.c_str(), guid);
        Packet(0x6E1).U64(guid).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_10177940: (active spec + 1, {specs with entries, 1-based}); nil, nil for an unknown unit.
    int GetInspectInfo(lua_State* L)
    {
        std::string token;
        if (!ReadString(L, token))
            return Usage(L, "Usage: C_CharacterAdvancement.GetInspectInfo(unitToken)");
        uint64_t guid;
        UnitTokenGuid(token.c_str(), guid);
        Player* player = FindPlayer(guid);
        if (!player)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 2;
        }
        PushInt(L, static_cast<int32_t>(player->spec + 1));
        std::vector<uint32_t> specs;
        for (uint32_t i = 0; i < 0x14; ++i)
            if (Build* b = player->Get(i))
                if (!b->entries.empty())
                    specs.push_back(i + 1);
        PushIntArray(L, specs);
        return 2;
    }

    // LAB_101803c0: abilities first, then talents, then by class type (+0x80), then higher rank first.
    bool InspectedOrder(const Entry& a, const Entry& b)
    {
        Row ra = FindRow(a.id), rb = FindRow(b.id);
        if (!ra || !rb)
            return false;
        auto ability = [](Row r) { const int t = RowType(r); return t == 1 || t == 4; };
        auto talent = [](Row r) { return RowType(r) == 2; };
        if (ability(ra) != ability(rb))
            return ability(ra) && !ability(rb);
        if (talent(ra) != talent(rb))
            return talent(ra) && !talent(rb);
        if (RowU32(ra, 0x80) != RowU32(rb, 0x80))
            return RowU32(ra, 0x80) < RowU32(rb, 0x80);
        return a.rank > b.rank;
    }

    // FUN_10177ae0: {{EntryId, Rank, Locked}, ...} for one of the unit's specs.
    // ---- build links -------------------------------------------------------------------------------
    std::string Format(const char* fmt, const std::vector<std::string>& args)   // FUN_1009e480 "{}"
    {
        std::string out;
        size_t next = 0;
        for (const char* p = fmt; *p; ++p)
            if (p[0] == '{' && p[1] == '}')
            {
                out += next < args.size() ? args[next++] : std::string();
                ++p;
            }
            else
                out += *p;
        return out;
    }

    std::string TrimWs(const std::string& s)   // FUN_101805a0 (" \t\r\n")
    {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return std::string();
        return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
    }

    // FUN_1016f050: the value of "<key>=" at the start, or after '#', '?' or '&', up to "& \t\r\n".
    std::string ParamValue(const std::string& s, const std::string& key)
    {
        const std::string k = key + "=";
        size_t at = std::string::npos;
        if (s.compare(0, k.size(), k) == 0)
            at = 0;
        else
            for (char sep : {'#', '?', '&'})
            {
                const size_t f = s.find(std::string(1, sep) + k);
                if (f != std::string::npos)
                {
                    at = f + 1;
                    break;
                }
            }
        if (at == std::string::npos)
            return std::string();
        const size_t from = at + k.size();
        const size_t end = s.find_first_of("& \t\r\n", from);
        return s.substr(from, end == std::string::npos ? std::string::npos : end - from);
    }

    // FUN_1016f880: what ImportPendingBuild does to the pasted text first.
    std::string NormaliseBuildUrl(const std::string& in)
    {
        std::string s = TrimWs(in);
        if (s.empty())
            return s;
        if (s.compare(0, 8, "talents=") == 0)
        {
            const size_t end = s.find_first_of("& \t\r\n", 8);
            return s.substr(8, end == std::string::npos ? std::string::npos : end - 8);
        }
        for (const char* key : {"#talents=", "?talents=", "&talents="})
        {
            const size_t f = s.find(key);
            if (f != std::string::npos)
            {
                const size_t from = f + 9;
                const size_t end = s.find_first_of("& \t\r\n", from);
                return s.substr(from, end == std::string::npos ? std::string::npos : end - from);
            }
        }
        const size_t cut = s.find_first_of("?#");
        if (cut != std::string::npos)
            s = s.substr(0, cut);
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        const size_t ov = s.size() >= 10 ? s.find("/overview/") : std::string::npos;
        if (ov != std::string::npos)
            return s.substr(ov + 10);
        if (s.compare(0, 7, "http://") == 0 || s.compare(0, 8, "https://") == 0)
        {
            const size_t slash = s.rfind('/');
            if (slash != std::string::npos)
                return s.substr(slash + 1);
        }
        return s;
    }

    // FUN_10182430: the whole string as a decimal u32 (std::stoul, which must consume every character).
    bool ParseU32(const std::string& s, uint32_t& out)
    {
        if (s.empty())
            return false;
        try
        {
            size_t used = 0;
            const unsigned long v = std::stoul(s, &used, 10);
            if (used != s.size())
                return false;
            out = static_cast<uint32_t>(v);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    // FUN_101817c0: a plain CoA build string (after the talents= parameter and URL decoding, '.' as
    // ':'): every token N, NtN or NeN and at least one talent token. Normalised to ":tok:...:".
    bool PlainCoALink(const std::string& url, std::string& out)
    {
        std::string s = TrimWs(url);
        if (s.empty())
            return false;
        const std::string v = ParamValue(s, "talents");
        if (!v.empty())
            s = v;
        s = AscBuildCreator::UrlDecode(s);
        for (char& c : s)
            if (c == '.')
                c = ':';
        while (!s.empty() && s.front() == ':')
            s.erase(0, 1);
        while (!s.empty() && s.back() == ':')
            s.pop_back();
        if (s.empty())
            return false;
        std::istringstream is(s);
        std::string tok, text;
        uint32_t count = 0, a = 0, b = 0;
        bool talent = false;
        while (std::getline(is, tok, ':'))
        {
            tok = TrimWs(tok);
            if (tok.empty())
                continue;
            const size_t t = tok.find('t'), e = tok.find('e');
            if (t == std::string::npos && e == std::string::npos)
            {
                for (char c : tok)
                    if (c < '0' || c > '9')
                        return false;
            }
            else if (t == std::string::npos)
            {
                if (!ParseU32(tok.substr(0, e), a) || !ParseU32(tok.substr(e + 1), b))
                    return false;
            }
            else if (e == std::string::npos)
            {
                if (!ParseU32(tok.substr(0, t), a) || !ParseU32(tok.substr(t + 1), b))
                    return false;
                talent = true;
            }
            else
                return false;
            text += ':' + tok;   // FUN_100a4fe0 + FUN_10167240
            ++count;
        }
        if (!count || !talent)
            return false;
        out = text + ':';
        return true;
    }

    std::string PreviewOf(const std::string& s)   // FUN_1016ef40
    {
        return s.size() < 0x61 ? s : s.substr(0, 0x60) + "...";
    }

    std::string NotYourClass(Row r, uint8_t cls)   // FUN_1016f780
    {
        if (r)
            if (const uint8_t* ct = ClassTypes().Row(RowU32(r, 0x80)))
                if (const uint32_t owner = RowU32(ct, 8))
                    return Format("belongs to CoA class {}, but your current class is {}",
                                  {std::to_string(owner), std::to_string(cls)});
        return Format("does not belong to your current CoA class {}", {std::to_string(cls)});
    }

    // FUN_1016de70: a new id is appended; a repeated rank-1 token adds a rank (capped at the row's
    // max), a repeated higher rank keeps the larger.
    void MergeEntry(std::vector<Entry>& v, uint32_t id, uint32_t rank)
    {
        for (Entry& e : v)
            if (e.id == id)
            {
                if (rank != 1)
                    e.rank = std::max(e.rank, rank);
                else if (Row r = FindRow(id))
                    e.rank = std::min(e.rank + 1, RowMaxRank(r));
                else
                    e.rank = e.rank + 1 == 0 ? 0xFFFFFFFF : e.rank + 1;
                return;
            }
        Entry e;
        e.row = FindRow(id);
        e.id = id;
        e.rank = rank;
        v.push_back(e);
    }

    bool RowHasSpell(Row r, uint32_t spell)   // FUN_101c6540: the nine rank spells at +0x14
    {
        for (uint32_t i = 0; i < 9; ++i)
            if (RowU32(r, 0x14 + i * 4) == spell)
                return true;
        return false;
    }

    // FUN_101806a0: the entries of a CoA build link for class `cls`, then the pending build's implicit
    // entries (flags 4 / 2 / 0x400000) the link lacks. False with the reason in `err`.
    bool ParseCoALink(const std::string& url, uint8_t cls, const Build* pending, std::vector<Entry>& entries,
                      std::string& err)
    {
        std::string text;
        if (!PlainCoALink(url, text))
            text = AscBuildCreator::DecodeLink(url);
        if (text.empty())
        {
            err = Format("Unable to decode CoA build link. Input length: {}. Input preview: {}",
                         {std::to_string(url.size()), PreviewOf(url)});
            return false;
        }
        std::istringstream is(text);
        std::string tok;
        uint32_t done = 0;
        AscDbc::Table& rows = Rows();
        while (std::getline(is, tok, ':'))
        {
            if (tok.empty())
                continue;
            const std::string n = std::to_string(done + 1);
            const size_t t = tok.find('t'), e = tok.find('e');
            if (t != std::string::npos)
            {
                uint32_t id = 0, rank = 0;
                if (!ParseU32(tok.substr(0, t), id) || !ParseU32(tok.substr(t + 1), rank))
                {
                    err = Format("Unable to parse CoA talent token: {}", {tok});
                    return false;
                }
                const Row r = FindRow(id);
                if (!r)
                {
                    err = Format("CoA build contains unknown entry {}. Parsed tokens before failure: {}. Payload preview: {}",
                                 {std::to_string(id), n, PreviewOf(text)});
                    return false;
                }
                if (!RowVisible(r) || !ClassTypeAdmits(r, cls))
                {
                    if (RowVisible(r))
                        err = Format("CoA build contains entry {} that {}. Parsed tokens before failure: {}.",
                                     {std::to_string(id), NotYourClass(r, cls), n});
                    else
                        err = Format("CoA build contains an entry unavailable on this realm: {}. Parsed tokens before failure: {}.",
                                     {std::to_string(id), n});
                    return false;
                }
                const int type = RowType(r);
                if (type < 1 || type > 4)
                {
                    err = Format("CoA build contains a non-advancement entry token: {}. Parsed tokens before failure: {}.",
                                 {std::to_string(id), n});
                    return false;
                }
                const uint32_t take = std::min(RowMaxRank(r), rank);
                if (!take)
                {
                    err = Format("CoA build contains an invalid rank for entry {}.", {std::to_string(id)});
                    return false;
                }
                MergeEntry(entries, id, take);
            }
            else if (e == std::string::npos)
            {
                uint32_t spell = 0;
                if (!ParseU32(tok, spell))
                {
                    err = Format("Unable to parse CoA spell token: {}", {tok});
                    return false;
                }
                Row found = nullptr;
                for (uint32_t id = rows.MinId(); id <= rows.MaxId() && !found; ++id)
                    if (Row r = FindRow(id))
                        if (RowVisible(r) && ClassTypeAdmits(r, cls) && RowType(r) == 1 && RowHasSpell(r, spell))
                            found = r;
                if (found)
                    MergeEntry(entries, RowU32(found, 0), 1);
                else
                {
                    for (uint32_t id = rows.MinId(); id <= rows.MaxId() && !found; ++id)
                        if (Row r = FindRow(id))
                            if (RowVisible(r) && RowType(r) == 1 && RowHasSpell(r, spell))
                                found = r;
                    if (found)
                        err = Format("CoA build contains ability spell {} from entry {} that {}.",
                                     {std::to_string(spell), std::to_string(RowU32(found, 0)), NotYourClass(found, cls)});
                    else
                        err = Format("CoA build contains an unknown ability spell for this class: {}. Parsed tokens before failure: {}.",
                                     {std::to_string(spell), n});
                    return false;
                }
            }
            ++done;   // enchant tokens (NeN) are counted and skipped
        }
        if (pending)
            for (const Entry& pe : pending->entries)
            {
                const Row r = pe.row ? pe.row : FindRow(pe.id);
                if (!r || !(RowHasFlag(r, 4) || RowHasFlag(r, 2) || RowHasFlag(r, 0x400000)) || !ClassTypeAdmits(r, cls))
                    continue;
                bool have = false;
                for (const Entry& x : entries)
                    have = have || x.id == pe.id;
                if (!have)
                    entries.push_back(pe);   // thunk_FUN_1014b750
            }
        if (entries.empty())
        {
            err = "No importable CoA tokens were found. Make sure the link is for your current CoA class and this client patch has those advancement entries.";
            return false;
        }
        return true;
    }

    // FUN_1017b910: a build link into the pending build. A CoA class reads the CoA link format; any
    // other class a Build Creator link, built at the player's level. The candidate is validated in
    // full on a copy of the pending build first. (false, reason | CA_LEARN_*, id, rank) on failure.
    int ImportPendingBuild(lua_State* L)
    {
        std::string url;
        if (!ReadString(L, url))
        {
            AscLua::luaL_error(L, "Usage: C_CharacterAdvancement.ImportPendingBuild(buildURL)");
            return 0;
        }
        url = NormaliseBuildUrl(url);
        Build* pending = g_mgr.pending;
        if (!pending)
        {
            AscLua::luaL_error(L, "ImportPendingBuild: pending build is not available");
            return 0;
        }
        const uint8_t* player = ActivePlayer();
        if (!player)
        {
            AscLua::luaL_error(L, "ImportPendingBuild: player not available");
            return 0;
        }
        const uint8_t cls = UnitClass(player);
        Build copy(*pending, true);
        if (cls >= 12 && cls <= 32)
        {
            std::vector<Entry> parsed;
            std::string err;
            if (!ParseCoALink(url, cls, pending, parsed, err))
            {
                PushBool(L, false);
                PushStr(L, err.c_str());
                PushNils(L, 2);
                return 4;
            }
            copy.entries = parsed;
            copy.Reorder(false);
        }
        else
        {
            const uint32_t level = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0xD8);
            Build b(AscBuildCreator::SeedFromLink(url), level, false);
            copy.entries = b.entries;
        }
        uint32_t eid = 0, rank = 0, result = 0;
        if (!copy.ValidateAll(true, {}, {}, false, eid, rank, result))
        {
            PushBool(L, false);
            PushStr(L, LearnName(result).c_str());
            PushInt(L, static_cast<int32_t>(eid));
            PushInt(L, static_cast<int32_t>(rank));
            return 4;
        }
        pending->entries = copy.entries;
        pending->Reorder(false);
        BumpPendingVersion();
        PushBool(L, true);
        PushNils(L, 3);
        return 4;
    }

    // ---- importing into the pending build -----------------------------------------------------------
    // FUN_1017c060: the Build Creator build with this id, built at the player's level and validated in
    // full; on success the pending build takes its entries. (false, CA_LEARN_*, id, rank) otherwise.
    int ImportPendingBuildID(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
        {
            AscLua::luaL_error(L, "Usage: C_CharacterAdvancement.ImportPendingBuildID(buildID)");
            return 0;
        }
        Build* pending = g_mgr.pending;
        if (!pending)
        {
            AscLua::luaL_error(L, "ImportPendingBuildID: pending build is not available");
            return 0;
        }
        const uint8_t* player = ActivePlayer();
        if (!player)
        {
            AscLua::luaL_error(L, "ImportPendingBuildID: player not available");
            return 0;
        }
        BuildSeed seed;
        if (!AscBuildCreator::SeedOfBuild(id, seed))
        {
            AscLua::luaL_error(L, "ImportPendingBuildID: invalid buildID");
            return 0;
        }
        const uint32_t level = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0xD8);
        Build b(seed, level, true);
        uint32_t eid = 0, rank = 0, result = 0;
        if (!b.ValidateAll(true, {}, {}, false, eid, rank, result))
        {
            PushBool(L, false);
            PushStr(L, LearnName(result).c_str());   // FUN_10169de0
            PushInt(L, static_cast<int32_t>(eid));
            PushInt(L, static_cast<int32_t>(rank));
            return 4;
        }
        pending->entries = b.entries;   // FUN_10158010
        pending->Reorder(false);
        BumpPendingVersion();
        PushBool(L, true);
        PushNils(L, 3);
        return 4;
    }

    int GetInspectedBuild(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return Usage(L, "Usage: C_CharacterAdvancement.GetInspectedBuild(unitToken, specialization)");
        const std::string token = CheckString(L, 1);
        const uint32_t spec = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        if (spec == 0)
            return Usage(L, "GetInspectedBuild: specialization must be >= 1");
        uint64_t guid;
        UnitTokenGuid(token.c_str(), guid);
        Player* player = FindPlayer(guid);
        Build* b = player ? player->Get(spec - 1) : nullptr;
        if (!b)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        std::vector<Entry> entries = b->entries;   // FUN_1014e460
        std::sort(entries.begin(), entries.end(), InspectedOrder);   // FUN_101681b0
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < entries.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetInt(L, "EntryId", static_cast<int32_t>(entries[i].id));
            SetInt(L, "Rank", static_cast<int32_t>(entries[i].rank));
            SetInt(L, "Locked", entries[i].locked);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // CA_GetCreditAmount (FUN_101a1d90): the credit map entry for the category, 0 when absent.
    int CA_GetCreditAmount(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const uint8_t category = static_cast<uint8_t>(ToInt(CheckNumber(L, 1)));
        auto it = g_credits.find(category);
        PushInt(L, it == g_credits.end() ? 0 : static_cast<int32_t>(it->second));
        return 1;
    }

    // SMSG 0x659 / 0x65B (FUN_101705e0 / FUN_10170dc0): u32 entry, C string result ->
    // CHARACTER_ADVANCEMENT_LOCK_ENTRY_RESULT / _UNLOCK_ENTRY_RESULT("%s%u", result, entry).
    void EntryResult(const char* event, CDataStore* p)
    {
        uint32_t entry;
        memcpy(&entry, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        const std::string result(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(result.size() + 1);
        AscRuntime::Signal(event, "%s%u", result.c_str(), entry);
    }
    void __cdecl OnLockEntryResult(void*, uint32_t, uint32_t, CDataStore* p) { EntryResult("CHARACTER_ADVANCEMENT_LOCK_ENTRY_RESULT", p); }
    void __cdecl OnUnlockEntryResult(void*, uint32_t, uint32_t, CDataStore* p) { EntryResult("CHARACTER_ADVANCEMENT_UNLOCK_ENTRY_RESULT", p); }

    std::string ReadCStr(CDataStore* p)
    {
        std::string v(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(v.size() + 1);
        return v;
    }

    // SMSG 0x72B (FUN_10170950): C string result -> CHARACTER_ADVANCEMENT_PURGE_TALENTS_RESULT("%b%s", ok,
    // result). ok is meant to be "result is CA_PURGE_TALENTS_OK", but the original pushes a dword whose low
    // byte alone holds it and whose upper bytes are a leftover pointer -- it is always true (IMPROVEMENTS).
    void __cdecl OnPurgeTalentsResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadCStr(p);
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_PURGE_TALENTS_RESULT", "%b%s", 1u, result.c_str());
    }

    // SMSG 0x72C (FUN_10170f30): C string result, C string, u32, u32 ->
    // CHARACTER_ADVANCEMENT_UPDATE_ENTRIES_RESULT("%b%s%s%u%u", result == CA_UPDATE_ENTRIES_OK, result, ...).
    void __cdecl OnUpdateEntriesResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        // The save's own answer, taken or refused: either way nothing is in flight any more.
        if (g_saveInFlight)
            AscLog::Printf("CoACompat: save answered, no build in flight");
        g_saveInFlight = false;
        const std::string result = ReadCStr(p);
        const std::string second = ReadCStr(p);
        uint32_t a, b;
        memcpy(&a, p->m_buffer + p->m_read, 4);
        memcpy(&b, p->m_buffer + p->m_read + 4, 4);
        p->m_read += 8;
        AscRuntime::Signal("CHARACTER_ADVANCEMENT_UPDATE_ENTRIES_RESULT", "%b%s%s%u%u", result == "CA_UPDATE_ENTRIES_OK" ? 1u : 0u,
                           result.c_str(), second.c_str(), a, b);
    }

    // 0x1016FDE0 (FUN_10171f60 -> list 0x10BE2DD8, after 0x80B5D0): for a player casting a spell whose
    // CA entry (FUN_101498d0 over the spell's first rank) lists point requirements (+0x1D8 class, +0x1E4
    // tab, +0x1F0 points, up to three, the first zero ends them), the active non-wildcard build must meet
    // each (FUN_101552e0 with no minimum).
    bool __cdecl CastRequirements(uint32_t unit, uint32_t rec, uint32_t, uint32_t, uint32_t)
    {
        if (!reinterpret_cast<char(__thiscall*)(void*)>(0x4CEE50)(reinterpret_cast<void*>(unit)))
            return true;
        AscCA::Build* build = AscCA::ActiveBuild();
        if (!build || build->wildcard)
            return true;
        AscCA::Row row = AscCA::RowBySpell(AscSpellRank::FirstRank(*reinterpret_cast<const uint32_t*>(rec)));
        if (!row)
            return true;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t points = AscCA::RowU32(row, 0x1F0 + i * 4);
            if (points == 0)
                return true;
            if (!build->MeetsPoints(AscCA::RowU32(row, 0x1D8 + i * 4), AscCA::RowU32(row, 0x1E4 + i * 4), points, 0))
                return false;
        }
        return true;
    }

    void Init()
    {
        AscRuntime::OnAfter80B5D0(&CastRequirements, 0x10171F65);
        sDC.AddPacketHandler(0x725, CNetClientCustomPacket((void*)&OnActiveSpec, nullptr));
        sDC.AddPacketHandler(0x659, CNetClientCustomPacket((void*)&OnLockEntryResult, nullptr));
        sDC.AddPacketHandler(0x72B, CNetClientCustomPacket((void*)&OnPurgeTalentsResult, nullptr));
        sDC.AddPacketHandler(0x72C, CNetClientCustomPacket((void*)&OnUpdateEntriesResult, nullptr));
        sDC.AddPacketHandler(0x65B, CNetClientCustomPacket((void*)&OnUnlockEntryResult, nullptr));
        sDC.AddPacketHandler(0x726, CNetClientCustomPacket((void*)&OnKnownEntries, nullptr));
        sDC.AddPacketHandler(0x7B4, CNetClientCustomPacket((void*)&OnPreviewState, nullptr));
        sDC.AddPacketHandler(0x728, CNetClientCustomPacket((void*)&OnLearnResult, nullptr));
        sDC.AddPacketHandler(0x729, CNetClientCustomPacket((void*)&OnUnlearnResult, nullptr));
        sDC.AddPacketHandler(0x72A, CNetClientCustomPacket((void*)&OnPurgeResult, nullptr));
        sDC.AddPacketHandler(0x926, CNetClientCustomPacket((void*)&OnCredit, nullptr));
        AscRuntime::OnGlueScreen(&ClearCredits);   // FUN_101a1b70
        sDC.AddPacketHandler(0x6CE, CNetClientCustomPacket((void*)&OnSpellSuggestion, nullptr));
        sDC.AddPacketHandler(0x6E2, CNetClientCustomPacket((void*)&OnInspectResult, nullptr));
        sDC.AddPacketHandler(0x6D2, CNetClientCustomPacket((void*)&OnStatSuggestion, nullptr));
        sDC.AddPacketHandler(0x64A, CNetClientCustomPacket((void*)&OnPatchEntry, nullptr));
        AscRuntime::OnEnterWorld(BuildIndex);   // 0x10172650 -> FUN_1014a530
        AscRuntime::OnGlueScreen(OnGlueScreen);
        AscRuntime::OnAfter403340(Update);
        // The suggestion manager's registration (0x10333880): glue reset, the tick, and a dirty mark on
        // every CA list change. Its two wildcard-manager lists (0x10be2cac / 0x10be2ccc) land with C_Wildcard.
        AscRuntime::OnGlueScreen(SuggestionsOnGlue);
        AscRuntime::OnAfter403340(SuggestionTick);
        for (std::vector<PairCallback>* list : {&g_onLearned, &g_onRankChanged, &g_onUnlearned, &g_onSpecChanged})
            list->push_back(SuggestionsDirty);
        AscRuntime::HookPacketHandler(0x6DF050, 6, &OnInitialSpells);
        AscRuntime::HookPacketHandler(0x6E7D60, 6, &OnLearnedSpell);
        AscRuntime::HookPacketHandler(0x6E2240, 5, &OnRemovedSpells);
        AscRuntime::HookPacketHandler(0x6E7840, 7, &OnRemovedSpell);
        AscRuntime::HookPacketHandler(0x6E7E00, 6, &OnSupersededSpell);
        g_5AAB90 = reinterpret_cast<Fn5AAB90_t>(AscRuntime::Detour(0x5AAB90, 5, reinterpret_cast<void*>(&Hook5AAB90)));
        RegisterIsItemAction();
    }

    const AscBindings::Binding kBindings[] = {
        {"C_CharacterAdvancement", "UnlearnAllSpells", UnlearnAllSpells},
        {"C_CharacterAdvancement", "UnlearnAllTalents", UnlearnAllTalents},
        {"C_CharacterAdvancement", "HasAnySuggestionContextOverrides", HasAnySuggestionContextOverrides},
        {"C_CharacterAdvancement", "IsSuggestionContextOverride", IsSuggestionContextOverride},
        {"C_CharacterAdvancement", "AddSuggestionContextOverride", AddSuggestionContextOverride},
        {"C_CharacterAdvancement", "RemoveSuggestionContextOverride", RemoveSuggestionContextOverride},
        {"C_CharacterAdvancement", "ClearSuggestionContextOverrides", ClearSuggestionContextOverrides},
        {"C_CharacterAdvancement", "IsPendingBuildAvailable", IsPendingBuildAvailable},
        {"C_CharacterAdvancement", "IsActiveBuildAvailable", IsActiveBuildAvailable},
        {"C_CharacterAdvancement", "GetGlobalAEInvestment", GetGlobalAEInvestment},
        {"C_CharacterAdvancement", "GetGlobalTEInvestment", GetGlobalTEInvestment},
        {"C_CharacterAdvancement", "GetRemainingAE", GetRemainingAE},
        {"C_CharacterAdvancement", "GetRemainingTE", GetRemainingTE},
        {"C_CharacterAdvancement", "GetExpectedAE", GetExpectedAE},
        {"C_CharacterAdvancement", "GetExpectedTE", GetExpectedTE},
        {"C_CharacterAdvancement", "GetClassAEInvestment", GetClassAEInvestment},
        {"C_CharacterAdvancement", "GetClassTEInvestment", GetClassTEInvestment},
        {"C_CharacterAdvancement", "GetClassPointInvestment", GetClassPointInvestment},
        {"C_CharacterAdvancement", "GetTabAEInvestment", GetTabAEInvestment},
        {"C_CharacterAdvancement", "GetTabTEInvestment", GetTabTEInvestment},
        {"C_CharacterAdvancement", "GetActiveSpecID", GetActiveSpecID},
        {"C_CharacterAdvancement", "IsLockedID", IsLockedID},
        {"C_CharacterAdvancement", "GetEntryBySpellID", GetEntryBySpellID},
        {"C_CharacterAdvancement", "GetEntryByInternalID", GetEntryByInternalID},
        {"C_CharacterAdvancement", "GetInternalID", GetInternalID},
        {"C_CharacterAdvancement", "IsAbilitySpellID", IsAbilitySpellID},
        {"C_CharacterAdvancement", "IsTalentSpellID", IsTalentSpellID},
        {"C_CharacterAdvancement", "IsTalentAbilitySpellID", IsTalentAbilitySpellID},
        {"C_CharacterAdvancement", "IsAbilityID", IsAbilityID},
        {"C_CharacterAdvancement", "IsTalentID", IsTalentID},
        {"C_CharacterAdvancement", "IsTalentAbilityID", IsTalentAbilityID},
        {"C_CharacterAdvancement", "IsMastery", IsMastery},
        {"C_CharacterAdvancement", "GetTalentEssenceCost", GetTalentEssenceCost},
        {"C_CharacterAdvancement", "GetAbilityEssenceCost", GetAbilityEssenceCost},
        {"C_CharacterAdvancement", "GetClassName", GetClassName},
        {"C_CharacterAdvancement", "GetTabName", GetTabName},
        {"C_CharacterAdvancement", "GetPendingGlobalAEInvestment", GetPendingGlobalAEInvestment},
        {"C_CharacterAdvancement", "GetPendingGlobalTEInvestment", GetPendingGlobalTEInvestment},
        {"C_CharacterAdvancement", "GetPendingRemainingAE", GetPendingRemainingAE},
        {"C_CharacterAdvancement", "GetPendingRemainingTE", GetPendingRemainingTE},
        {"C_CharacterAdvancement", "GetPendingExpectedAE", GetPendingExpectedAE},
        {"C_CharacterAdvancement", "GetPendingExpectedTE", GetPendingExpectedTE},
        {"C_CharacterAdvancement", "GetPendingClassAEInvestment", GetPendingClassAEInvestment},
        {"C_CharacterAdvancement", "GetPendingClassTEInvestment", GetPendingClassTEInvestment},
        {"C_CharacterAdvancement", "GetPendingClassPointInvestment", GetPendingClassPointInvestment},
        {"C_CharacterAdvancement", "GetPendingTabAEInvestment", GetPendingTabAEInvestment},
        {"C_CharacterAdvancement", "IsPendingEntryID", IsPendingEntryID},
        {"C_CharacterAdvancement", "GetPendingRankByEntryID", GetPendingRankByEntryID},
        {"C_CharacterAdvancement", "GetLowestInvestmentRequired", GetLowestInvestmentRequired},
        {"C_CharacterAdvancement", "GetActiveChrSpec", GetActiveChrSpec},
        {"C_CharacterAdvancement", "CancelPendingBuild", CancelPendingBuild},
        {"C_CharacterAdvancement", "ClearPendingBuild", ClearPendingBuild},
        {"C_CharacterAdvancement", "ClearPendingBuildByTab", ClearPendingBuildByTab},
        {"C_CharacterAdvancement", "AddByEntryID", AddByEntryID},
        {"C_CharacterAdvancement", "CanAddByEntryID", CanAddByEntryID},
        {"C_CharacterAdvancement", "MeetsInvestmentForAddByEntryID", MeetsInvestmentForAddByEntryID},
        {"C_CharacterAdvancement", "IsPending", IsPending},
        {"C_CharacterAdvancement", "GetPendingSummary", GetPendingSummary},
        {"C_CharacterAdvancement", "RemoveByEntryID", RemoveByEntryID},
        {"C_CharacterAdvancement", "CanRemoveByEntryID", CanRemoveByEntryID},
        {"C_CharacterAdvancement", "CanApplyPendingBuild", CanApplyPendingBuild},
        {"C_CharacterAdvancement", "CanClearPendingBuild", CanClearPendingBuild},
        {"C_CharacterAdvancement", "ApplyPendingBuild", ApplyPendingBuild},
        {"C_CharacterAdvancement", "GetEntriesByClass", GetEntriesByClass},
        {"C_CharacterAdvancement", "GetSpellsByClass", GetSpellsByClass},
        {"C_CharacterAdvancement", "GetTalentsByClass", GetTalentsByClass},
        {"C_CharacterAdvancement", "GetMasteriesByClass", GetMasteriesByClass},
        {"C_CharacterAdvancement", "GetImplicitByClass", GetImplicitByClass},
        {"C_CharacterAdvancement", "GetAllEntries", GetAllEntries},
        // A bare global in the original (FUN_10183e30 registers it after the table), despite its usage text.
        {nullptr, "IsSpellIDKnown", IsSpellIDKnown},
        // And as CA_IsSpellKnown in the world state (same handler 0x1017CC90; live capture 2026-09-27).
        {nullptr, "CA_IsSpellKnown", IsSpellIDKnown},
        {"C_CharacterAdvancement", "IsKnownID", IsKnownID},
        {"C_CharacterAdvancement", "IsKnownSpellID", IsKnownSpellID},
        {"C_CharacterAdvancement", "UnitKnownID", UnitKnownID},
        {"C_CharacterAdvancement", "UnitTalentRankByID", UnitTalentRankByID},
        {"C_CharacterAdvancement", "GetTalentRankByID", GetTalentRankByID},
        {"C_CharacterAdvancement", "GetTalentRankBySpellID", GetTalentRankBySpellID},
        {"C_CharacterAdvancement", "GetLearnedAE", GetLearnedAE},
        {"C_CharacterAdvancement", "GetLearnedTE", GetLearnedTE},
        {"C_CharacterAdvancement", "KnowsConnectedNodesFor", KnowsConnectedNodesFor},
        {"C_CharacterAdvancement", "GetKnownSpellEntries", GetKnownSpellEntries},
        {"C_CharacterAdvancement", "GetKnownTalentEntries", GetKnownTalentEntries},
        {"C_CharacterAdvancement", "GetKnownSpellEntriesForClass", GetKnownSpellEntriesForClass},
        {"C_CharacterAdvancement", "GetKnownTalentEntriesForClass", GetKnownTalentEntriesForClass},
        {"C_Spell", "HasRuneUI", HasRuneUI},
        {"C_CharacterAdvancement", "LearnID", LearnID},
        {"C_CharacterAdvancement", "CanLearnID", CanLearnID},
        {"C_CharacterAdvancement", "CanUnlearnID", CanUnlearnID},
        {"C_CharacterAdvancement", "LockID", LockID},
        {"C_CharacterAdvancement", "UnlockID", UnlockID},
        {"C_CharacterAdvancement", "ShouldConfirmLearnID", ShouldConfirmLearnID},
        {"C_CharacterAdvancement", "ShouldConfirmUnlearnID", ShouldConfirmUnlearnID},
        {"C_CharacterAdvancement", "ShouldConfirmUnlearnAllSpells", ShouldConfirmUnlearnAllSpells},
        {"C_CharacterAdvancement", "ShouldConfirmUnlearnAllTalents", ShouldConfirmUnlearnAllTalents},
        {"C_CharacterAdvancement", "CanUnlearnAllSpells", CanUnlearnAllSpells},
        {"C_CharacterAdvancement", "CanUnlearnAllTalents", CanUnlearnAllTalents},
        {"C_CharacterAdvancement", "UnlearnID", UnlearnID},
        {"C_CharacterAdvancement", "GetQualityCount", GetQualityCount},
        {"C_CharacterAdvancement", "GetQualityLimit", GetQualityLimit},
        {"C_CharacterAdvancement", "GetKnownSpells", GetKnownSpells},
        {"C_CharacterAdvancement", "IsConnectionAllowed", IsConnectionAllowed},
        {"C_CharacterAdvancement", "GetClassInfo", GetClassInfoBySpell},
        {nullptr, "GetClassInfo", GetClassInfo},   // FUN_100df760's global, handler_GetClassInfo
        {"C_CharacterAdvancement", "CanUseBrowser", CanUseBrowser},
        {"C_CharacterAdvancement", "GetRootSpellTagTypes", GetRootSpellTagTypes},
        {"C_CharacterAdvancement", "GetSpellTagTypes", GetSpellTagTypes},
        {"C_CharacterAdvancement", "GetSpellTagTypeDisplayInfo", GetSpellTagTypeDisplayInfo},
        {"C_CharacterAdvancement", "PickupSpell", PickupSpell},
        {"C_CharacterAdvancement", "GetSuggestedStats", GetSuggestedStats},
        {"C_CharacterAdvancement", "GetCategories", GetCategories},
        {"C_CharacterAdvancement", "GetCategoryDisplayInfo", GetCategoryDisplayInfo},
        {"C_CharacterAdvancement", "CanSwitchActiveChrSpec", CanSwitchActiveChrSpec},
        {"C_CharacterAdvancement", "SwitchActiveChrSpec", SwitchActiveChrSpec},
        {"C_CharacterAdvancement", "ActivateLoadout", ActivateLoadout},
        {"C_CharacterAdvancement", "SetLoadoutName", SetLoadoutName},
        {"C_CharacterAdvancement", "SetLoadoutSortOrder", SetLoadoutSortOrder},
        {"C_CharacterAdvancement", "CanSwapEntriesByID", CanSwapEntriesByID},
        {"C_CharacterAdvancement", "SwapEntriesByID", SwapEntriesByID},
        {"C_CharacterAdvancement", "GetEntriesAvailableForSwap", GetEntriesAvailableForSwap},
        {"C_CharacterAdvancement", "GetEntriesAvailableForSwapInClass", GetEntriesAvailableForSwapInClass},
        {"C_CharacterAdvancement", "GetEntriesAvailableForTrade", GetEntriesAvailableForTrade},
        {"C_CharacterAdvancement", "InspectUnit", InspectUnit},
        {"C_CharacterAdvancement", "GetInspectInfo", GetInspectInfo},
        {"C_CharacterAdvancement", "GetInspectedBuild", GetInspectedBuild},
        {"C_CharacterAdvancement", "ImportPendingBuildID", ImportPendingBuildID},
        {"C_CharacterAdvancement", "ImportPendingBuild", ImportPendingBuild},
        {"C_CharacterAdvancement", "GetPendingTabTEInvestment", GetPendingTabTEInvestment},
        {"C_CharacterAdvancement", "GetQualityInfo", GetQualityInfo},
        {"C_CharacterAdvancement", "ExportBuild", ExportBuild},
        {nullptr, "CA_GetCreditAmount", CA_GetCreditAmount},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

// ---- CoA compatibility (the realm's staged-preview protocol) ---------------------------------------
// The genuine client stages an archetype switch on its own pending build with
// SwitchActiveChrSpec, and that is all it can stage: `EnterSpec` adds the archetype's identity
// and signature rows and nothing else, while the rest of its default state - the tree's free
// rows, the passives the window draws at rank 1 - is the realm's data, not the client DBC's.
// So the realm stages the switch: this module sends CMSG 0x7B1 with the archetype, the realm
// answers SMSG 0x7B4 with that archetype's complete default state (handled in OnPreviewState
// above) and the pending build - the one Save Changes commits - holds exactly it. No client file is
// involved, and since a browse must not be mistaken for a switch, the window's identity is answered
// here as well: see `GetActiveChrSpec` below.
namespace AscCACompat
{
    using namespace AscCA;
    using AscScript::PushBool;        // The realm's custom classes are ids 12..32 (SharedDefines.h CLASS_BARBARIAN..CLASS_SPIRIT_MAGE);
        // the preview/reset protocol only applies to them.
        bool IsCoAPlayer()
        {
            const uint8_t* player = AscScript::ActivePlayer();
            return player && UnitClass(player) >= 12 && UnitClass(player) <= 32;
        }


    // The archetype a *staged preview* holds: the pending build names one that the realm's build
    // does not. Nothing has to remember the staging - the two builds say it - so the answer cannot
    // go stale: a pending build that comes back to the character's own archetype (the reset below
    // on the archetype already held, an undo, the save's own state push, a relog's first push)
    // stops counting the moment it does, and every caller below reads that one predicate.
    uint32_t PreviewSpec()
    {
        // A save in flight is not a browse: the build is on its way to being the character's.
        if (g_saveInFlight)
            return 0;
        if (!g_mgr.pending)
            return 0;
        const uint32_t staged = SpecOf(*g_mgr.pending);
        if (!staged)
            return 0;
        // The comparison needs the character's own archetype, and between the realm's active-spec
        // push (0x725) and the build that follows it (0x726) that archetype's build is still empty:
        // nothing is staged against anything there, so nothing is reported as a preview. Reading it
        // as one would drop the build the push has just announced in the window's identity reader.
        const uint32_t own = ActiveBuild() ? SpecOf(*ActiveBuild()) : 0;
        if (!own)
            return 0;
        return own == staged ? 0 : staged;
    }

    // One line per answer the window reads while an archetype is browsed, so the ordering of a
    // report ("the trees repaint", "the tag moves", "N opens the browsed one") can be read off
    // Logs\Extensions.log instead of guessed at. Silent while nothing is staged: the shipped
    // answers are the only ones in play then, and they are not ours to narrate.
    void LogAnswer(const char* question, const char* answer)
    {
        if (PreviewSpec())
            AscLog::Printf("CoACompat: %s -> %s (browse staged)", question, answer);
    }

    // The window's archetype click goes through SwitchActiveChrSpec. The realm owns the
    // archetype's defaults - they are its data, not the client DBC's - so the click is routed
    // there instead of staged locally: the realm answers 0x7B4 with that archetype's complete
    // default state, and the pending build, the one Save Changes commits, takes it in one
    // step. Nothing is bumped here on purpose: the click has not changed anything yet, and the
    // window reads a pending-build event as "the change you asked for is here" - it spends its
    // `expectingNewSpec` flag on it and repoints the tree at whatever the pending build holds,
    // which at that moment is still the old archetype.
    int SwitchActiveChrSpec(lua_State* L)
    {
        uint32_t spec;
        if (!ReadNumber(L, spec))
            return Usage(L, "Usage: C_CharacterAdvancement.SwitchActiveChrSpec(specID)");
        if (!IsCoAPlayer())
            return AscCA::SwitchActiveChrSpec(L);
        AscLog::Printf("CoACompat: SwitchActiveChrSpec(%u) -> CMSG 0x7B1", spec);
        AscScript::Packet(0x7B1).U32(spec).Send();
        return 0;
    }

    // The window's identity: the archetype the character is on - which, while an archetype is
    // browsed, is the archetype being browsed. Save Changes commits the *pending* build and prices
    // it against the character's own, so the identity has to follow the pending build: the staged
    // archetype is the archetype whose trees are drawn, whose ranks the window shows and whose
    // departure the next Save prices, and it is the archetype the realm announced for the browse
    // (0x725). An earlier pass answered the staged archetype to the single read that followed the
    // staging and the character's own to every other read, dropping the browse with it. The browse
    // then lived for exactly one read: the trees snapped back to the character's own archetype a
    // frame later, and a Save clicked after that read priced the character's own build against
    // itself, found no difference and applied nothing - the same switch saved on some clicks and did
    // nothing on others, with no cost ever named.
    //
    // A browse now ends where the player or the realm actually ends it: the chooser being shown, the
    // window closing, Undo (`C_CharacterAdvancement.CancelPendingBuild`), another archetype staged,
    // the character's own archetype staged, or the save's own push. Until one of those happens every
    // reader is told the archetype being browsed, so the window, the price and the charge are one
    // build. See `InstallBrowseEnds` in the Lua below for the two ends the window drives.
    int GetActiveChrSpec(lua_State* L)
    {
        if (IsCoAPlayer() && PreviewSpec())
        {
            const uint32_t staged = SpecOf(*g_mgr.pending);
            CoADbg("identity: staged %u [%s]", staged, CoADbgEntries(g_mgr.pending).c_str());
            LogAnswer("GetActiveChrSpec", "staged (browsing)");
            AscScript::PushInt(L, static_cast<int32_t>(staged));
            return 1;
        }
        return AscCA::GetActiveChrSpec(L);
    }

    // TEMPORARY, part of the browse/preview diagnostic: the window's save path decides between its
    // confirmation dialog and an immediate apply on its own (`CharacterAdvancementUtil
    // .ConfirmApplyPendingBuild` raises CONFIRM_APPLY_PENDING_BUILD for a nonzero cost and applies
    // silently for a zero one), so a save that charges without a dialog can only be explained by
    // reading that decision. The install script below logs it, and the popups the window raises, to
    // this same file the prices are written to.
    /* Whether that diagnostic writes, for the one part of it that lives in the client's Lua: that part
     * wraps two FrameXML globals to narrate the window's save path (the glue's `InstallSaveTrace`), and
     * a global replaced to narrate a trace that writes nothing is still a global replaced. */
    int CompatTraceEnabled(lua_State* L)
    {
        PushBool(L, CoATraceEnabled());
        return 1;
    }

    int CompatTrace(lua_State* L)
    {
        const char* message = AscLua::tostring(L, 1);
        CoADbg("%s", message ? message : "");
        return 0;
    }

    // Whether an archetype is being browsed: a staged preview names one that is not the character's
    // own (`PreviewSpec`). The window's own ends of a browse ask this before dropping one, so that
    // dropping a browse never touches a change the player has actually made.
    int IsBrowsingArchetype(lua_State* L)
    {
        PushBool(L, IsCoAPlayer() && PreviewSpec() != 0);
        return 1;
    }

    // A staged preview is not an unsaved change to the character's build. Everything that asks this
    // question - the Undo button, the save glow, the close prompt the window raises from OnHide -
    // describes the player's own tree, and a preview has not touched it: the realm still holds the
    // same build, closing the window simply drops the preview (`GetActiveChrSpec` above), and the way
    // back out of one is the realm's own card in the chooser, which the shipped click already asks
    // for. A real change to the character's own build still
    // answers true, and a preview is still commitable: Save Changes gates on `CanApplyPendingBuild`,
    // which prices the staged archetype, not on this.
    int IsPending(lua_State* L)
    {
        if (IsCoAPlayer() && PreviewSpec())
        {
            LogAnswer("IsPending", "false (a browse is not a change)");
            PushBool(L, false);
            return 1;
        }
        /* The realm announces an archetype before it writes that archetype's build (0x725 then 0x726),
         * and the window reads this question in between - the specialization signal drives
         * `UpdateActiveSpec`, whose `ShowTreeView` runs `UpdateCommitButtons`. With no build for the
         * announced archetype the shipped answer compares the pending build against an empty one and
         * says yes, so the window flashed Undo and the save glow, and raised the close prompt if the
         * player closed it in that instant, for a change the realm was already writing. There is
         * nothing to be pending against until the build exists, so the answer is no; the very next
         * packet answers it for real. */
        if (IsCoAPlayer() && g_mgr.player && !g_mgr.player->Get(g_mgr.player->spec))
        {
            LogAnswer("IsPending", "false (the realm is writing this archetype's build)");
            PushBool(L, false);
            return 1;
        }
        return AscCA::IsPending(L);
    }

    // Undo, and the close of a window that was browsing: the shipped discards the pending build and
    // rebuilds it from the character's own, which is exactly right - what is added here is the
    // announcement. `ResetPendingBuild` only bumps the version, so the window kept drawing the
    // discarded build until the manager's next tick (up to a second), and the trees, the commit
    // buttons and the point counters were all a tick behind a change the player had already made.
    int CancelPendingBuild(lua_State* L)
    {
        const int pushed = AscCA::CancelPendingBuild(L);
        FlushPendingBuildSignal(true);
        return pushed;
    }

    // The save: the window hands the pending build to the realm (CMSG 0x727) and the realm answers
    // with the build it settled on. From the send until that answer, the pending build is the
    // character's - it is the one being committed - so nothing may treat it as a browse (see
    // `g_saveInFlight`). Set after the shipped handler returns, never before: a refusal inside it
    // raises through Lua and would otherwise leave the flag set for a packet that never went out.
    int ApplyPendingBuild(lua_State* L)
    {
        if (!IsCoAPlayer())
            return AscCA::ApplyPendingBuild(L);
        const int pushed = AscCA::ApplyPendingBuild(L);
        g_saveInFlight = true;
        AscLog::Printf("CoACompat: ApplyPendingBuild -> CMSG 0x727, save in flight");
        return pushed;
    }
}

namespace AscCACoAGlue
{
    // Installed once the world state exists (OnWorldRegistered). This is DLL-side glue for
    // the popups the client build never shipped (their registrations lived in the retired
    // consumer add-on): the priced apply and reset confirmations and the close prompt. The
    // window's own code asks for them by name - `CharacterAdvancementUtil.ConfirmApplyPendingBuild`
    // shows CONFIRM_APPLY_PENDING_BUILD with the cost the realm's rules computed,
    // `CoATalentFrameMixin:OnHide` shows the close prompt when IsPending - so without them the
    // save and reset flows ended silently, with the cost never named. No client file is
    // modified: the code is registered into the live Lua state from the DLL, exactly the way
    // the original Extensions.dll ran its own Lua.
    //
    // Nothing here decides what the window shows. The window's identity, the archetype its trees
    // repoint at and the end of a browse are answered by natives (see AscCACompat above), so this
    // glue no longer hooks the window at all and a hook that installs too early - the frame may not
    // exist yet when the world state is built - can no longer leave the window on the wrong
    // archetype.
    void Install()
    {
        // FrameScript_Execute 0x819210(code, 0, 0) - the same call AscRealmData::RunLua makes.
        const char* const code =
            "-- CoA character advancement compatibility (installed by Extensions.dll)\n"
            "local dialogsDone = false\n"\
            "local function ApplyPending()\n"\
            "  C_CharacterAdvancement.ApplyPendingBuild()\n"\
            "  if BuildCreatorUtil and BuildCreatorUtil.GetPendingBuildID and C_BuildCreator then\n"\
            "    local id = BuildCreatorUtil.GetPendingBuildID()\n"\
            "    if id then\n"\
            "      C_BuildCreator.ActivateBuild(id, true, true)\n"\
            "      BuildCreatorUtil.ClearPendingBuildID()\n"\
            "    end\n"\
            "  end\n"\
            "end\n"\
            "local function Dialog(key, text, onAccept)\n"\
            "  if StaticPopupDialogs[key] then return end\n"\
            "  StaticPopupDialogs[key] = {\n"\
            "    text = _G[key] or text,\n"\
            "    button1 = ACCEPT, button2 = CANCEL,\n"\
            "    timeout = 0, whileDead = 1, hideOnEscape = 1, exclusive = 1, showAlert = 1,\n"\
            "    OnAccept = onAccept,\n"\
            "  }\n"\
            "end\n"\
            "local function InstallDialogs()\n"\
            "  if dialogsDone or not StaticPopupDialogs then return end\n"\
            "  dialogsDone = true\n"\
            "  Dialog('CONFIRM_APPLY_PENDING_BUILD', 'Apply these changes?\\n\\n%s', ApplyPending)\n"\
            "  Dialog('CONFIRM_RESET_BUILD', 'Reset this build?\\n\\n%s', ApplyPending)\n"\
            "  Dialog('CONFIRM_RESET_BUILD_NO_COST', 'Reset this build?', ApplyPending)\n"\
            "  Dialog('CLOSE_CHARACTER_ADVANCEMENT_UNSAVED_PENDING_CHANGES', 'You have unsaved changes. Close anyway?', function()\n"\
            "    C_CharacterAdvancement.CancelPendingBuild()\n"\
            "    local parent = CoATalentFrame and CoATalentFrame:GetParent()\n"\
            "    if parent then HideUIPanel(parent) end\n"\
            "  end)\n"\
            "end\n"\
            "-- One order for the trees' own refresh pass. `TalentTreeBaseMixin:Update` spends ONE dirty\n"\
            "-- reason per frame and its rebuild branch wipes the whole table - a rebuild is meant to\n"\
            "-- supersede a nodes refresh - but nothing enforces that order when both are pending, and the\n"\
            "-- window itself sets both: `ShowTreeView` marks `Nodes` on both trees and the repoint that\n"\
            "-- follows marks `RebuildTree` on the spec tree. The spec tree then refreshed the previous\n"\
            "-- archetype's nodes against the new build and rebuilt them on the following frame - the\n"\
            "-- second tree visibly repopulating, most visibly when the window is reopened onto the\n"\
            "-- character's own archetype. The rebuild is consumed first here, which is the order the\n"\
            "-- shipped pass already implies. Installed once; every mark below re-arms it.\n"\
            "local TreeReason = TalentTreeBaseMixin and TalentTreeBaseMixin.DirtyReason\n"\
            "local TreeNodes = TreeReason and TreeReason.Nodes or 1\n"\
            "local TreeRebuild = TreeReason and TreeReason.RebuildTree or 2\n"\
            "local TreeGates = TreeReason and TreeReason.RebuildGates or 3\n"\
            "-- The install is asked for again before every mark, and this call is only the first ask:\n"\
            "-- the mixin is not in the login state's Lua yet on some logins, where the guarded\n"\
            "-- install silently did nothing and the shipped pass stayed in place.\n"\
            "local function InstallTreeOrder()\n"\
            "  -- In place is the mixin's own method being this pass, not a flag saying it once was.\n"\
            "  -- The trees run whatever function the mixin holds, and the add-on's files re-executing\n"\
            "  -- hands the mixin a fresh table whose Update is the shipped one - a flag would then\n"\
            "  -- vouch for an install that is no longer there, and the ordering this exists for would\n"\
            "  -- be silently lost while nothing looked wrong.\n"\
            "  local mixin = TalentTreeBaseMixin\n"\
            "  if not (mixin and mixin.Update) then return false end\n"\
            "  if mixin.Update == CoACompatTreeOrder then return true end\n"\
            "  local base = mixin.Update\n"\
            "  CoACompatTreeOrder = function(self)\n"\
            "    -- A nodes refresh is only ever valid on a tree whose frames are already built, and\n"\
            "    -- both the rebuild and the gate build wipe and refill the frames it indexes\n"\
            "    -- (`BuildTree` empties `gateCache`, `CreateGates` gives each entry its gate).\n"\
            "    -- Cheap to drop: the rebuild drew the nodes itself, and `CreateGates` ends in\n"\
            "    -- `UpdateGates`.\n"\
            "    if self.dirty and (self.dirty[TreeRebuild] or self.dirty[TreeGates]) then\n"\
            "      self.dirty[TreeNodes] = nil\n"\
            "    end\n"\

            "    return base(self)\n"\
            "  end\n"\
            "  mixin.Update = CoACompatTreeOrder\n"\
            "  return true\n"\
            "end\n"\
            "InstallTreeOrder()\n"\
            "-- The other half of the same ordering: a nodes pass ends in `UpdateGates`, which reads\n"\
            "-- `gateInfo.gate` for every cached gate, and only the gate build gives an entry its frame.\n"\
            "-- The rebuild refills the cache and marks the gate build, so any pass that reaches the\n"\
            "-- nodes branch first - the dirty table is walked with `next`, and that order is the\n"\
            "-- table's own - indexes a gate that has never been created: 'TalentTreeBase.lua attempt\n"\
            "-- to index local gate (a nil value)' out of `UpdateGates`, on the archetype whose tree\n"\
            "-- was just repointed. The order above is what the shipped pass implies and it is\n"\
            "-- installed by every mark; this is the invariant itself, held at the read site so it\n"\
            "-- holds whatever pass happens to be in effect, including the shipped one on a session\n"\
            "-- whose window was drawn before the order could be installed. Safe to re-enter: the\n"\
            "-- gate build ends in this same call, which then finds every entry framed.\n"\
            "local function InstallGateGuard()\n"\
            "  -- Verified the same way, and for the same reason (see InstallTreeOrder).\n"\
            "  local mixin = TalentTreeBaseMixin\n"\
            "  if not (mixin and mixin.UpdateGates) then return false end\n"\
            "  if mixin.UpdateGates == CoACompatGateGuard then return true end\n"\
            "  local base = mixin.UpdateGates\n"\
            "  local building = false\n"\
            "  CoACompatGateGuard = function(self)\n"\
            "    if not building and self.useGates and self.gateCache and self.getGateTemplate\n"\
            "        and self.getGateAttachmentPoint and self.gateCurrencyCount then\n"\
            "      for _, gateInfo in ipairs(self.gateCache) do\n"\
            "        if not gateInfo.gate then\n"\
            "          building = true\n"\
            "          self:CreateGates()\n"\
            "          building = false\n"\
            "          break\n"\
            "        end\n"\
            "      end\n"\
            "    end\n"\
            "    return base(self)\n"\
            "  end\n"\
            "  mixin.UpdateGates = CoACompatGateGuard\n"\
            "  return true\n"\
            "end\n"\
            "InstallGateGuard()\n"\
            "-- The guards above are on the mixin, and the trees do not read the mixin: `MixinAndLoadScripts`\n"\
            "-- in the tree templates copies it onto each frame as that frame is built, so a tree built\n"\
            "-- before this glue ran keeps the shipped pass for the whole session - and the window's own\n"\
            "-- `MarkDirty` re-arms `OnUpdate` from the frame's own field, which is that copy, undoing\n"\
            "-- the re-arm the mark below does. Hence the third install: the same two guards, on the\n"\
            "-- frames themselves, where nothing copies them away - the order on `Update` (what the\n"\
            "-- frame re-arms) and the gate build on `UpdateGates` (what the nodes branch reads through).\n"\
            "-- Idempotent, and re-asserted whenever the window is shown, when a build is written\n"\
            "-- (`CoACompatMarkTrees`), and at every install.\n"\
            "local function GuardTree(tree)\n"\
            "  if not tree or tree.coaTreeGuarded then return tree ~= nil end\n"\
            "  tree.coaTreeGuarded = true\n"\
            "  local pass = tree.Update\n"\
            "  if pass and pass ~= CoACompatTreeOrder then\n"\
            "    tree.Update = function(self)\n"\
            "      if self.dirty and (self.dirty[TreeRebuild] or self.dirty[TreeGates]) then\n"\
            "        self.dirty[TreeNodes] = nil\n"\
            "      end\n"\
            "      return pass(self)\n"\
            "    end\n"\
            "  end\n"\
            "  local gates = tree.UpdateGates\n"\
            "  if gates and gates ~= CoACompatGateGuard then\n"\
            "    local building = false\n"\
            "    tree.UpdateGates = function(self)\n"\
            "      if not building and self.useGates and self.gateCache and self.getGateTemplate\n"\
            "          and self.getGateAttachmentPoint and self.gateCurrencyCount then\n"\
            "        for _, gateInfo in ipairs(self.gateCache) do\n"\
            "          if not gateInfo.gate then\n"\
            "            -- A build that fails must not leave this flag set: the guard would then never\n"\
            "            -- build again, and every later read would index a gate that was never made.\n"\
            "            -- The failure itself still reaches whoever ran the pass.\n"\
            "            building = true\n"\
            "            local ok, err = pcall(self.CreateGates, self)\n"\
            "            building = false\n"\
            "            if not ok then error(err, 0) end\n"\
            "            break\n"\
            "          end\n"\
            "        end\n"\
            "      end\n"\
            "      return gates(self)\n"\
            "    end\n"\
            "  end\n"\
            "  -- Whatever the frame had armed is still spent, and now by the ordered pass.\n"\
            "  if tree.dirty and next(tree.dirty) and tree.Update then\n"\
            "    tree:SetScript('OnUpdate', tree.Update)\n"\
            "  end\n"\
            "  return true\n"\
            "end\n"\
            "local function GuardWindowTrees()\n"\
            "  local view = CoATalentFrame and CoATalentFrame.TreeView\n"\
            "  if not view then return false end\n"\
            "  GuardTree(view.ClassTree)\n"\
            "  GuardTree(view.SpecTree)\n"\
            "  return true\n"\
            "end\n"\
            "local function InstallTreeGuards()\n"\
            "  GuardWindowTrees()\n"\
            "  -- The window is built by the archetype add-on, which this glue can precede. Its own\n"\
            "  -- OnShow is where a window that appears later is caught: showing it is what rebuilds\n"\
            "  -- the trees, and this hook runs in that same frame, before the pass they arm.\n"\
            "  local frame = CoATalentFrame\n"\
            "  if not (frame and frame.HookScript) or frame.coaTreeGuardsHooked then return true end\n"\
            "  frame.coaTreeGuardsHooked = true\n"\
            "  frame:HookScript('OnShow', GuardWindowTrees)\n"\
            "  return true\n"\
            "end\n"\
            "InstallTreeGuards()\n"\
            "local function MarkTree(tree)\n"\
            "  if not (tree and tree.MarkDirty) then return end\n"\
            "  tree:MarkDirty(TreeNodes)\n"\
            "  -- Whatever the mark armed, the ordered pass is what runs.\n"\
            "  local order = CoACompatTreeOrder or tree.Update\n"\
            "  if order then tree:SetScript('OnUpdate', order) end\n"\
            "  -- Spent in this call, not on the next frame: the window is opened and drawn in one\n"\
            "  -- beat, and a rebuild left to OnUpdate paints the archetype it came from for the\n"\
            "  -- frame the window comes up on - the snap when it is reopened after a preview.\n"\
            "  if tree.Update then tree:Update() end\n"\
            "end\n"\
            "-- The two trees, told from the DLL which build they must draw (see AscCA::MarkWindowTrees).\n"\
            "-- A tree that is hidden when the build is written never hears the announcement - the trees\n"\
            "-- only subscribe to it in OnShow - so the mark is left on the tree itself, which the trees\n"\
            "-- keep while hidden and spend on the first frame they are visible. The class tree's tab is\n"\
            "-- the class, fixed for the window's whole life, so a new build changes only the ranks it\n"\
            "-- draws: it always takes `Nodes`. The spec tree takes `Nodes` only when it is already\n"\
            "-- drawing this spec; otherwise it is repointed here, which is the rebuild it needs, because\n"\
            "-- `SetSpecID` is the only call that repoints it and `ShowTreeView` - the path a click on the\n"\
            "-- character's own card takes - does not.\n"\
            "function CoACompatMarkTrees(specID)\n"\
            "  local view = CoATalentFrame and CoATalentFrame.TreeView\n"\
            "  if not view then return end\n"\
            "  InstallTreeOrder()    -- the installs the login state may have had to defer\n"\
            "  InstallTreeGuards()\n"\

            "  MarkTree(view.ClassTree)\n"\
            "  if not view.SpecTree then return end\n"\
            "  if view.specID == specID then\n"\
            "    MarkTree(view.SpecTree)\n"\
            "  else\n"\
            "    -- The repoint is the client's own `SetSpecID`, and it reads the spec's own record and\n"\
            "    -- DBC straight out of `GetSpecInfoByID`: an archetype this client's DBCs cannot draw\n"\
            "    -- would be indexed into a nil `specInfo.Spec` there, and a DBC this client has no\n"\
            "    -- file for would leave the tree tabless (`SetClassTab(classDBC, nil)`), which its own\n"\
            "    -- rebuild then asserts on. So the repoint is asked for only when the client can draw\n"\
            "    -- it, and the mark above still refreshes the ranks of the tree already drawn.\n"\
            "    local specInfo = C_ClassInfo and C_ClassInfo.GetSpecInfoByID and specID and specID ~= 0\n"\
            "        and C_ClassInfo.GetSpecInfoByID(specID)\n"\
            "    local specDBC = specInfo and specInfo.Spec and CharacterAdvancementUtil\n"\
            "        and CharacterAdvancementUtil.GetSpecDBCByFile and CharacterAdvancementUtil.GetSpecDBCByFile(specInfo.Spec)\n"\
            "    if view.SetSpecID and specInfo and specInfo.Class and specInfo.Name and specDBC then\n"\
            "      view:SetSpecID(specID)\n"\
            "      if view.SpecTree.Update then view.SpecTree:Update() end\n"\
            "    else\n"\
            "      local trace = C_CharacterAdvancement.CompatTrace\n"\
            "      if trace and specID and specID ~= 0 then\n"\
            "        trace('mark: spec ' .. tostring(specID) .. ' is not drawable by this client; tree left on ' .. tostring(view.specID))\n"\
            "      end\n"\
            "    end\n"\
            "  end\n"\
            "end\n"\
            "-- What ends a browse. A browse is answered as the window's identity until something ends\n"\
            "-- it, so its ends have to be named rather than left to whichever read of the identity\n"\
            "-- happens to come next (see AscCACompat::GetActiveChrSpec). Two of them are the window's\n"\
            "-- own: closing it, because reopening the window is asking about the character and not\n"\
            "-- about the archetype being browsed, and showing the archetype chooser, which is where\n"\
            "-- the player goes to pick another archetype or to come back to the character's own. Both\n"\
            "-- drop only a browse: a change the player has actually made (IsPending) is left alone,\n"\
            "-- and closing with one of those keeps the shipped unsaved-changes prompt. Hooked on the\n"\
            "-- frames, not on the mixin tables: the frames are built with `MixinAndLoadScripts`, so a\n"\
            "-- method replaced on the table afterwards is one the frame no longer calls.\n"\
            "local browseEndsDone = false\n"\
            "-- Global as well as called below: the staging that starts a browse installs these too\n"\
            "-- (`EnsureBrowseEnds`), because both events below can have passed by the time the window\n"\
            "-- this hooks exists, and a browse that outlives its window is what that cost.\n"\
            "function CoACompatInstallBrowseEnds()\n"\
            "  if browseEndsDone then return end\n"\
            "  local frame = CoATalentFrame\n"\
            "  if not (frame and frame.HookScript and frame.UpdateActiveSpec and frame.ShowSpecView\n"\
            "      and C_CharacterAdvancement.IsBrowsingArchetype) then return end\n"\
            "  local function EndBrowse(refresh)\n"\
            "    if not C_CharacterAdvancement.IsBrowsingArchetype() then return end\n"\
            "    C_CharacterAdvancement.CancelPendingBuild()\n"\
            "    if not refresh then return end\n"\
            "    -- The chooser paints a card per archetype from the window's identity, and its own OnShow\n"\
            "    -- has already run by this point - so the card the browse left behind was painted as\n"\
            "    -- Active, and the trees behind it were still the browsed archetype's. Ask the window to\n"\
            "    -- move back onto the character's own archetype and to paint the chooser once more: the\n"\
            "    -- refresh passes through the tree view on its way, hiding the chooser, so showing it\n"\
            "    -- again runs its OnShow a second time against the identity the browse no longer holds.\n"\
            "    frame:UpdateActiveSpec()\n"\
            "    frame:ShowSpecView()\n"\
            "  end\n"\
            "  if not frame.coaBrowseEndsHooked then\n"\
            "    frame.coaBrowseEndsHooked = true\n"\
            "    frame:HookScript('OnHide', function() EndBrowse(false) end)\n"\
            "  end\n"\
            "  -- Both hooks have to be in before this counts as installed: the chooser's own OnShow is\n"\
            "  -- what ends the browse the window's closing does not, and the window itself can be built\n"\
            "  -- before the chooser's frame is. If it is not there yet, the next staging retries.\n"\
            "  local specView = frame.SpecView\n"\
            "  if not (specView and specView.HookScript and specView.OnShow) then return end\n"\
            "  specView:HookScript('OnShow', function() EndBrowse(true) end)\n"\
            "  browseEndsDone = true\n"\
            "  local trace = C_CharacterAdvancement.CompatTrace\n"\
            "  if trace then trace('install: browse ends hooked (window OnHide, chooser OnShow)') end\n"\
            "end\n"\
            "-- The browse/preview diagnostic, when it is asked for (COA_COMPAT_TRACE): what the window's\n"\
            "-- own save path\n"\
            "-- decided, and every popup it raised. `ConfirmApplyPendingBuild` is the window's own\n"\
            "-- gate - it raises `CONFIRM_APPLY_PENDING_BUILD` when the price it reads is nonzero and\n"\
            "-- applies the build straight away when it is zero - so a save that takes a cost without\n"\
            "-- a dialog is read off these lines against the `canApply`/`apply` prices. Wrapped on the\n"\
            "-- utility table and the popup global, both FrameXML's, so no binding of ours is replaced\n"\
            "-- and a FrameXML reload re-installs this like the rest.\n"\
            "local function InstallSaveTrace()\n"\
            "  if CoACompatSaveTrace then return end\n"\
            "  local trace = C_CharacterAdvancement.CompatTrace\n"\
            "  local enabled = C_CharacterAdvancement.CompatTraceEnabled\n"\
            "  -- Asked for, or this is not installed at all: it replaces two FrameXML globals, and a\n"\
            "  -- global replaced to narrate a trace that writes nothing is still a global replaced.\n"\
            "  if not (trace and enabled and enabled() and CharacterAdvancementUtil\n"\
            "      and CharacterAdvancementUtil.ConfirmApplyPendingBuild and StaticPopup_Show) then return end\n"\
            "  CoACompatSaveTrace = true\n"\
            "  local shown = StaticPopup_Show\n"\
            "  StaticPopup_Show = function(which, arg1, arg2, data)\n"\
            "    trace('popup ' .. tostring(which) .. ' | ' .. tostring(arg1))\n"\
            "    return shown(which, arg1, arg2, data)\n"\
            "  end\n"\
            "  local confirm = CharacterAdvancementUtil.ConfirmApplyPendingBuild\n"\
            "  CharacterAdvancementUtil.ConfirmApplyPendingBuild = function(...)\n"\
            "    local _, _, _, _, _, marks, gold = C_CharacterAdvancement.CanApplyPendingBuild()\n"\
            "    local priced = (marks and marks > 0) or (gold and gold > 0)\n"\
            "    trace('confirm: marks=' .. tostring(marks) .. ' gold=' .. tostring(gold) .. ' -> '\n"\
            "      .. (priced and 'DIALOG' or 'SILENT APPLY'))\n"\
            "    return confirm(...)\n"\
            "  end\n"\
            "end\n"\
            "-- One step at a time: these hook the window's own functions, and a step that fails used\n"\
            "-- to take every later one with it - the browse ends among them, which is how a browse\n"\
            "-- could outlive the window it belonged to. A failure is named, not swallowed.\n"\
            "local function Note(msg)\n"\
            "  local trace = C_CharacterAdvancement.CompatTrace\n"\
            "  if trace then trace('install: ' .. msg) end\n"\
            "  print('CoACompat: ' .. msg)\n"\
            "end\n"\
            "local function InstallStep(name, fn)\n"\
            "  local ok, err = pcall(fn)\n"\
            "  if not ok then Note(name .. ' failed: ' .. tostring(err)) end\n"\
            "end\n"\
            "local function InstallAll()\n"\
            "  InstallStep('InstallDialogs', InstallDialogs)\n"\
            "  InstallStep('InstallTreeOrder', InstallTreeOrder)\n"\
            "  InstallStep('InstallGateGuard', InstallGateGuard)\n"\
            "  InstallStep('InstallTreeGuards', InstallTreeGuards)\n"\
            "  InstallStep('InstallBrowseEnds', CoACompatInstallBrowseEnds)\n"\
            "  InstallStep('InstallSaveTrace', InstallSaveTrace)\n"\
            "end\n"\
            "local f = CreateFrame('Frame')\n"\
            "f:RegisterEvent('ADDON_LOADED')\n"\
            "f:RegisterEvent('PLAYER_ENTERING_WORLD')\n"\
            "f:SetScript('OnEvent', InstallAll)\n"\
            "InstallAll()\n";
        reinterpret_cast<void(__cdecl*)(const char*, int, int)>(0x819210)(code, 0, 0);
        // The helper above exists now, so the marks can be asked for.
        AscCA::g_windowTrees = true;
    }
    bool s_installed = false;
}

// The binding below registers after the genuine module's own (AscBindings registers the modules
// in construction order, so this one reroutes the entries it owns) and installs the Lua glue once
// the world state is built. Each entry is a single question the window asks and the shipped answer
// gets wrong for a staged archetype: which archetype the character is on (`GetActiveChrSpec`),
// whether a browse is an unsaved change (`IsPending`), what a click on an archetype card asks the
// realm for (`SwitchActiveChrSpec`) and what closing a browse discards (`CancelPendingBuild`).
// `IsBrowsingArchetype` is the one the DLL adds for itself: the window's own ends of a browse ask it
// (see `InstallBrowseEnds`) so they can drop a browse without touching a change the player made.
namespace
{
    const AscBindings::Binding kCoACompatBindings[] = {
        {"C_CharacterAdvancement", "SwitchActiveChrSpec", AscCACompat::SwitchActiveChrSpec},
        {"C_CharacterAdvancement", "CancelPendingBuild", AscCACompat::CancelPendingBuild},
        {"C_CharacterAdvancement", "IsPending", AscCACompat::IsPending},
        {"C_CharacterAdvancement", "GetActiveChrSpec", AscCACompat::GetActiveChrSpec},
        {"C_CharacterAdvancement", "IsBrowsingArchetype", AscCACompat::IsBrowsingArchetype},
        {"C_CharacterAdvancement", "ApplyPendingBuild", AscCACompat::ApplyPendingBuild},
        // The browse/preview diagnostic (see AscCACompat::CoATraceEnabled): the writer, and whether it
        // writes at all, which is what the glue's save-path wrappers ask before installing.
        {"C_CharacterAdvancement", "CompatTrace", AscCACompat::CompatTrace},
        {"C_CharacterAdvancement", "CompatTraceEnabled", AscCACompat::CompatTraceEnabled},
    };
    void CoACompatInit()
    {
        if (!AscCACoAGlue::s_installed)
        {
            AscCACoAGlue::s_installed = true;
            AscBindings::OnWorldRegistered(&AscCACoAGlue::Install);
        }
    }
    AscBindings::Module s_coaCompatModule(kCoACompatBindings,
        sizeof(kCoACompatBindings) / sizeof(kCoACompatBindings[0]), &CoACompatInit);
}
// ---- for the browser filters (AscCAFilter.cpp) -----------------------------------------------------
bool HiddenFromIndex(Row r) { return HiddenBase(r, false); }                 // FUN_101c67c0(0)
bool HiddenFromSearch(Row r) { return Hidden(r, false); }                    // the gates FUN_101c1ce0 opens with
const char* RowText(Row r, uint32_t off) { return RowStr(r, off); }
void PushEntryTable(lua_State* L, Row r) { PushEntry(L, r); }                // FUN_10172cf0
std::vector<uint32_t> SuggestedFor(uint32_t category) { return SuggestedForCategory(category); }   // FUN_10333650
void OnEntryLearned(void (*cb)(uint32_t, uint32_t)) { g_onLearned.push_back(cb); }
void OnEntryUnlearned(void (*cb)(uint32_t, uint32_t)) { g_onUnlearned.push_back(cb); }
void OnSpecChanged(void (*cb)(uint32_t, uint32_t)) { g_onSpecChanged.push_back(cb); }
void SuppressLearnSounds(bool on) { SuppressLearnCalls(on); }
bool SpellCustomAttr18(uint32_t spell, uint32_t mask) { return CustomAttrC(spell, mask); }
const uint8_t* SpellCustomAttrRow(uint32_t spell) { return CustomAttr(spell); }
void RebuildSpellCustomAttr() { BuildCustomAttr(); }
// FUN_10217510: every occurrence of `tag` leaves the spell's list (the list itself stays).
void SpellTagRemove(uint32_t spell, uint32_t tag)
{
    auto it = SpellTagMap().find(spell);
    if (it != SpellTagMap().end())
        it->second.erase(std::remove(it->second.begin(), it->second.end(), tag), it->second.end());
}
// FUN_102175b0: appended unless the spell already lists it.
void SpellTagAdd(uint32_t spell, uint32_t tag)
{
    auto it = SpellTagMap().find(spell);
    if (it != SpellTagMap().end() && std::find(it->second.begin(), it->second.end(), tag) != it->second.end())
        return;
    SpellTagMap()[spell].push_back(tag);
}
// FUN_10182ff0: the known-tag set (+0x428) cleared and refilled from the known spells (+0x3E8).
void RebuildKnownTags()
{
    g_knownTags.clear();
    for (uint32_t spell : g_knownSpells)
        for (uint32_t tag : SpellTagsOf(spell))
            g_knownTags.insert(tag);
}
const std::vector<Row>& IndexedRows() { return g_visible; }
const char* RowIconPtr(Row r) { return RowStr(r, 0xC0); }
uint32_t SpellAtRankOf(Row r, uint32_t rank) { return SpellAtRank(r, rank); }
std::string QualityName(int q)
{
    const uint8_t i = static_cast<uint8_t>(q);
    return i < 9 ? std::string(kQualities[i]) : "UNEXPECTED_ENUM_VALUE_" + std::to_string(i);
}

// FUN_10152dc0: the spells an entry currently gives -- its rank's spell, or that spell's rank chain
// cut to the learned rank (+0xC) -- and FUN_101528d0 takes the last.
uint32_t EntryCurrentSpell(const Build& b, uint32_t id)
{
    const Entry* e = nullptr;
    for (const Entry& x : b.entries)
        if (x.id == id)
        {
            e = &x;
            break;
        }
    Row r = e ? FindRow(id) : nullptr;
    if (!r || e->rank == 0)
        return 0;
    uint32_t maxRank = 0;   // FUN_101c6030
    while (maxRank < 9 && RowU32(r, 0x14 + maxRank * 4))
        ++maxRank;
    if (e->rank > maxRank)
        return 0;
    const uint32_t spell = RowU32(r, 0x10 + e->rank * 4);
    const uint32_t learned = e->u0c;   // FUN_10152f90
    if (!spell || !learned)
        return 0;
    const std::vector<uint32_t>& chain = AscSpellRank::Chain(spell);
    if (chain.empty())
        return spell;
    const size_t n = learned <= chain.size() ? learned : chain.size();
    return n ? chain[n - 1] : 0;
}
std::vector<uint32_t> SuggestedList(bool abilities, uint32_t max, bool skipKnown) { return SuggestedEntries(abilities, max, skipKnown); }   // FUN_10333770
const std::vector<uint32_t>& SpellTagTypesOf(uint32_t spell) { return SpellTagsOf(spell); }        // FUN_10217450
bool RequestInFlight(uint32_t id) { return InFlight(id) != nullptr; }        // FUN_10a31720
uint32_t ModeRequiredLevel(Row r) { return RowU32(r, ModeColumn(0x68, 0x6C, 0x70)); }
void MarkSuggestionsDirty() { SuggestionsDirty(0, 0); }   // 0x10333950 via the wildcard lists 0x10be2cac / 0x10be2ccc
}

uint32_t AscCA_RemovingSpell() { return AscCA::g_removingSpell; }   // 0x10BDEBC4
