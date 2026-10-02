#pragma once
// The original's CharacterAdvancementMgr (static 0x10bde440, accessor FUN_101722e0) and the objects it
// owns, transcribed from the decompile. See AscCAMgr.cpp.
//
//   Row     a DBFilesClient\CharacterAdvancement.dbc record as the DLL holds it: the 0x1FC-byte struct
//           its parser FUN_101c9120 builds (AscDbcLayouts), not the 0x2B4-byte file row. Every CA
//           function passes these pointers around.
//   Build   CharacterAdvancementBuild, 0x278 bytes (ctor FUN_1014f820, copy FUN_1014eb80, from a unit
//           FUN_1014e4d0): owner GUID, spec, class, mode flags, level, credits and the entry list.
//   Player  the per-GUID record, 0x20 bytes (FUN_10148650): up to 20 spec builds + the active spec.
#include <cstdint>
#include <unordered_set>
#include <utility>
#include <string>
#include <vector>

struct lua_State;

namespace AscCA
{
    typedef const uint8_t* Row;

    Row FindRow(uint32_t id);                 // CharacterAdvancement.dbc rows[id - min], nullptr if absent
    inline uint32_t RowU32(Row r, uint32_t off) { return *reinterpret_cast<const uint32_t*>(r + off); }

    // FUN_1014e160 over the type string at row +4: 0 None, 1 Ability, 2 Talent, 3 Trait,
    // 4 TalentAbility; -1 when the string is none of them.
    int RowType(Row r);
    inline bool RowHasFlag(Row r, uint32_t mask) { return (RowU32(r, 0x124) & mask) != 0; }   // FUN_101c64e0

    // CharacterAdvancementBuildEntry (0x20 bytes).
    struct Entry
    {
        Row row = nullptr;     // +0x00, re-resolved by UpdatePointers (FUN_10158e40)
        uint32_t id = 0;       // +0x04
        uint32_t rank = 0;     // +0x08
        uint32_t u0c = 0;      // +0x0C
        uint8_t locked = 0;    // +0x10
        uint32_t u14 = 0;      // +0x14
        uint32_t u18 = 0;      // +0x18
        uint32_t u1c = 0;      // +0x1C
    };

    // What FUN_1014f0f0 reads from a CBuildCreatorEntry: +0x60 class, +0xF0 category, +0xF8 primary stat,
    // the +0xFC spell list {spell, level} and +0 the id.
    struct BuildSeed
    {
        uint32_t classKey = 0;
        uint32_t category = 0;
        uint32_t primaryStat = 0;
        std::vector<std::pair<uint32_t, uint32_t>> spells;
        std::string id;
    };

    class Build
    {
    public:
        Build();                                     // FUN_1014f820 (the config-driven defaults)
        Build(const Build& src, bool full);          // FUN_1014eb80
        // FUN_1014f0f0: a build of a Build Creator entry at `lvl` (0 = the server max level): every
        // spell at or below that level adds a rank of its row; a Hero adds its primary-stat entry.
        Build(const BuildSeed& seed, uint32_t lvl, bool full);
        ~Build();
        Build& operator=(const Build&) = delete;

        uint64_t serial = 0;       // +0x00, registered in the live-build map (DAT_10bdd5c0)
        uint64_t guid = 0;         // +0x08
        int32_t spec = -1;         // +0x10
        uint32_t classKey = 0;     // +0x14  unit class byte
        uint8_t draft = 0;         // +0x18
        uint8_t wildcard = 0;      // +0x19
        uint8_t specDraft = 0;     // +0x1A
        uint8_t qualities = 0;     // +0x1B
        uint8_t hero = 0;          // +0x1C  class 10
        uint8_t coa = 0;           // +0x1D  class 12..32
        uint8_t stock = 0;         // +0x1E  class 1..9, 11
        uint8_t ready = 0;         // +0x1F  set by FUN_10157450
        std::vector<Entry> entries;   // +0x20
        uint32_t u2c = 0;          // +0x2C  unit addon field 2
        uint32_t level = 0;        // +0x30
        uint32_t credit[4] = {};   // +0x34..+0x40
        uint32_t u44 = 0, u48 = 0; // +0x44, +0x48
        std::string name;          // +0x4C
        uint8_t u64 = 0, u65 = 0;  // +0x64, +0x65

        // Entry list (FUN_10152920 / FUN_10155060 / FUN_10155640 / FUN_101550c0).
        uint32_t RankOf(uint32_t id) const;
        bool Has(uint32_t id) const;
        bool LockedOf(uint32_t id) const;
        bool HasGroup(uint32_t group, uint32_t exceptId) const;

        // Essence budget from CharacterAdvancementEssence.dbc (FUN_10153190 / FUN_10153340 at the build's
        // level, FUN_101530b0 / FUN_10153260 at an explicit one).
        uint32_t AEBudget(uint32_t lvl) const;
        uint32_t TEBudget(uint32_t lvl) const;

        // Spent essence (FUN_10153b20 & co). A zero minimum disables the "row +off < minimum" filter.
        uint32_t GlobalAE(uint32_t minimum) const;
        uint32_t GlobalTE(uint32_t minimum) const;
        uint32_t ClassAE(uint32_t cls, uint32_t minimum) const;
        uint32_t ClassTE(uint32_t cls, uint32_t minimum) const;
        uint32_t ClassPoints(uint32_t cls, uint32_t minimum) const;
        uint32_t TabAE(uint32_t cls, uint32_t tab, uint32_t minimum) const;
        uint32_t TabTE(uint32_t cls, uint32_t tab, uint32_t minimum) const;
        uint32_t RemainingAE() const;               // FUN_10153a90
        uint32_t RemainingTE() const;               // FUN_10153ac0
        bool MeetsPoints(uint32_t cls, uint32_t tab, uint32_t points, uint32_t minimum) const;   // FUN_101552e0

        // FUN_101525f0 / FUN_10154350: the mode's cost column, waived for a wildcard choice group this
        // build already holds.
        uint32_t AECost(Row r, uint32_t ranks) const;
        uint32_t TECost(Row r, uint32_t ranks) const;
        uint32_t AEColumn(Row r, uint32_t ranks) const;
        uint32_t TEColumn(Row r, uint32_t ranks) const;

        // FUN_10158e40: re-resolve every entry's row; drop entries whose row is gone (kept on a dev realm
        // when `keepMissing`).
        void UpdatePointers(bool keepMissing);

        // ---- learn validation (AscCARules.cpp) ----------------------------------------------------
        // FUN_10157450: the "full" build preparation -- rule sets and result caches, then ready. A
        // non-full copy has no rules, so it validates everything as learnable (FUN_10151480's guard).
        void Prepare();
        uint64_t learnUnitRules = 0;    // +0x1A8: bit n = a rule in CA_LEARN_* slot n, run on the unit
        uint64_t learnBuildRules = 0;   // +0x1B4: run on the build
        bool learnCacheOn = false;      // +0x250
        std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> learnCache;   // +0x230: id -> {rank, result}

        // FUN_10151480: the first failing CA_LEARN_* slot for learning one more rank of `id` (0 = ok).
        // `include` empty = every slot; `exclude` empty = none excluded. `unit` null = the owner's.
        uint32_t ValidateLearn(void* unit, uint32_t id, const std::vector<uint32_t>& include,
                               const std::vector<uint32_t>& exclude);
        uint64_t learnMask = 0;         // +0x1C0 bitset: the slots FUN_1015cbf0 marks as it installs them
        uint32_t unlearnUnitRules = 0;  // +0x1D0: bit n = a rule in CA_UNLEARN_* slot n, run on the unit
        uint32_t unlearnBuildRules = 0; // +0x1DC
        std::vector<std::pair<uint32_t, uint32_t>> unlearnCache;   // +0x254 (on with learnCacheOn): id -> result

        // FUN_10151a50: the first failing CA_UNLEARN_* slot for removing `id` (0 = ok).
        uint32_t ValidateUnlearn(void* unit, uint32_t id, const std::vector<uint32_t>& include,
                                 const std::vector<uint32_t>& exclude);
        // Purge rules (FUN_1015cbf0): abilities +0x1E8 unit / +0x1F4 build, talents +0x200 / +0x20C;
        // bit n = CA_PURGE_{ABILITIES,TALENTS}_* slot n.
        uint32_t purgeUnitRules[2] = {};
        uint32_t purgeBuildRules[2] = {};
        // FUN_10151870 (abilities) / FUN_10151960 (talents): the first failing purge slot (0 = ok).
        uint32_t ValidatePurge(bool talents, void* unit) const;
        // FUN_101551a0 / FUN_101554a0: at least `amount` AE / TE spent (on rows whose +0x88 / +0x8C
        // is below `below`, 0 = all), group-shared wildcard costs counted once.
        bool SpentAtLeast(bool talents, uint32_t amount, uint32_t below) const;
        // FUN_10158050: re-learn every entry on an emptied full copy (in the current order, or the Reorder
        // pass when !inOrder). maskMode also excludes the installed slots the +0x1C0 mask leaves out (and
        // LOW_LEVEL on a draft build). False with the first failing {id, rank, result}.
        bool ValidateAll(bool inOrder, const std::vector<uint32_t>& include, std::vector<uint32_t> exclude,
                         bool maskMode, uint32_t& id, uint32_t& rank, uint32_t& result) const;
        // FUN_101731e0: remove an entry together with the mastery bookkeeping it carries.
        void RemoveWithMastery(uint32_t id);
        bool MasteryInvolved(uint32_t id) const;          // FUN_10173c80
        uint32_t MasteryTarget(uint32_t id) const;        // FUN_1016f4f0
        bool ClassFusion() const;                         // FUN_10172490
        std::vector<Entry> TakeMasteryBearers();          // FUN_101738f0
        void ReaddWhileValid(const std::vector<Entry>& saved);   // FUN_10173e20
        // FUN_101560d0 (OptimizeForTraversal): Reorder that reports the first step it could not place.
        bool Optimize(bool restoreOnFail, uint32_t& result, uint32_t& id, uint32_t& rank);

        // FUN_101557d0: reorder the entries into an order they can be learned in (ranks expanded to single
        // steps, sorted, then placed while each validates). False when some step never validates.
        bool Reorder(bool restoreOnFail);

        // FUN_10156b50: make the build learnable (ValidateAll in order, CA_LEARN slot 0x2C excluded) while
        // keeping every `keep` id: as it is, reordered, or with the fewest rank units outside `keep`
        // removed (FUN_10150af0 per combination). False, with the entries unchanged, when nothing works.
        bool TraverseOrFix(const std::vector<uint32_t>& keep);

        // Entry mutation (FUN_10151190 add at a rank, FUN_10158940 set rank, FUN_10151390 add one rank);
        // each forgets the entry's cached learn result.
        void AddEntry(uint32_t id, uint32_t rank);
        void SetRank(uint32_t id, uint32_t rank);
        void AddRank(uint32_t id);
        void Remove(uint32_t id);         // FUN_10157c30
        void ForgetResult(uint32_t id);   // FUN_10158a60(id, 1, 0)

        // The four std::function hooks ResetPendingBuild installs (+0x68/+0xE0/+0x108/+0x130); only the
        // pending build has them (AscCARules.cpp: OnBeforeAdd / OnAfterAdd / OnAfterRank / OnAfterRemove).
        bool pendingHooks = false;

    private:
        void Register();
    };

    int ActiveSpecIndex();     // FUN_1016f580: the player record's active spec, -1 without one
    Build* ActiveBuild();      // FUN_1016f390: the player's build for the active spec, else the default build
    // The build's ability rows (types 1 and 4), what +0x44 caches. The original fills +0x44 only on the
    // first pending-build update after the known entries arrive, so a check right after a fresh client
    // start reads 0; the Wildcard starting phase checks count here instead.
    uint32_t AbilityCount(const Build& b);
    Build* PendingBuild();     // mgr +0x24, nullptr when none
    void ResetPendingBuild();  // FUN_10173d00

    // FUN_101498d0: the first entry (class-allowed rows sort first) listing this spell, or nullptr.
    Row RowBySpell(uint32_t spellId);
    bool IsCASpell(uint32_t spellId);              // FUN_101497c0: the spell is some CA entry's (0x10bdd540)
    // FUN_1008e5b0: the client spellbook (0x7260E0) or the CA known set has the spell -- every spell of its
    // chain when it heads one -- or, with `orBuild`, the active build holds the entry listing it.
    bool SpellKnown(uint32_t spellId, bool orBuild);
    // Manager +0x3E8 (FUN_1016fdb0 / FUN_101725e0): the CA-learned spell set.
    const std::unordered_set<uint32_t>& KnownSpells();

    // Shared with AscCARules.cpp.
    bool RowVisible(Row r);                        // FUN_101c6cc0
    uint32_t RowMaxRank(Row r);                    // FUN_101c6030
    bool ClassTypeAdmits(Row r, uint8_t cls);      // FUN_101c6a40
    uint8_t UnitClassOf(const uint8_t* unit);      // unit type 3/4: UNIT_FIELD_BYTES_0 class, else 0
    Build* BuildOf(uint64_t guid);                 // FUN_1016f700 + FUN_10149480(active spec)
    void RequestAutoLearn();                       // mgr +0x458 = 1 (the pending hooks)

    // FUN_10151df0 + FUN_10151fd0 + FUN_101547e0: can `base` become `entries`? CA_UPDATE_ENTRIES_* (0 = ok);
    // on 3/5 the failing {learn result, id, rank}; the marks / money it would cost and token costs.
    struct TokenCost { uint32_t money, marks, item, count; };
    struct ApplyCosts { std::vector<TokenCost> tokens; uint32_t marks = 0, money = 0; };
    uint32_t ValidateApply(void* unit, const Build& base, const std::vector<Entry>& entries, uint32_t& learnResult,
                           uint32_t& id, uint32_t& rank, ApplyCosts& costs);
    uint64_t BumpPendingVersion();                 // DAT_10bde3d8++

    TokenCost UnlearnCostOf(const Build& b, Row r);        // FUN_10154650
    int QualityIndexOf(const Build& b, Row r);             // FUN_101539b0
    uint32_t QualityCostOf(const Build& b, Row r);         // FUN_101539f0
    uint32_t QualitySumOf(const Build& b, int quality);    // FUN_10153f60
    uint32_t RequiredLevelOf(const Build& b, Row r);       // FUN_10153af0
    uint32_t PoolLevelOf(const Build& b, bool talent);     // FUN_10153510 (ability) / FUN_10153750 (talent)
    bool ClassAllowedForPlayer(Row r);                     // FUN_101c6af0
    void MarkSuggestionsDirty();                           // the suggestion cache, on an undesired-list change
    // The CA manager's lists fired from 0x726 (per entry learned / unlearned: id, rank) and 0x725 (a
    // spec change: old, new) -- FUN_102784b0 / FUN_102784d0 / FUN_102784f0.
    void OnEntryLearned(void (*cb)(uint32_t, uint32_t));
    void OnEntryUnlearned(void (*cb)(uint32_t, uint32_t));
    void OnSpecChanged(void (*cb)(uint32_t, uint32_t));
    const std::vector<Row>& IndexedRows();                 // 0x10bdd514: the entry index's visible rows
    const char* RowIconPtr(Row r);                         // +0xC0 as stored (compared by pointer)
    uint32_t SpellAtRankOf(Row r, uint32_t rank);          // FUN_101c62e0
    uint32_t EntryCurrentSpell(const Build& b, uint32_t id);   // FUN_101528d0 (last of FUN_10152dc0)
    std::string QualityName(int q);                        // 0x10b2d710, UNEXPECTED_ENUM_VALUE_ past 8
    void SuppressLearnSounds(bool on);
    bool SpellCustomAttr18(uint32_t spell, uint32_t mask);   // SpellCustomAttr.dbc +0x18 & mask
    const uint8_t* SpellCustomAttrRow(uint32_t spell);
    void SpellTagRemove(uint32_t spell, uint32_t tag);   // FUN_10217510
    void SpellTagAdd(uint32_t spell, uint32_t tag);      // FUN_102175b0
    void RebuildKnownTags();                             // FUN_10182ff0
    void RebuildSpellCustomAttr();   // FUN_1020b080 (e.g. after SMSG 0x5F4 overwrites a row)       // FUN_102a7f10: the spell's SpellCustomAttr.dbc row                     // FUN_10308830 (NOPs 0x5216F0's three call sites)
    TokenCost PurgeCost(const Build& b, bool talents);     // FUN_10152650 / FUN_101543b0

    // For the browser filters (AscCAFilter.cpp).
    bool HiddenFromIndex(Row r);                           // FUN_101c67c0(0): off this realm, or the class can't use it
    bool HiddenFromSearch(Row r);                          // FUN_101c6bd0: also the pending-build and mode gates
    const char* RowText(Row r, uint32_t off);              // a string field of the row ("" when none)
    void PushEntryTable(lua_State* L, Row r);              // FUN_10172cf0 (nil for a null row)
    std::vector<uint32_t> SuggestedFor(uint32_t category); // FUN_10333650: the category's top suggestions
    std::vector<uint32_t> SuggestedList(bool abilities, uint32_t max, bool skipKnown);   // FUN_10333770
    const std::vector<uint32_t>& SpellTagTypesOf(uint32_t spell);   // FUN_10217450
    bool RequestInFlight(uint32_t id);                     // FUN_10a31720: a learn/unlearn request is pending
    uint32_t ModeRequiredLevel(Row r);                     // +0x68 / +0x6C draft / +0x70 wildcard
    uint32_t UnitItemCount(const void* unit, uint32_t item);   // FUN_10308570 (active player only)
    const uint32_t kMarkOfAscensionItem = 0x5B9D2;
}
