// Quest log of 64 quests, part 2: the CGQuestLog list (build, sort, collapse, index lookups, per-quest queries) and
// its Lua API over 64 slots. Not in the genuine DLL; built with ASCENSION_QUEST_LOG_64. Each client function below is
// replaced whole (AscRuntime::ReplaceFunction) by a port of the CoA64 body (WoW/Source/Ui/QuestLog.cpp,
// QuestLog_Scripts.cpp; the address is its OFFSET tag), with 64-slot bounds and a DLL-owned list.
//
// The list. The client's CGQuestLog::m_quests (0xC237B0, 50 x 16 bytes, count 0xC23AD0) and m_sortTypes (0xC23618,
// 25 ints, count 0xC23AD4) are too small for 64 quests and their zone headers; the list below holds 128 rows and 64
// sort types (as CoA64 sizes them). Three techniques: functions with list logic or descriptor slot walks are ported;
// functions that only index the list get their operands pointed at the DLL list (kOperandPatches); the four
// leaderboard functions also get their one descriptor read replaced by a call (kSlotSites). The quest watch and POI
// functions (Tasks 11 and 12) still read the old arrays, so after every write the first 50 rows and 25 sort types are
// copied back there with the counts clamped (Mirror); those readers miss only rows 50+. See Sync for the list.
//
// The 25-bit collapse filter (cvar questLogCollapseFilter) is unchanged (spec non-goal): sort types past the 25th
// have no collapse bit and always show expanded.
#include <PatchConfig.hpp>

#if ASCENSION_QUEST_LOG_64
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscQuestLog64.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <Windows.h>

// The list itself, with no client access (tests/quest_log_64.py compiles this block).
namespace AscQuestLog64List
{
    constexpr uint32_t kRows = AscQuestLog64::kMaxSlots * 2;   // one quest plus at most one header per slot
    constexpr uint32_t kNativeRows = 50, kNativeSortTypes = 25;
    constexpr uint32_t kCollapseBits = 25;

    struct Row
    {
        int32_t questId;    // the sort type for a header
        int32_t logSlot;
        int32_t isHeader;
        int32_t expired;
    };
    static_assert(sizeof(Row) == 16, "CGQuestLog::m_quests entry");

    struct List
    {
        Row rows[kRows];
        uint32_t count;
        int32_t sortTypes[AscQuestLog64::kMaxSlots];
        uint32_t numSortTypes;
    };

    // The index of `type` among the first `limit` sort types; `limit` when absent.
    uint32_t FindSort(const List& l, int32_t type, uint32_t limit)
    {
        uint32_t i = 0;
        while (i < limit && l.sortTypes[i] != type)
            ++i;
        return i;
    }

    // Update's step for one quest: its row, then a header for its sort type when the type is new. Returns the
    // quest's row.
    uint32_t Append(List& l, int32_t questId, uint32_t slot, int32_t sortType)
    {
        const uint32_t row = l.count;
        if (l.count < kRows)
            l.rows[l.count++] = Row{questId, static_cast<int32_t>(slot), 0, 0};
        if (FindSort(l, sortType, l.numSortTypes) == l.numSortTypes && l.numSortTypes < AscQuestLog64::kMaxSlots)
        {
            l.sortTypes[l.numSortTypes++] = sortType;
            if (l.count < kRows)
                l.rows[l.count++] = Row{sortType, 0, 1, 0};
        }
        return row;
    }

    // A non-header row with this sort index is folded away (no collapse bit past the 25th type: never).
    bool Collapsed(uint32_t sortIndex, int32_t filter)
    {
        return sortIndex < kCollapseBits && !((1u << sortIndex) & static_cast<uint32_t>(filter));
    }

    // Update keeps the sort types it had, negated when expanded, to carry each header's state to its new index.
    void SaveSortTypes(const List& l, int32_t filter, int32_t* out)
    {
        for (uint32_t i = 0; i < l.numSortTypes; ++i)
            out[i] = i >= kCollapseBits || ((1u << i) & static_cast<uint32_t>(filter)) ? -l.sortTypes[i] : l.sortTypes[i];
    }

    // The filter after the sort types were rebuilt and sorted: a type seen collapsed stays collapsed, any other is
    // expanded. Bits only for the first 25 types.
    int32_t MergeFilter(const List& l, int32_t filter, const int32_t* old, uint32_t numOld)
    {
        uint32_t f = static_cast<uint32_t>(filter);
        for (uint32_t i = 0; i < l.numSortTypes && i < kCollapseBits; ++i)
        {
            uint32_t j = 0;
            for (; j < numOld; ++j)
            {
                if (old[j] == l.sortTypes[i])
                {
                    f &= ~(1u << i);
                    break;
                }
                if (old[j] == -l.sortTypes[i])
                {
                    f |= 1u << i;
                    break;
                }
            }
            if (j >= numOld)
                f |= 1u << i;
        }
        return static_cast<int32_t>(f);
    }

    // One row as QSortQuestLogEntries (0x5E08F0) sees it. `known`: the quest record is cached (headers: always).
    struct SortKey
    {
        bool header;
        bool known;
        uint32_t sort;     // index in the sort types, 0 when absent
        int32_t level;     // -1 already replaced by the player's level
        const char* name;
    };

    // The sort index QSortQuestLogEntries uses: searched over all 64 entries, 0 when absent.
    uint32_t SortIndex(const List& l, int32_t type)
    {
        const uint32_t i = FindSort(l, type, AscQuestLog64::kMaxSlots);
        return i < AscQuestLog64::kMaxSlots ? i : 0;
    }

    // QSortQuestLogEntries: shown rows before folded ones, then by sort type, header first, then by level, then name.
    int Compare(const SortKey& a, const SortKey& b, int32_t filter, int (*names)(const char*, const char*))
    {
        if ((!a.header && !a.known) || (!b.header && !b.known))
            return 0;
        const bool hiddenA = !a.header && Collapsed(a.sort, filter);
        const bool hiddenB = !b.header && Collapsed(b.sort, filter);
        if (hiddenA)
            return hiddenB ? 0 : 1;
        if (hiddenB)
            return -1;
        if (a.sort != b.sort)
            return a.sort < b.sort ? -1 : 1;
        if (a.header)
            return -1;
        if (b.header)
            return 1;
        if (a.level == b.level)
            return names(a.name, b.name);
        return a.level >= b.level ? 1 : -1;
    }

    // GetQuestIndex (0x5DEEB0): the 1-based row of a quest, 0 when absent. Walks every row, as the original walks
    // all 50.
    uint32_t QuestIndex(const List& l, int32_t questId)
    {
        for (uint32_t i = 0; i < kRows; ++i)
            if (!l.rows[i].isHeader && l.rows[i].questId == questId)
                return i + 1;
        return 0;
    }

    // The copy the client's own arrays keep for readers not yet ported: the first 50 rows and 25 sort types, counts
    // clamped, the rest zeroed.
    void Mirror(const List& l, Row* rows, uint32_t& count, int32_t* sortTypes, uint32_t& numSortTypes)
    {
        count = l.count < kNativeRows ? l.count : kNativeRows;
        memset(rows, 0, kNativeRows * sizeof(Row));
        memcpy(rows, l.rows, count * sizeof(Row));
        numSortTypes = l.numSortTypes < kNativeSortTypes ? l.numSortTypes : kNativeSortTypes;
        memset(sortTypes, 0, kNativeSortTypes * sizeof(int32_t));
        memcpy(sortTypes, l.sortTypes, numSortTypes * sizeof(int32_t));
    }

    // An operand that addressed the old list, pointed at the DLL list: 0xC23AD0 (the count) -> `count`, a row field
    // 0xC237B0 + k -> `rows` + k.
    uint32_t Relocated(uint32_t original, uint32_t rows, uint32_t count)
    {
        return original == 0xC23AD0 ? count : rows + (original - 0xC237B0);
    }

    enum X86Reg : uint8_t { kEax = 0, kEcx = 1, kEdx = 2, kEbx = 3, kEsi = 6, kEdi = 7 };

    // The code replacing a slot site at `start` (`size` bytes, filled with nops after it):
    //     push slot / push dword [ebp + player] / call target / add esp, 8 / [mov dest, eax] / jmp short done
    // Returns the bytes used (16 at most).
    uint32_t SlotSiteCode(uint8_t* code, uint32_t size, uint32_t start, uint32_t done, X86Reg slot, X86Reg dest, int8_t player,
                          uint32_t target)
    {
        memset(code, 0x90, size);
        uint32_t n = 0;
        code[n++] = static_cast<uint8_t>(0x50 + slot);
        code[n++] = 0xFF;
        code[n++] = 0x75;
        code[n++] = static_cast<uint8_t>(player);
        code[n++] = 0xE8;
        const uint32_t rel = target - (start + n + 4);
        memcpy(code + n, &rel, 4);
        n += 4;
        code[n++] = 0x83;
        code[n++] = 0xC4;
        code[n++] = 0x08;
        if (dest != kEax)
        {
            code[n++] = 0x8B;
            code[n++] = static_cast<uint8_t>(0xC0 | (dest << 3));
        }
        code[n++] = 0xEB;
        code[n] = static_cast<uint8_t>(done - (start + n + 1));
        return n + 1;
    }
}

namespace
{
    using namespace AscQuestLog64List;
    using AscQuestLog64::kMaxSlots;

    List s_list = {};

    // Client globals (CoA64 STATIC tags).
    Row* NativeRows() { return reinterpret_cast<Row*>(0xC237B0); }                       // m_quests[50]
    uint32_t& NativeCount() { return *reinterpret_cast<uint32_t*>(0xC23AD0); }            // m_numQuest
    int32_t* NativeSortTypes() { return reinterpret_cast<int32_t*>(0xC23618); }          // m_sortTypes[25]
    uint32_t& NativeNumSortTypes() { return *reinterpret_cast<uint32_t*>(0xC23AD4); }     // m_numSortTypes
    int32_t& Selected() { return *reinterpret_cast<int32_t*>(0xC23AD8); }                // m_selectedQuest
    int32_t& Abandon() { return *reinterpret_cast<int32_t*>(0xC23ADC); }                 // m_abandonQuest
    uint32_t& NumWatches() { return *reinterpret_cast<uint32_t*>(0xC23AE0); }            // m_numQuestWatches
    uint32_t& NumShown() { return *reinterpret_cast<uint32_t*>(0xC23AE4); }              // m_numShownQuests
    int32_t& TimeOffset() { return *reinterpret_cast<int32_t*>(0xC23AE8); }              // m_timeOffset
    uint8_t& TrackedLoaded() { return *reinterpret_cast<uint8_t*>(0xC23AF0); }           // m_trackedQuestsLoaded
    uint32_t& CallbackCount() { return *reinterpret_cast<uint32_t*>(0xC23AF4); }         // s_questCallbackCount
    int32_t& NextQuestUpdate() { return *reinterpret_cast<int32_t*>(0xC23AF8); }         // m_nextQuestUpdate
    int32_t WatchQuest(uint32_t i) { return *reinterpret_cast<int32_t*>(0xC23680 + i * 12); }   // m_questWatches[i]
    uint8_t* CollapseCVar() { return *reinterpret_cast<uint8_t**>(0xBD0A34); }           // s_cvQuestLogCollapseFilter
    int32_t CollapseFilter() { return *reinterpret_cast<int32_t*>(CollapseCVar() + 0x30); }

    constexpr int kQuestLogUpdate = 293;   // SCRIPT_EVENT_QUEST_LOG_UPDATE
    constexpr uint32_t kTypePlayer = 0x10;
    const char kFile[] = "AscQuestLog64List.cpp";

    // Client functions (all checked in IDA, 2026-11-25).
    uint8_t* Player()
    {
        const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)()>(0x4D3790)();   // ClntObjMgrGetActivePlayer
        return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t, const char*, int)>(0x4D4DB0)(
            static_cast<uint32_t>(guid), static_cast<uint32_t>(guid >> 32), kTypePlayer, kFile, 0);
    }
    // g_questDBCache.GetRecord (0x67DE90 on 0xC5DA48); `callback` 0x5DE930 QuestQueryCallback, 0x5DE8F0 the counter.
    const uint8_t* Quest(int32_t id, uint32_t callback = 0)
    {
        uint32_t none[2] = {0, 0};
        return reinterpret_cast<const uint8_t*(__thiscall*)(void*, int32_t, uint32_t*, uint32_t, void*, int)>(0x67DE90)(
            reinterpret_cast<void*>(0xC5DA48), id, none, callback, nullptr, 0);
    }
    template <class T> T Q(const uint8_t* quest, uint32_t offset) { T v; memcpy(&v, quest + offset, sizeof(T)); return v; }
    // QuestCache fields (offsets read from the functions replaced here).
    constexpr uint32_t kQLevel = 0x08, kQSort = 0x10, kQType = 0x14, kQPlayers = 0x18, kQFriendly = 0x1C,
                       kQFriendlyAmount = 0x20, kQHostile = 0x24, kQHostileAmount = 0x28, kQMoney = 0x34,
                       kQStartItem = 0x4C, kQFlags = 0x50, kQName = 0xB4, kQObjective = 0x1C24, kQObjectiveCount = 0x1C34,
                       kQItem = 0x1C44, kQItemCount = 0x1C5C, kQPlayersKilled = 0x2098;
    const uint8_t* DbRecord(uint32_t db, int32_t id)   // WowClientDB::GetRecord 0x65C290
    {
        return reinterpret_cast<const uint8_t*(__thiscall*)(void*, int32_t)>(0x65C290)(reinterpret_cast<void*>(db), id);
    }
    void SetCollapseFilter(int32_t value)   // CVar::Set(value, 1, 0, 0, 1) 0x766940
    {
        reinterpret_cast<int(__thiscall*)(void*, int32_t, int, int, int, int)>(0x766940)(CollapseCVar(), value, 1, 0, 0, 1);
    }
    void Signal(int event) { reinterpret_cast<void(__cdecl*)(int)>(0x615020)(event); }   // Script_SendUniqueSignal
    void QueryQuestGiverStatus() { reinterpret_cast<void(__cdecl*)()>(0x6D50C0)(); }
    int SortNames(const char* a, const char* b)   // SStrSortUTF8I 0x76EC80 (stdcall)
    {
        return reinterpret_cast<int(__stdcall*)(const char*, const char*, int)>(0x76EC80)(a, b, 0x7FFFFFFF);
    }

    // The player's quest log entry for `slot` (0x14 bytes): 0..24 the descriptor, as CGPlayer::GetQuestLogEntry
    // (0x5E0870) reads it through player + 0x1008; 25..63 the 0x8F7 store; nullptr past 63.
    const uint8_t* LogEntry(const uint8_t* player, uint32_t slot)
    {
        const uint8_t* fields = *reinterpret_cast<const uint8_t* const*>(player + 0x1008);   // descriptor + 0x250
        if (!fields && slot < AscQuestLog64::kNativeSlots)
            return nullptr;
        return AscQuestLog64::SlotEntry(fields ? fields - 0x250 : nullptr, slot);
    }

    // 0x5E0870 CGPlayer::GetQuestLogEntry (thiscall, `self` = player + 0x1008; __fastcall with an unused edx has the
    // same stack contract). 0..24 the descriptor; 25..63 the 0x8F7 store, which only holds the active player's log
    // (the original's `slot > 25` let slot 25 read past the descriptor's log).
    const uint8_t* __fastcall GetQuestLogEntry(const uint8_t* const* self, void*, int32_t slot)
    {
        if (slot < 0)
            return nullptr;
        if (slot < static_cast<int32_t>(AscQuestLog64::kNativeSlots))
            return *self + 0x28 + slot * 0x14;
        const uint8_t* active = Player();
        if (!active || slot >= static_cast<int32_t>(kMaxSlots) ||
            *reinterpret_cast<const uint8_t* const*>(active + 0x1008) != *self)
            return nullptr;
        return LogEntry(active, static_cast<uint32_t>(slot));
    }
    uint32_t EntryQuest(const uint8_t* e) { return *reinterpret_cast<const uint32_t*>(e); }
    uint32_t EntryState(const uint8_t* e) { return *reinterpret_cast<const uint32_t*>(e + 4); }
    uint16_t EntryCount(const uint8_t* e, uint32_t i) { return *reinterpret_cast<const uint16_t*>(e + 8 + i * 2); }
    uint32_t EntryTime(const uint8_t* e) { return *reinterpret_cast<const uint32_t*>(e + 16); }

    // The copy back into m_quests / m_sortTypes, for the readers not yet replaced: Task 11 (0x5DF610, 0x5DF700,
    // 0x5DF780, 0x5DFC40, 0x5E64D0, 0x5E67C0, 0x5E6240, 0x5E4FB0) and Task 12 (0x5E2B60, 0x5E2EB0,
    // 0x5E3840, 0x5E6650), and the DLL's own readers of 0xC237B0 / 0xC23AD0 (Task 13): AscAttachFixes QuestRelevant,
    // AscSmallApis ForOpenLoggedQuest and AscQuestSpellHide HiddenSpell. Remove once they are.
    void Sync() { Mirror(s_list, NativeRows(), NativeCount(), NativeSortTypes(), NativeNumSortTypes()); }

    const Row* QuestRow(int32_t index)
    {
        return index >= 0 && static_cast<uint32_t>(index) < s_list.count && !s_list.rows[index].isHeader
                   ? &s_list.rows[index] : nullptr;
    }

    // ---- per-quest queries -------------------------------------------------------------------------------------

    // 0x5DEE30 CGQuestLog::IsQuestFailed
    int __cdecl IsQuestFailed(uint32_t index)
    {
        return index < s_list.count && !s_list.rows[index].isHeader && s_list.rows[index].expired;
    }

    // 0x5E0EA0 CGQuestLog::IsQuestCompleteEx. The item count passes 0, not 8: AscAttachLate's byte patch at
    // 0x5E1071 (push 8 -> push 0), kept here because this replaces the function it patches.
    int __cdecl IsQuestCompleteEx(uint32_t index, int8_t strict)
    {
        if (index >= s_list.count || s_list.rows[index].isHeader)
            return 0;
        uint8_t* player = Player();
        if (!player)
            return 0;
        const Row& row = s_list.rows[index];
        const uint8_t* entry = LogEntry(player, static_cast<uint32_t>(row.logSlot));
        const uint8_t* q = Quest(row.questId, 0x5DE930);
        if (!q || !entry || (EntryState(entry) & 2) || IsQuestFailed(index))
            return 0;

        const uint32_t special = Q<uint32_t>(q, kQFlags) & 6;
        if (!special && !Q<int32_t>(q, kQFriendly) && !Q<int32_t>(q, kQHostile) && !Q<int32_t>(q, kQMoney))
        {
            bool objectives = false;
            for (uint32_t i = 0; i < 4; ++i)
                objectives |= Q<int32_t>(q, kQObjective + i * 4) != 0;
            for (uint32_t i = 0; i < 6; ++i)
                objectives |= Q<int32_t>(q, kQItem + i * 4) != 0;
            if (strict && !objectives && !Q<int32_t>(q, kQPlayersKilled))
                return 0;
        }
        if (special && !(EntryState(entry) & 1))
            return 0;
        typedef int(__cdecl* Rep_t)(int32_t);   // GetRepListRepValue 0x5D05B0
        const Rep_t rep = reinterpret_cast<Rep_t>(0x5D05B0);
        if (Q<int32_t>(q, kQFriendly) && rep(Q<int32_t>(q, kQFriendly)) < Q<int32_t>(q, kQFriendlyAmount))
            return 0;
        if (Q<int32_t>(q, kQHostile) && rep(Q<int32_t>(q, kQHostile)) > Q<int32_t>(q, kQHostileAmount))
            return 0;
        const int32_t money = Q<int32_t>(q, kQMoney);
        if (money < 0 && reinterpret_cast<uint32_t(__thiscall*)(void*)>(0x51A2B0)(player) < static_cast<uint32_t>(-money))
            return 0;
        for (uint32_t i = 0; i < 4; ++i)
            if (Q<int32_t>(q, kQObjective + i * 4) && EntryCount(entry, i) < Q<uint32_t>(q, kQObjectiveCount + i * 4))
                return 0;
        typedef int(__thiscall* Count_t)(void*, int32_t, int);   // CGBag_C::GetItemTypeCount 0x754D00 on player+0x18F0
        for (uint32_t i = 0; i < 6; ++i)
        {
            const int32_t item = Q<int32_t>(q, kQItem + i * 4);
            if (item && reinterpret_cast<Count_t>(0x754D00)(player + 0x18F0, item, 0) < Q<int32_t>(q, kQItemCount + i * 4))
                return 0;
        }
        return static_cast<int32_t>(EntryCount(entry, 0)) >= Q<int32_t>(q, kQPlayersKilled);
    }

    // 0x5E10B0 CGQuestLog::HasNoObjectives. Returns int: the original sets all of eax (xor eax,eax / setnl al) and its
    // callers test eax.
    int __cdecl HasNoObjectives(uint32_t index)
    {
        if (index >= s_list.count || s_list.rows[index].isHeader)
            return 0;
        uint8_t* player = Player();
        if (!player)
            return 0;
        const Row& row = s_list.rows[index];
        const uint8_t* entry = LogEntry(player, static_cast<uint32_t>(row.logSlot));
        const uint8_t* q = Quest(row.questId, 0x5DE930);
        if (!q || !entry || (EntryState(entry) & 2) || IsQuestFailed(index))
            return 0;
        for (uint32_t i = 0; i < 4; ++i)
            if (Q<int32_t>(q, kQObjective + i * 4))
                return 0;
        for (uint32_t i = 0; i < 6; ++i)
        {
            const int32_t item = Q<int32_t>(q, kQItem + i * 4);
            if (item && (item != Q<int32_t>(q, kQStartItem) || Q<int32_t>(q, kQItemCount + i * 4) > 1))
                return 0;
        }
        return (!Q<int32_t>(q, kQPlayersKilled) && !(Q<uint32_t>(q, kQFlags) & 6) && !Q<int32_t>(q, kQFriendly) &&
               !Q<int32_t>(q, kQHostile) && Q<int32_t>(q, kQMoney) >= 0) ? 1 : 0;
    }

    // 0x5E57E0 CGQuestLog::SetQuestExpired
    int __cdecl SetQuestExpired(uint32_t index)
    {
        if (index >= s_list.count || IsQuestCompleteEx(index, 1))
            return 0;
        s_list.rows[index].expired = 1;
        Sync();
        if (Player())
            QueryQuestGiverStatus();
        return 1;
    }

    // 0x5E5840 CGQuestLog::SetQuestExpiredByID (matches header rows too, as the original does)
    int __cdecl SetQuestExpiredByID(int32_t questId)
    {
        uint32_t index = 0;
        while (index < s_list.count && s_list.rows[index].questId != questId)
            ++index;
        if (index >= s_list.count)
            return 0;
        return SetQuestExpired(index);
    }

    // 0x5DEE60 CGQuestLog::IsQuestExpiredByID (first row with the id, header or not)
    int __cdecl IsQuestExpiredByID(int32_t questId)
    {
        for (uint32_t i = 0; i < s_list.count; ++i)
            if (s_list.rows[i].questId == questId)
                return !s_list.rows[i].isHeader && s_list.rows[i].expired;
        return 0;
    }

    // 0x5DECC0 IsDailyQuest
    int __cdecl IsDailyQuest(int32_t index)
    {
        const Row* r = QuestRow(index);
        const uint8_t* q = r ? Quest(r->questId, 0x5DE930) : nullptr;
        return q ? Q<uint32_t>(q, kQFlags) & 0x1000 : 0;
    }

    // 0x5DED30 GetQuestSuggestedPlayers
    int __cdecl GetQuestSuggestedPlayers(int32_t index)
    {
        const Row* r = QuestRow(index);
        const uint8_t* q = r ? Quest(r->questId, 0x5DE930) : nullptr;
        return q ? Q<int32_t>(q, kQPlayers) : 0;
    }

    // 0x5E0000 GetQuestTitle: the quest's title, or the header's zone (AreaTable 0xAD314C +0x2C) / sort
    // (QuestSort 0xAD4448 +4) name.
    const char* __cdecl GetQuestTitle(int32_t index)
    {
        if (index < 0 || static_cast<uint32_t>(index) >= s_list.count)
            return nullptr;
        const Row& r = s_list.rows[index];
        if (!r.isHeader)
            return reinterpret_cast<const char*(__cdecl*)(int32_t)>(0x5DEC70)(r.questId);   // GetQuestTitleText
        if (r.questId > 0)
        {
            const uint8_t* area = DbRecord(0xAD314C, r.questId);
            return area ? Q<const char*>(area, 0x2C) : nullptr;
        }
        if (r.questId == 0)
            return "Missing header! (quest designers)";
        const uint8_t* sort = DbRecord(0xAD4448, -r.questId);
        return sort ? Q<const char*>(sort, 4) : nullptr;
    }

    // 0x5E0070 GetQuestTag: QuestInfo (0xAD4424) name of the quest's type.
    const char* __cdecl GetQuestTag(int32_t index)
    {
        const Row* r = QuestRow(index);
        const uint8_t* q = r ? Quest(r->questId, 0x5DE930) : nullptr;
        const uint8_t* info = q ? DbRecord(0xAD4424, Q<int32_t>(q, kQType)) : nullptr;
        return info ? Q<const char*>(info, 4) : nullptr;
    }

    // ---- index lookups -----------------------------------------------------------------------------------------

    // 0x5DE8C0 CGQuestLog::GetQuestLogSlot
    int __cdecl GetQuestLogSlot(int32_t index)
    {
        const Row* r = QuestRow(index);
        return r ? r->logSlot : -1;
    }

    // 0x5DEC40 CGQuestLog::GetQuestAt
    int __cdecl GetQuestAt(int32_t index)
    {
        const Row* r = QuestRow(index);
        return r ? r->questId : 0;
    }

    // 0x5DEEB0 CGQuestLog::GetQuestIndex
    int __cdecl GetQuestIndex(int32_t questId) { return static_cast<int>(QuestIndex(s_list, questId)); }

    // 0x5DEBD0 CGQuestLog::GetSelectedLogEntry
    int __cdecl GetSelectedLogEntry()
    {
        for (uint32_t i = 0; i < s_list.count; ++i)
            if (!s_list.rows[i].isHeader && s_list.rows[i].questId == Selected())
                return static_cast<int>(i);
        return -1;
    }

    // 0x5E00E0 CGQuestLog::IsSelectedQuestExpired
    int __cdecl IsSelectedQuestExpired()
    {
        const uint32_t i = static_cast<uint32_t>(GetSelectedLogEntry());
        return i < s_list.count && !s_list.rows[i].isHeader && s_list.rows[i].expired;
    }

    // 0x5DFFA0 CGQuestLog::SetSelectedQuest: clears the quest point, selects the row's quest and shows its point.
    int __cdecl SetSelectedQuest(int32_t index)
    {
        if (Selected())
            reinterpret_cast<void(__cdecl*)(float, float, int, const char*)>(0x7F4930)(
                0.0f, 0.0f, 0, reinterpret_cast<const char*>(0x9E14FF));
        if (index < 0 || static_cast<uint32_t>(index) >= s_list.count)
            Selected() = 0;
        else if (!s_list.rows[index].isHeader)
        {
            Selected() = s_list.rows[index].questId;
            reinterpret_cast<void(__cdecl*)()>(0x5DEB60)();   // AddQuestPointDisplay
        }
        return 0;
    }

    // 0x5E11D0 CGQuestLog::IsOnQuest
    int __cdecl IsOnQuest(int32_t questId)
    {
        const uint8_t* player = Player();
        if (!player || (*reinterpret_cast<const uint32_t*>(player + 0xBC) & 0x10000))
            return 0;
        for (uint32_t slot = 0; slot < kMaxSlots; ++slot)
            if (const uint8_t* e = LogEntry(player, slot))
                if (EntryQuest(e) && static_cast<int32_t>(EntryQuest(e)) == questId)
                    return 1;
        return 0;
    }

    // 0x5E0CC0 CGQuestLog::AbandonSelectedQuest: CMSG_QUESTLOG_REMOVE_QUEST (0x6D4F80) for the quest's slot.
    void __cdecl AbandonSelectedQuest()
    {
        if (!Abandon() || !Player())
            return;
        for (uint32_t i = 0; i < kRows; ++i)
            if (!s_list.rows[i].isHeader && s_list.rows[i].questId == Abandon())
            {
                reinterpret_cast<void(__stdcall*)(uint32_t)>(0x6D4F80)(static_cast<uint8_t>(s_list.rows[i].logSlot));
                return;
            }
    }

    // ---- list build, sort and collapse -------------------------------------------------------------------------

    // 0x5DEAE0 GetQuestSortId
    int __cdecl GetQuestSortId(uint32_t index)
    {
        if (index >= s_list.count)
            return -1;
        int32_t type = s_list.rows[index].questId;
        if (!s_list.rows[index].isHeader)
        {
            const uint8_t* q = Quest(type);
            if (!q)
                return -1;
            type = Q<int32_t>(q, kQSort);
        }
        const uint32_t i = FindSort(s_list, type, s_list.numSortTypes);
        return i < s_list.numSortTypes ? static_cast<int>(i) : -1;
    }

    SortKey Key(const Row& r, const uint8_t* player)
    {
        SortKey k = {r.isHeader != 0, true, 0, 0, nullptr};
        if (k.header)
        {
            k.sort = SortIndex(s_list, r.questId);
            return k;
        }
        const uint8_t* q = Quest(r.questId);
        k.known = q != nullptr;
        if (!q)
            return k;
        k.sort = SortIndex(s_list, Q<int32_t>(q, kQSort));
        k.level = Q<int32_t>(q, kQLevel);
        if (k.level == -1 && player)
            k.level = *reinterpret_cast<const int32_t*>(*reinterpret_cast<const uint8_t* const*>(player + 0xD0) + 0xC0);
        k.name = reinterpret_cast<const char*>(q + kQName);
        return k;
    }

    // 0x5E08F0 QSortQuestLogEntries (its only caller is FilterAndSortQuests, replaced below, so it is not hooked)
    int __cdecl CompareRows(const void* a, const void* b)
    {
        const uint8_t* player = Player();
        return Compare(Key(*static_cast<const Row*>(a), player), Key(*static_cast<const Row*>(b), player),
                       CollapseFilter(), &SortNames);
    }

    // 0x5E0B80 CGQuestLog::FilterAndSortQuests
    void __cdecl FilterAndSortQuests()
    {
        qsort(s_list.rows, s_list.count, sizeof(Row), &CompareRows);
        NumShown() = s_list.count;
        for (uint32_t i = 0; i < s_list.count; ++i)
            if (!s_list.rows[i].isHeader && Collapsed(static_cast<uint32_t>(GetQuestSortId(i)), CollapseFilter()))
                --NumShown();
        Sync();
    }

    // 0x5E0C00 CGQuestLog::CollapseHeader. A header past the 25th sort type has no collapse bit (spec non-goal).
    void __cdecl CollapseHeader(uint32_t index, int32_t collapse)
    {
        if (index < s_list.count && s_list.rows[index].isHeader)
        {
            const uint32_t i = FindSort(s_list, s_list.rows[index].questId, s_list.numSortTypes);
            if (i == s_list.numSortTypes || i >= kCollapseBits)
                return;
            const int32_t filter = CollapseFilter();
            const int32_t bit = static_cast<int32_t>(1u << i);
            SetCollapseFilter(collapse ? filter & ~bit : filter | bit);
        }
        else
            SetCollapseFilter((collapse != 0) - 1);
        FilterAndSortQuests();
        Signal(kQuestLogUpdate);
    }

    // 0x5E6940 CGQuestLog::Update: rebuilds the list from the 64 slots, sorts it, keeps each header's collapse
    // state, drops watches of quests no longer in the log, and asks for unknown POIs.
    void __cdecl Update(int32_t rebuild)
    {
        if (!rebuild)
        {
            Signal(kQuestLogUpdate);
            return;
        }
        if (CallbackCount())
            return;
        uint8_t* player = Player();
        if (!player)
            return;

        int32_t filter = CollapseFilter();
        int32_t oldSortTypes[kMaxSlots];
        const uint32_t numOld = s_list.numSortTypes;
        SaveSortTypes(s_list, filter, oldSortTypes);

        s_list = {};
        CallbackCount() = 0;
        uint32_t numUnknown = 0;
        int32_t unknown[kMaxSlots] = {};
        const int32_t timeOffset = TimeOffset();

        for (uint32_t slot = 0; slot < kMaxSlots; ++slot)
        {
            const uint8_t* entry = LogEntry(player, slot);
            const int32_t questId = entry ? static_cast<int32_t>(EntryQuest(entry)) : 0;
            if (questId <= 0)
                continue;
            const uint8_t* q = Quest(questId, 0x5DE8F0);   // QuestQueryCounterCallback reruns Update
            if (!q)
            {
                ++CallbackCount();
                continue;
            }
            if (!reinterpret_cast<uint8_t(__cdecl*)(int32_t)>(0x5E28B0)(questId))   // CGQuestLog::HasQuestPOI (al only)
            {
                ++numUnknown;
                unknown[slot] = questId;
            }
            const uint32_t row = Append(s_list, questId, slot, Q<int32_t>(q, kQSort));
            const int32_t time = static_cast<int32_t>(EntryTime(entry));
            if (time && !(EntryState(entry) & 2) &&
                timeOffset + time - static_cast<int32_t>(::time(nullptr)) - 1 < 0)
                SetQuestExpired(row);
        }

        qsort(s_list.sortTypes, s_list.numSortTypes, sizeof(int32_t),
              reinterpret_cast<int(__cdecl*)(const void*, const void*)>(0x5DFDC0));   // QSortQuestSortTypes
        filter = MergeFilter(s_list, filter, oldSortTypes, numOld);
        SetCollapseFilter(filter);
        FilterAndSortQuests();

        if (!TrackedLoaded() && s_list.count)
            reinterpret_cast<void(__cdecl*)()>(0x5E65B0)();   // CGQuestLog::LoadTrackedQuests

        bool found = true;
        for (uint32_t i = 0; i < NumWatches(); ++i)
        {
            if (!found)
                --i;
            found = false;
            for (uint32_t j = 0; j < s_list.count; ++j)
                if (!s_list.rows[j].isHeader && s_list.rows[j].questId == WatchQuest(i))
                {
                    found = true;
                    break;
                }
            if (!found)
                reinterpret_cast<void(__cdecl*)(int32_t)>(0x5DEA60)(WatchQuest(i));   // CGQuestLog::RemoveQuestWatch
        }

        Signal(kQuestLogUpdate);

        if (numUnknown)
        {
            AscScript::Packet packet(0x1E3);   // CMSG_QUEST_POI_QUERY
            packet.U32(numUnknown);
            for (uint32_t i = 0; i < kMaxSlots; ++i)
                if (unknown[i])
                    packet.U32(static_cast<uint32_t>(unknown[i]));
            packet.Send();
        }

        if (NextQuestUpdate() && static_cast<int32_t>(::time(nullptr)) >= NextQuestUpdate())
        {
            AscScript::Packet packet(0x1CE);   // CMSG_QUERY_TIME
            packet.Send();
            NextQuestUpdate() = 0;
        }
    }

    // 0x5E71A0 CGQuestLog::InitializeGame: the original, then the DLL list emptied with the client's.
    typedef int(__cdecl* InitializeGame_t)();
    InitializeGame_t g_initializeGame = nullptr;
    int __cdecl InitializeGame()
    {
        const int r = g_initializeGame();
        s_list = {};
        return r;
    }

    // ---- Lua API (QuestLog_Scripts.cpp) ------------------------------------------------------------------------

    using namespace AscLua;

    // 0x5DF010 Script_GetNumQuestLogEntries: shown rows, quests
    int __cdecl Script_GetNumQuestLogEntries(lua_State* L)
    {
        lua_pushnumber(L, NumShown());
        uint32_t quests = 0;
        for (uint32_t i = 0; i < s_list.count; ++i)
            quests += !s_list.rows[i].isHeader;
        lua_pushnumber(L, quests);
        return 2;
    }

    // 0x5E5CC0 Script_GetQuestLogTitle: title, level, tag, suggestedGroup, isHeader, isCollapsed, isComplete,
    // isDaily, questID [, displayQuestID]
    int __cdecl Script_GetQuestLogTitle(lua_State* L)
    {
        if (!lua_isnumber(L, 1))
            luaL_error(L, "Usage: GetQuestLogTitle(index)");
        const int32_t index = static_cast<int32_t>(lua_tonumber(L, 1)) - 1;
        lua_pushstring(L, GetQuestTitle(index));
        const Row* quest = QuestRow(index);
        lua_pushnumber(L, quest ? reinterpret_cast<int(__cdecl*)(int32_t)>(0x5E0D20)(quest->questId) : 0);   // GetQuestLevel
        lua_pushstring(L, GetQuestTag(index));
        lua_pushnumber(L, GetQuestSuggestedPlayers(index));
        if (index >= 0 && static_cast<uint32_t>(index) < s_list.count && s_list.rows[index].isHeader)
        {
            lua_pushnumber(L, 1.0);
            const uint32_t sort = static_cast<uint32_t>(GetQuestSortId(static_cast<uint32_t>(index)));
            if (sort >= kCollapseBits || ((1u << sort) & static_cast<uint32_t>(CollapseFilter())))
                lua_pushnil(L);
            else
                lua_pushnumber(L, 1.0);
        }
        else
        {
            lua_pushnil(L);
            lua_pushnil(L);
        }

        const uint8_t* player = Player();
        if (!player)
            lua_pushnil(L);
        else
        {
            const uint8_t* entry = quest && static_cast<uint32_t>(quest->logSlot) < kMaxSlots
                                       ? LogEntry(player, static_cast<uint32_t>(quest->logSlot)) : nullptr;
            if ((entry && (EntryState(entry) & 2)) || (quest && quest->expired))
                lua_pushnumber(L, -1.0);
            else if (IsQuestCompleteEx(static_cast<uint32_t>(index), 1))
                lua_pushnumber(L, 1.0);
            else
                lua_pushnil(L);
        }

        if (IsDailyQuest(index))
            lua_pushnumber(L, 1.0);
        else
            lua_pushnil(L);
        lua_pushnumber(L, quest ? quest->questId : 0);
        const uint8_t* cvar = reinterpret_cast<uint8_t*(__cdecl*)(const char*)>(0x767440)("displayQuestID");   // CVar::Lookup
        if (!cvar || !*reinterpret_cast<const int32_t*>(cvar + 0x30))
            return 9;
        lua_pushboolean(L, 1);
        return 10;
    }

    // The selected quest's log entry for the two Script_*s below (slot 0 when nothing is selected, as the original).
    const uint8_t* SelectedEntry(const uint8_t* player)
    {
        const int32_t index = GetSelectedLogEntry();
        const uint32_t slot = index < 0 ? 0 : static_cast<uint32_t>(s_list.rows[index].logSlot);
        return slot < kMaxSlots ? LogEntry(player, slot) : nullptr;
    }

    // 0x5E4320 Script_GetQuestLogTimeLeft
    int __cdecl Script_GetQuestLogTimeLeft(lua_State* L)
    {
        const uint8_t* player = Player();
        const uint8_t* entry = player ? SelectedEntry(player) : nullptr;
        const int32_t offset = TimeOffset();
        if (!entry || !EntryTime(entry) || (EntryState(entry) & 2) || !offset)
        {
            lua_pushnil(L);
            return 1;
        }
        int32_t left = offset + static_cast<int32_t>(EntryTime(entry)) - static_cast<int32_t>(::time(nullptr)) - 1;
        lua_pushnumber(L, left < 0 ? 0 : left);
        return 1;
    }

    // 0x5E43F0 Script_IsCurrentQuestFailed
    int __cdecl Script_IsCurrentQuestFailed(lua_State* L)
    {
        const uint8_t* player = Player();
        const uint8_t* entry = player ? SelectedEntry(player) : nullptr;
        if (entry && ((EntryState(entry) & 2) || IsSelectedQuestExpired()))
            lua_pushnumber(L, 1.0);
        else
            lua_pushnil(L);
        return 1;
    }

    // 0x5E4070 Script_IsUnitOnQuest(index, unit): a party member's log has 25 slots (the server sends no others);
    // the player's own has 64.
    int __cdecl Script_IsUnitOnQuest(lua_State* L)
    {
        if (!lua_isnumber(L, 1) || !lua_isstring(L, 2))
            luaL_error(L, "Usage: IsUnitOnQuest(index, \"unit\")");
        const Row* quest = QuestRow(static_cast<int32_t>(lua_tonumber(L, 1)) - 1);
        if (quest && quest->questId)
        {
            uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x60C1C0)(lua_tolstring(L, 2, nullptr));
            if (reinterpret_cast<int(__cdecl*)(const uint64_t*)>(0x52C680)(&guid))   // CGPartyInfo::IsMember
            {
                uint8_t* member = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t, const char*, int)>(0x4D4DB0)(
                    static_cast<uint32_t>(guid), static_cast<uint32_t>(guid >> 32), kTypePlayer, kFile, 0);
                if (member && !(*reinterpret_cast<const uint32_t*>(member + 0xBC) & 0x10000))
                {
                    const bool self = member == Player();
                    const uint32_t slots = self ? kMaxSlots : AscQuestLog64::kNativeSlots;
                    for (uint32_t slot = 0; slot < slots; ++slot)
                    {
                        const uint8_t* e = self ? LogEntry(member, slot)
                                                : reinterpret_cast<const uint8_t*(__thiscall*)(void*, uint32_t)>(0x5E0870)(
                                                      member + 0x1008, slot);   // CGPlayer::GetQuestLogEntry
                        if (e && static_cast<int32_t>(EntryQuest(e)) == quest->questId)
                        {
                            lua_pushnumber(L, 1.0);
                            return 1;
                        }
                    }
                }
            }
        }
        lua_pushnil(L);
        return 1;
    }

    // ---- operand patches: client functions that only index the list ------------------------------------------
    //
    // Instead of a port, every operand that addresses the old list is pointed at the DLL list: m_quests row
    // fields (0xC237B0 + 0/4/8/0xC) -> s_list.rows + the same offset, the count 0xC23AD0 -> s_list.count, and each
    // `cmp eax, 32h` (50 rows) bounding a row walk -> `cmp al, 80h` (128 rows; 128 does not fit a signed imm8, and
    // every such site counts eax up from 0 by 1 and branches with jb, so the 8-bit unsigned compare is exact). The
    // selection globals (0xC23AD8, 0xC23ADC) are not relocated. Each function's patches are applied only when every
    // original operand matches; otherwise the function is left alone (it then reads the mirror) and the log says so.
    // Table generated from IDA (2026-11-25); see the task 10 report.
    enum OperandKind : uint8_t { kRows, kCount, kBound };
    struct OperandPatch
    {
        uint32_t function;
        uint32_t at;         // the operand's 4 bytes (kRows, kCount); the instruction (kBound)
        uint32_t original;   // the operand's value (kBound: unused)
        OperandKind kind;
    };
    const OperandPatch kOperandPatches[] = {
        {0x5E1460, 0x5E150E, 0xC237B4, kRows},    // GetQuestLeaderBoardByID: mov edi, m_quests[].logSlot
        {0x5E1460, 0x5E1A3B, 0xC237B0, kRows},    //   mov ecx, offset m_quests
        {0x5E1460, 0x5E1A54, 0, kBound},          //   cmp eax, 32h
        {0x5E1460, 0x5E1F37, 0xC237B0, kRows},    //   mov ecx, offset m_quests
        {0x5E1460, 0x5E1F54, 0, kBound},          //   cmp eax, 32h
        {0x5E20A0, 0x5E2147, 0xC237B4, kRows},    // GetQuestLogItemDropByID: mov esi, m_quests[].logSlot
        {0x5E2370, 0x5E2401, 0xC23AD0, kCount},   // GetQuestLeaderBoardForObject
        {0x5E2370, 0x5E240C, 0xC237B8, kRows},
        {0x5E2370, 0x5E2415, 0xC237B4, kRows},
        {0x5E2630, 0x5E26C1, 0xC23AD0, kCount},   // GetQuestLeaderBoardForItem
        {0x5E2630, 0x5E26CC, 0xC237B8, kRows},
        {0x5E2630, 0x5E26D5, 0xC237B4, kRows},
        {0x5E1300, 0x5E1423, 0xC237B0, kRows},    // GetNumQuestLeaderBoards: mov ecx, offset m_quests
        {0x5E1300, 0x5E1439, 0, kBound},          //   cmp eax, 32h
        {0x5E0DA0, 0x5E0DAB, 0xC23AD0, kCount},   // GetQuestSpecialItem
        {0x5E0DA0, 0x5E0DBD, 0xC237B8, kRows},
        {0x5E0DA0, 0x5E0DC9, 0xC237B0, kRows},
        {0x5E1250, 0x5E125E, 0xC23AD0, kCount},   // UpdateQuestItem
        {0x5E1250, 0x5E126A, 0xC237B0, kRows},
        {0x5E1250, 0x5E12AF, 0xC23AD0, kCount},
        {0x5DF460, 0x5DF4A6, 0xC23AD0, kCount},   // Script_GetQuestLogRequiredMoney
        {0x5DF460, 0x5DF4B1, 0xC237B8, kRows},
        {0x5DF460, 0x5DF4BA, 0xC237B0, kRows},
        {0x5DF520, 0x5DF55B, 0xC23AD0, kCount},   // Script_GetQuestLogPushable
        {0x5DF520, 0x5DF566, 0xC237B8, kRows},
        {0x5DF520, 0x5DF56F, 0xC237B0, kRows},
        {0x5E06D0, 0x5E0719, 0xC23AD0, kCount},   // Script_GetQuestLogCompletionText
        {0x5E06D0, 0x5E0724, 0xC237B8, kRows},
        {0x5E06D0, 0x5E072D, 0xC237B0, kRows},
        {0x5E3D60, 0x5E3EF4, 0xC23AD0, kCount},   // Script_GetAbandonQuestItems
        {0x5E3D60, 0x5E3F09, 0xC237B0, kRows},
        {0x5E3D60, 0x5E3FAD, 0xC23AD0, kCount},
        {0x5E41A0, 0x5E420C, 0xC23AD0, kCount},   // Script_GetNumQuestLeaderBoards
        {0x5E41A0, 0x5E4217, 0xC237B8, kRows},
        {0x5E41A0, 0x5E4220, 0xC237B0, kRows},
        {0x5E4260, 0x5E42CC, 0xC23AD0, kCount},   // Script_GetNumQuestItemDrops
        {0x5E4260, 0x5E42D7, 0xC237B8, kRows},
        {0x5E4260, 0x5E42E0, 0xC237B0, kRows},
        {0x5E4ED0, 0x5E4F0C, 0xC23AD0, kCount},   // Script_QuestLogPushQuest
        {0x5E4ED0, 0x5E4F17, 0xC237B8, kRows},
        {0x5E4ED0, 0x5E4F20, 0xC237B0, kRows},
        {0x5E51D0, 0x5E521B, 0xC23AD0, kCount},   // Script_GetQuestLink
        {0x5E51D0, 0x5E522C, 0xC237B8, kRows},
        {0x5E51D0, 0x5E5234, 0xC237B0, kRows},
        {0x5E5F60, 0x5E602E, 0xC23AD0, kCount},   // Script_GetQuestLogLeaderBoard
        {0x5E5F60, 0x5E6039, 0xC237B8, kRows},
        {0x5E5F60, 0x5E6041, 0xC237B0, kRows},
        {0x5E60D0, 0x5E619E, 0xC23AD0, kCount},   // Script_GetQuestLogItemDrop
        {0x5E60D0, 0x5E61A9, 0xC237B8, kRows},
        {0x5E60D0, 0x5E61B1, 0xC237B0, kRows},
        {0x621070, 0x621EF9, 0xC23AD0, kCount},   // CGTooltip::SetUnit (then GetQuestAt, ported)
        {0x621070, 0x6221FC, 0xC23AD0, kCount},
        {0x626720, 0x626AD2, 0xC23AD0, kCount},   // CGTooltip::SetObject (then GetQuestAt, ported)
        {0x626720, 0x626D43, 0xC23AD0, kCount},
        {0x5E2950, 0x5E2963, 0xC237B0, kRows},    // ComputeBlobQuestObjectives: mov ecx, offset m_quests
        {0x5E2950, 0x5E2976, 0, kBound},          //   cmp eax, 32h
        {0x5E2950, 0x5E2982, 0xC23AD0, kCount},
        {0x5E2950, 0x5E2992, 0xC237B8, kRows},
        {0x5E2950, 0x5E29CD, 0xC237B4, kRows},    //   then GetQuestLogEntry (0x5E0870, replaced)
        {0x5E2950, 0x5E29E5, 0xC237B0, kRows},
    };
    const uint8_t kBoundOriginal[3] = {0x83, 0xF8, 0x32};   // cmp eax, 32h
    const uint8_t kBoundPatched[3] = {0x80, 0xF8, 0x80};    // cmp al, 80h

    // The quest log entry the four leaderboard functions below read, for any slot (they keep their slot < 0 test).
    const uint8_t* __cdecl SiteSlotEntry(const uint8_t* player, uint32_t slot)
    {
        return player && slot < kMaxSlots ? LogEntry(player, slot) : nullptr;
    }

    // ---- slot sites: the descriptor read inside the leaderboard functions ---------------------------------------
    //
    // GetQuestLeaderBoardByID, GetQuestLogItemDropByID, GetQuestLeaderBoardForObject and GetQuestLeaderBoardForItem
    // each compute their log entry in one block:
    //     cmp slot, 19h / ja null / mov r, player / mov r, [r+1008h] / lea / lea dest, [...] / jmp done / null: xor dest
    // That block (from the cmp up to the null label, 23 bytes, no jump into it) becomes
    //     push slot / push [ebp+player] / call SiteSlotEntry / add esp, 8 / mov dest, eax / jmp done / nops
    // which clobbers eax, ecx and edx as the original block does. The rest of each function (3 KB for
    // GetQuestLeaderBoardByID, with AscAttachLate's byte patches) runs unchanged.
    struct SlotSite
    {
        uint32_t function, start, nullLabel, done;
        X86Reg slot, dest;
        int8_t player;          // [ebp + player]
        uint8_t original[23];
    };
    const SlotSite kSlotSites[] = {
        {0x5E1460, 0x5E1553, 0x5E156A, 0x5E156C, kEdi, kEdi, -4,
         {0x83, 0xFF, 0x19, 0x77, 0x12, 0x8B, 0x55, 0xFC, 0x8B, 0x82, 0x08, 0x10, 0x00, 0x00, 0x8D, 0x4C, 0xBF, 0x0A,
          0x8D, 0x3C, 0x88, 0xEB, 0x02}},
        {0x5E20A0, 0x5E218C, 0x5E21A3, 0x5E21A5, kEsi, kEax, 0x0C,
         {0x83, 0xFE, 0x19, 0x77, 0x12, 0x8B, 0x4D, 0x0C, 0x8B, 0x91, 0x08, 0x10, 0x00, 0x00, 0x8D, 0x44, 0xB6, 0x0A,
          0x8D, 0x04, 0x82, 0xEB, 0x02}},
        {0x5E2370, 0x5E241D, 0x5E2434, 0x5E2436, kEax, kEcx, 0x10,
         {0x83, 0xF8, 0x19, 0x77, 0x12, 0x8B, 0x4D, 0x10, 0x8B, 0x91, 0x08, 0x10, 0x00, 0x00, 0x8D, 0x44, 0x80, 0x0A,
          0x8D, 0x0C, 0x82, 0xEB, 0x02}},
        {0x5E2630, 0x5E26DD, 0x5E26F4, 0x5E26F6, kEax, kEax, 0x10,
         {0x83, 0xF8, 0x19, 0x77, 0x12, 0x8B, 0x4D, 0x10, 0x8B, 0x91, 0x08, 0x10, 0x00, 0x00, 0x8D, 0x44, 0x80, 0x0A,
          0x8D, 0x04, 0x82, 0xEB, 0x02}},
    };

    void WriteCode(uint32_t at, const void* bytes, uint32_t size)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(at), bytes, size);
        VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    }

    bool OperandMatches(const OperandPatch& p)
    {
        if (p.kind == kBound)
            return memcmp(reinterpret_cast<const void*>(p.at), kBoundOriginal, 3) == 0;
        return memcmp(reinterpret_cast<const void*>(p.at), &p.original, 4) == 0;
    }

    void ApplyOperand(const OperandPatch& p)
    {
        if (p.kind == kBound)
            return WriteCode(p.at, kBoundPatched, 3);
        const uint32_t value = Relocated(p.original, reinterpret_cast<uint32_t>(s_list.rows),
                                         reinterpret_cast<uint32_t>(&s_list.count));
        WriteCode(p.at, &value, 4);
    }

    void ApplySlotSite(const SlotSite& s)
    {
        uint8_t code[sizeof(s.original)];
        SlotSiteCode(code, sizeof(code), s.start, s.done, s.slot, s.dest, s.player,
                     reinterpret_cast<uint32_t>(&SiteSlotEntry));
        WriteCode(s.start, code, sizeof(code));
    }

    // Applies one function's operand patches and slot site, all or nothing.
    void PatchFunction(uint32_t function)
    {
        bool ok = true;
        for (const OperandPatch& p : kOperandPatches)
            if (p.function == function && !OperandMatches(p))
            {
                AscLog::Printf("AscQuestLog64List: 0x%06X operand at 0x%06X does not match; function left unpatched",
                               function, p.at);
                ok = false;
            }
        for (const SlotSite& s : kSlotSites)
            if (s.function == function && (memcmp(reinterpret_cast<const void*>(s.start), s.original, sizeof(s.original)) != 0 ||
                                           s.start + sizeof(s.original) != s.nullLabel))
            {
                AscLog::Printf("AscQuestLog64List: 0x%06X slot site at 0x%06X does not match; function left unpatched",
                               function, s.start);
                ok = false;
            }
        if (!ok)
            return;
        for (const OperandPatch& p : kOperandPatches)
            if (p.function == function)
                ApplyOperand(p);
        for (const SlotSite& s : kSlotSites)
            if (s.function == function)
                ApplySlotSite(s);
    }

    void PatchOperands()
    {
        uint32_t done[64] = {};
        uint32_t count = 0;
        for (const OperandPatch& p : kOperandPatches)
        {
            bool seen = false;
            for (uint32_t i = 0; i < count; ++i)
                seen |= done[i] == p.function;
            if (seen || count == 64)
                continue;
            done[count++] = p.function;
            PatchFunction(p.function);
        }
    }

    // ---- MAX_QUESTLOG_QUESTS / MAX_WATCHABLE_QUESTS ------------------------------------------------------------

    // FrameXML sets both to 25 when the interface loads; they follow the server's log size (25 without a packet).
    void SetInterfaceLimit(lua_State* L)
    {
        if (!L)
            return;
        for (const char* name : {"MAX_QUESTLOG_QUESTS", "MAX_WATCHABLE_QUESTS"})
        {
            lua_pushnumber(L, AscQuestLog64::LogSize());
            lua_setfield(L, LUA_GLOBALSINDEX, name);
        }
    }
    void ApplyInterfaceLimit() { SetInterfaceLimit(GetState()); }

    constexpr int kPlayerEnteringWorld = 0xFF;   // the client's event table index (0xC24EB0 + 0xFF * 4 = 0xC252AC)

    struct Replacement
    {
        uint32_t address;
        void* fn;
    };

    void Init()
    {
        const Replacement replacements[] = {
            {0x5E6940, reinterpret_cast<void*>(&Update)},
            {0x5E0B80, reinterpret_cast<void*>(&FilterAndSortQuests)},
            {0x5E0C00, reinterpret_cast<void*>(&CollapseHeader)},
            {0x5DEAE0, reinterpret_cast<void*>(&GetQuestSortId)},
            {0x5E57E0, reinterpret_cast<void*>(&SetQuestExpired)},
            {0x5E5840, reinterpret_cast<void*>(&SetQuestExpiredByID)},
            {0x5DEE30, reinterpret_cast<void*>(&IsQuestFailed)},
            {0x5DEE60, reinterpret_cast<void*>(&IsQuestExpiredByID)},
            {0x5E0EA0, reinterpret_cast<void*>(&IsQuestCompleteEx)},
            {0x5E10B0, reinterpret_cast<void*>(&HasNoObjectives)},
            {0x5DECC0, reinterpret_cast<void*>(&IsDailyQuest)},
            {0x5DED30, reinterpret_cast<void*>(&GetQuestSuggestedPlayers)},
            {0x5E0000, reinterpret_cast<void*>(&GetQuestTitle)},
            {0x5E0070, reinterpret_cast<void*>(&GetQuestTag)},
            {0x5DE8C0, reinterpret_cast<void*>(&GetQuestLogSlot)},
            {0x5DEC40, reinterpret_cast<void*>(&GetQuestAt)},
            {0x5DEEB0, reinterpret_cast<void*>(&GetQuestIndex)},
            {0x5DEBD0, reinterpret_cast<void*>(&GetSelectedLogEntry)},
            {0x5E00E0, reinterpret_cast<void*>(&IsSelectedQuestExpired)},
            {0x5DFFA0, reinterpret_cast<void*>(&SetSelectedQuest)},
            {0x5E11D0, reinterpret_cast<void*>(&IsOnQuest)},
            {0x5E0CC0, reinterpret_cast<void*>(&AbandonSelectedQuest)},
            {0x5DF010, reinterpret_cast<void*>(&Script_GetNumQuestLogEntries)},
            {0x5E5CC0, reinterpret_cast<void*>(&Script_GetQuestLogTitle)},
            {0x5E4320, reinterpret_cast<void*>(&Script_GetQuestLogTimeLeft)},
            {0x5E43F0, reinterpret_cast<void*>(&Script_IsCurrentQuestFailed)},
            {0x5E4070, reinterpret_cast<void*>(&Script_IsUnitOnQuest)},
            {0x5E0870, reinterpret_cast<void*>(&GetQuestLogEntry)},   // also serves 0x5E58C0, 0x5E2950
        };
        for (const Replacement& r : replacements)
            AscRuntime::ReplaceFunction(r.address, r.fn);
        // push ebp / mov ebp,esp / push ecx / push ebx: five bytes.
        g_initializeGame = reinterpret_cast<InitializeGame_t>(
            AscRuntime::Detour(0x5E71A0, 5, reinterpret_cast<void*>(&InitializeGame)));
        PatchOperands();

        AscRuntime::OnStockEvent(kPlayerEnteringWorld, &SetInterfaceLimit);   // world entry and every /reload
        AscQuestLog64::OnChanged(&ApplyInterfaceLimit);
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
#endif
