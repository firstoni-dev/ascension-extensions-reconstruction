# Quest log of 64 quests: xref audit and in-game checklist

`ASCENSION_QUEST_LOG_64` (on by default) gives `Ascension.exe` a quest log of 64 quests: the server sends slots 25..63
in SMSG `0x8F7`, and the DLL moves every client structure sized for 25 quests to its own 64-slot copy. That structure
is the quest list `m_quests`, the sort types, the watches and the POIs. The DLL changes the client code in three ways
(`src/Ascension/AscQuestLog64List.cpp`):

- **Replaced functions.** `AscRuntime::ReplaceFunction` swaps out the whole function, and the original never runs.
- **Operand patches.** `kOperandPatches` points an array address at the DLL copy, or turns a bound of 25 or 50 into 64
  or 128.
- **Site rewrites.** `kSlotSites` and the `Packet_Group_17` site replace a 25-slot descriptor read with a call that
  covers 64 slots.

This page records the audit that checks that no client reader of the old arrays or of the descriptor quest slots was
missed. It also holds the in-game checklist for the user.

## Audit result

Run on 2026-11-25 against `D:\WoW\Coa-Dev\Ascension.exe` (MD5 `d7c1f84725331dda38fa2e134cfad61a`, IDA 9.5 database)
and the source at `029487e`. The run exits with status 0: no reference is UNCOVERED, and no table entry fails to match
the binary.

Classes:

- **detoured:** the function is replaced whole.
- **operand-patched:** each of its references is an operand patch or lies inside a rewritten site.
- **harmless:** each remaining reference is explained. Either a rule in the script's `HARMLESS` table gives the reason,
  or the call graph shows that only replaced functions can reach the function.
- **UNCOVERED:** anything else.

<!-- generated: tools/ida/quest_log_64_xrefs.py output below -->
Covered set read from AscQuestLog64.cpp, AscQuestLog64List.cpp: 177 operand patches, 5 slot sites, 1 failed-quest site, 33 replaced functions, 1 detours in AscQuestLog64*, 234 detours elsewhere.

Ranges: m_sortTypes[25] 0xC23618..0xC2367B; m_questWatches[25] 0xC23680..0xC237AB; m_quests[50] / count / numSortTypes 0xC237B0..0xC23AD7; m_questPOIs[25] 0xC23B00..0xC23FAF; descriptor quest slots desc+0x278..+0x46B.

Result: UNCOVERED 0, detoured 32, operand-patched 47, harmless 3, problems 0.

| Function | Name | References | # | Class | Reason |
|---|---|---|---|---|---|
| 0x5DE8C0 | GetQuestLogSlot | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> GetQuestLogSlot |
| 0x5DEA60 | RemoveQuestWatch | m_questWatches[25], m_sortTypes[25] | 4 | operand-patched | 4 site(s) patched |
| 0x5DEAE0 | GetQuestSortId | m_quests[50] / count / numSortTypes, m_sortTypes[25] | 5 | detoured | ReplaceFunction -> GetQuestSortId |
| 0x5DEBD0 | GetSelectedLogEntry | m_quests[50] / count / numSortTypes | 2 | detoured | ReplaceFunction -> GetSelectedLogEntry |
| 0x5DEC40 | GetQuestAt | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> GetQuestAt |
| 0x5DECC0 | IsDailyQuest | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> IsDailyQuest |
| 0x5DED30 | GetQuestSuggestedPlayers | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> GetQuestSuggestedPlayers |
| 0x5DEE30 | IsQuestFailed | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> IsQuestFailed |
| 0x5DEE60 | IsQuestExpiredByID | m_quests[50] / count / numSortTypes | 4 | detoured | ReplaceFunction -> IsQuestExpiredByID |
| 0x5DEEB0 | GetQuestIndex | m_quests[50] / count / numSortTypes | 1 | detoured | ReplaceFunction -> GetQuestIndex |
| 0x5DEEE0 | GetQuestPOIIcon | m_questPOIs[25] | 7 | operand-patched | 7 site(s) patched |
| 0x5DEF60 | ShiftQuestWatches | m_questWatches[25] | 4 | operand-patched | 4 site(s) patched |
| 0x5DF010 | Script_GetNumQuestLogEntries | m_quests[50] / count / numSortTypes | 2 | detoured | ReplaceFunction -> Script_GetNumQuestLogEntries |
| 0x5DF460 | Script_GetQuestLogRequiredMoney | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5DF520 | Script_GetQuestLogPushable | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5DF610 | Script_IsQuestWatched | m_questWatches[25], m_quests[50] / count / numSortTypes | 4 | operand-patched | 4 site(s) patched |
| 0x5DF700 | Script_RemoveQuestWatch | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5DF780 | Script_GetQuestIndexForWatch | m_questWatches[25], m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5DFA80 | Script_QuestPOIGetQuestIDByIndex | m_questPOIs[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5DFC40 | Script_GetQuestWatchIndex | m_questWatches[25], m_quests[50] / count / numSortTypes | 4 | operand-patched | 4 site(s) patched |
| 0x5DFEF0 | UpdateWatchTimers | m_questWatches[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5DFFA0 | SetSelectedQuest | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> SetSelectedQuest |
| 0x5E0000 | GetQuestTitle | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> GetQuestTitle |
| 0x5E0070 | GetQuestTag | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> GetQuestTag |
| 0x5E00E0 | IsSelectedQuestExpired | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> IsSelectedQuestExpired |
| 0x5E0240 | SaveTrackedQuests | m_questWatches[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5E06D0 | Script_GetQuestLogCompletionText | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E08F0 | QSortQuestLogEntries | m_sortTypes[25] | 4 | harmless | only reachable from replaced functions (callers: 0x5E0B80) |
| 0x5E0B80 | FilterAndSortQuests | m_quests[50] / count / numSortTypes | 5 | detoured | ReplaceFunction -> FilterAndSortQuests |
| 0x5E0C00 | CollapseHeader | m_quests[50] / count / numSortTypes, m_sortTypes[25] | 5 | detoured | ReplaceFunction -> CollapseHeader |
| 0x5E0CC0 | AbandonSelectedQuest | m_quests[50] / count / numSortTypes | 2 | detoured | ReplaceFunction -> AbandonSelectedQuest |
| 0x5E0DA0 | GetQuestSpecialItem | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E0EA0 | IsQuestCompleteEx | descriptor slots, m_quests[50] / count / numSortTypes | 5 | detoured | ReplaceFunction -> IsQuestCompleteEx |
| 0x5E10B0 | HasNoObjectives | descriptor slots, m_quests[50] / count / numSortTypes | 5 | detoured | ReplaceFunction -> HasNoObjectives |
| 0x5E11D0 | IsOnQuest | descriptor slots | 1 | detoured | ReplaceFunction -> IsOnQuest |
| 0x5E1250 | UpdateQuestItem | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E1300 | GetNumQuestLeaderBoards | m_quests[50] / count / numSortTypes | 1 | operand-patched | 1 site(s) patched |
| 0x5E1460 | GetQuestLeaderBoardByID | descriptor slots, m_quests[50] / count / numSortTypes | 4 | operand-patched | 4 site(s) patched |
| 0x5E20A0 | GetQuestLogItemDropByID | descriptor slots, m_quests[50] / count / numSortTypes | 2 | operand-patched | 2 site(s) patched |
| 0x5E2370 | GetQuestLeaderBoardForObject | descriptor slots, m_quests[50] / count / numSortTypes | 4 | operand-patched | 4 site(s) patched |
| 0x5E2630 | GetQuestLeaderBoardForItem | descriptor slots, m_quests[50] / count / numSortTypes | 4 | operand-patched | 4 site(s) patched |
| 0x5E28B0 | HasQuestPOI | m_questPOIs[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5E2900 | GetQuestPOIIndex | m_questPOIs[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5E2950 | ComputeBlobQuestObjectives | m_quests[50] / count / numSortTypes | 5 | operand-patched | 5 site(s) patched |
| 0x5E2B60 | GetQuestWorldMapAreaID | m_questPOIs[25], m_quests[50] / count / numSortTypes | 13 | operand-patched | 13 site(s) patched |
| 0x5E2EB0 | UpdatePOIIcons | m_questPOIs[25], m_quests[50] / count / numSortTypes | 30 | detoured | ReplaceFunction -> UpdatePOIIcons |
| 0x5E3840 | GetPOIQuestIDByVisibleIndex | m_questPOIs[25], m_questWatches[25], m_quests[50] / count / numSortTypes | 17 | detoured | ReplaceFunction -> GetPOIQuestIDByVisibleIndex |
| 0x5E3AC0 | GetQuestPOIInfo | m_questPOIs[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5E3B90 | GetQuestPOIDistance | m_questPOIs[25] | 8 | operand-patched | 8 site(s) patched |
| 0x5E3D60 | Script_GetAbandonQuestItems | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E4070 | Script_IsUnitOnQuest | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> Script_IsUnitOnQuest |
| 0x5E41A0 | Script_GetNumQuestLeaderBoards | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E4260 | Script_GetNumQuestItemDrops | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E4320 | Script_GetQuestLogTimeLeft | descriptor slots, m_quests[50] / count / numSortTypes | 2 | detoured | ReplaceFunction -> Script_GetQuestLogTimeLeft |
| 0x5E43F0 | Script_IsCurrentQuestFailed | descriptor slots, m_quests[50] / count / numSortTypes | 2 | detoured | ReplaceFunction -> Script_IsCurrentQuestFailed |
| 0x5E4ED0 | Script_QuestLogPushQuest | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E4FB0 | Script_GetQuestIndexForTimer | descriptor slots, m_quests[50] / count / numSortTypes | 4 | detoured | ReplaceFunction -> Script_GetQuestIndexForTimer |
| 0x5E51D0 | Script_GetQuestLink | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E57E0 | SetQuestExpired | m_quests[50] / count / numSortTypes | 2 | detoured | ReplaceFunction -> SetQuestExpired |
| 0x5E5840 | SetQuestExpiredByID | m_quests[50] / count / numSortTypes | 3 | detoured | ReplaceFunction -> SetQuestExpiredByID |
| 0x5E5A50 | UpdateAllQuestPOI | m_questPOIs[25] | 9 | operand-patched | 9 site(s) patched |
| 0x5E5C10 | SortQuestWatches | m_questWatches[25] | 2 | operand-patched | 2 site(s) patched |
| 0x5E5CC0 | Script_GetQuestLogTitle | descriptor slots, m_quests[50] / count / numSortTypes | 14 | detoured | ReplaceFunction -> Script_GetQuestLogTitle |
| 0x5E5F60 | Script_GetQuestLogLeaderBoard | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E60D0 | Script_GetQuestLogItemDrop | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E6240 | Script_GetQuestTimers | descriptor slots, m_quests[50] / count / numSortTypes | 5 | detoured | ReplaceFunction -> Script_GetQuestTimers |
| 0x5E64D0 | AddQuestWatch | m_questWatches[25], m_quests[50] / count / numSortTypes | 8 | operand-patched | 8 site(s) patched |
| 0x5E6650 | Script_GetQuestPOILeaderBoard | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E67C0 | Script_AddQuestWatch | m_quests[50] / count / numSortTypes | 3 | operand-patched | 3 site(s) patched |
| 0x5E6940 | Update | descriptor slots, m_questWatches[25], m_quests[50] / count / numSortTypes, m_sortTypes[25] | 30 | detoured | ReplaceFunction -> Update |
| 0x5E6F30 | ShutdownGame | m_questPOIs[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5E7020 | FindOrFreePOISlot | m_questPOIs[25] | 3 | operand-patched | 3 site(s) patched |
| 0x5E70D0 | UpdateQuestPOIFromUserServer | m_questPOIs[25] | 1 | operand-patched | 1 site(s) patched |
| 0x5E71A0 | InitializeGame | m_questPOIs[25], m_questWatches[25], m_quests[50] / count / numSortTypes | 12 | operand-patched | 9 site(s) patched; 3 harmless: memset of the old m_quests, its count clear and the memset of the old m_questWatches; the DLL's detour runs this original first and then empties the DLL list and s_watches (its POI loop is operand-patched) |
| 0x5E7370 | HandlePOIQueryResponse | m_questPOIs[25] | 2 | operand-patched | 2 site(s) patched |
| 0x621070 | CGTooltip::SetUnit | m_quests[50] / count / numSortTypes | 2 | operand-patched | 2 site(s) patched |
| 0x626720 | CGTooltip::SetObject | m_quests[50] / count / numSortTypes | 2 | operand-patched | 2 site(s) patched |
| 0x6D7DE0 | CGPlayer_C::CheckForQuestItemLoot | descriptor slots | 1 | operand-patched | 1 site(s) patched |
| 0x6D8030 | Packet_Group_17 (SMSG_QUESTUPDATE_*) | descriptor slots | 1 | operand-patched | 1 site(s) patched |
| 0x6DF370 | OnUpdateQuest | descriptor slots | 1 | harmless | descriptor-change handler: the slot comes from the changed descriptor field, so it is always 0..24 (verified: (offset - 28h) / 14h, `cmp edx, 19h`); slots 25..63 change through SMSG 0x8F7 and AscQuestLog64 Refresh / SlotUpdated |
| 0x70F580 | CanHighlight | descriptor slots | 1 | detoured | ReplaceFunction -> CanHighlight |
| nofunc 0x9CB285 | static constructor of m_questPOIs | m_questPOIs[25] | 1 | harmless | CRT initialiser of the old array, run before the DLL loads; that array is unused once the POI group is relocated, and s_pois starts zeroed as this constructor would leave it |

### Sites of the harmless, UNCOVERED and descriptor-slot rows (detoured rows omitted)

- 0x5E08F0 QSortQuestLogEntries: 0x5E0981 m_sortTypes[25] 0xC23618; 0x5E09AD m_sortTypes[25] 0xC23618; 0x5E09D5 m_sortTypes[25] 0xC23618; 0x5E0A07 m_sortTypes[25] 0xC23618
- 0x5E1460 GetQuestLeaderBoardByID: 0x5E150C m_quests[50] / count / numSortTypes 0xC237B4; 0x5E1565 lea desc+278h indexed; 0x5E1A3A m_quests[50] / count / numSortTypes 0xC237B0; 0x5E1F36 m_quests[50] / count / numSortTypes 0xC237B0
- 0x5E20A0 GetQuestLogItemDropByID: 0x5E2145 m_quests[50] / count / numSortTypes 0xC237B4; 0x5E219E lea desc+278h indexed
- 0x5E2370 GetQuestLeaderBoardForObject: 0x5E23FF m_quests[50] / count / numSortTypes 0xC23AD0; 0x5E240A m_quests[50] / count / numSortTypes 0xC237B8; 0x5E2413 m_quests[50] / count / numSortTypes 0xC237B4; 0x5E242F lea desc+278h indexed
- 0x5E2630 GetQuestLeaderBoardForItem: 0x5E26BF m_quests[50] / count / numSortTypes 0xC23AD0; 0x5E26CA m_quests[50] / count / numSortTypes 0xC237B8; 0x5E26D3 m_quests[50] / count / numSortTypes 0xC237B4; 0x5E26EF lea desc+278h indexed
- 0x6D7DE0 CGPlayer_C::CheckForQuestItemLoot: 0x6D7E32 add desc+278h indexed
- 0x6D8030 Packet_Group_17 (SMSG_QUESTUPDATE_*): 0x6D8091 add desc+278h indexed
- 0x6DF370 OnUpdateQuest: 0x6DF3E4 lea desc+278h indexed
- nofunc 0x9CB285 static constructor of m_questPOIs: 0x9CB285 m_questPOIs[25] 0xC23B0C
<!-- end of generated output -->

What the table does not list:

- The aligned dwords in the data segments that point into a range. The scan found none.
- The end-of-array pointer 0xC23FB4 in the atexit destructor of `m_questPOIs` (0x9D64D0). It lies outside the range.
  It frees the blobs of the old POI array, which is empty because the POI group moved to `s_pois`.
- The CoA64 "ignore" rows: 0x6E5180 / 0x6E5960 mirror handlers, and 0x614190 / 0x614300 script-event registration.
  They pass descriptor offsets as immediates and read no slot.
- 0x5E0420, 0x5E0D20, 0x61E5C0 and 0x6D4F80. These original bodies read no descriptor slot themselves. Their DLL
  wrappers (AscInitHooks, AscGossipQuest) read all 64 slots.
- `CGPlayer::GetQuestLogEntry` (0x5E0870) and its callers, which get the descriptor through `this`. That function is
  replaced.

## Rerunning the audit

The script reads the covered set from `src/Ascension/AscQuestLog64*.cpp` each time it runs, so it stays correct as
the tables change. It needs an IDA database of `Ascension.exe`. Work on a copy: never write into the game folder.

```bat
mkdir %TEMP%\ql64 & copy D:\WoW\Coa-Dev\Ascension.exe %TEMP%\ql64\
"C:\Program Files\IDA Professional 9.5\idat.exe" -A -L%TEMP%\ql64\audit.log ^
  "-S%CD%\tools\ida\quest_log_64_xrefs.py --out %TEMP%\ql64\audit.md" %TEMP%\ql64\Ascension.exe
echo %ERRORLEVEL%
```

Run it from the repository root. The first run analyses the binary and writes `Ascension.exe.i64`, which takes a few
minutes. Later runs can open the `.i64` instead, and take about a minute. The exit status is 1 when a row is
UNCOVERED or when the script reports a problem. A problem is one of these:

- a table byte that does not match the binary;
- a table entry outside the function it names;
- a rewritten DLL site that the descriptor heuristic does not find;
- an operand patch that is not among the references found.

Without `--out`, the report goes to the IDA output only. Options:

- `--repo PATH` reads another checkout.
- `--no-exit` keeps IDA open, for the GUI or an MCP session. The result is then in `QL64_RESULT`.
- `python -I tools/ida/quest_log_64_xrefs.py --parse-only` checks, without IDA, that the source tables still parse.

To change what the script accepts, edit the source tables. An unexplained reference becomes harmless only through a
`HARMLESS` rule in the script, and each rule must give a reason that has been checked in IDA. The report lists rules
that matched nothing, so stale ones show up.

The descriptor heuristic and its limits are described at the top of the script. In short:

- It follows registers and `[ebp+x]` slots that hold `[player+1008h]` or `[obj+8]`, with constant offsets.
- It reports the first instruction that forms or reads descriptor +0x278..+0x46B.
- It does not follow control flow, values that cross a call, or pointers passed as `this` or arguments.
- As a recall check, every slot site the DLL rewrites must be found.

## In-game checklist

Setup:

- **Server:** `F:\WorkDir\azerothcore-wotlk-coa\var\build\ql64\bin\worldserver.exe` (branch `feat/quest-log-64`), with
  `Quests.LogSize = 64` in the worldserver.conf it uses.
- **DLL:** build into `out\` (`cmake --build out`) and copy `out\Extensions.dll` next to
  `D:\WoW\Coa-Dev\Ascension.exe`.
- **Character:** about 40 quests in the log (`.quest add <id>` as GM). Some of them should be in slots 25..63 and be
  timed, auto-complete, item-drop, object or kill quests.
- **Log:** `Logs\Extensions.log` should have no `AscQuestLog64List: ... left unpatched` / `does not match` line.

Mark each line PASS or FAIL, with a note.

| # | Check | Result |
|---|---|---|
| 1 | The quest log lists all 40 quests with zone headers, and the counter reads "Quests: 40/64" | |
| 2 | Quests 26–40 can be tracked in the watch frame; more than 25 tracked at once; the tracked set survives `/reload` | |
| 3 | Quests 26–40 show POIs on the world map and minimap. The watch-frame POI buttons and the SuperTrack arrow work for them | |
| 4 | Quests 26–40 show the scaled level colour and reward XP in the log | |
| 5 | Killing a mob for an overflow quest (e.g. quest 30) updates the tracker and the log's objective lines | |
| 6 | A quest item for an overflow quest (e.g. quest 35) drops as quest loot and shows the "x/y" message. Its quest objects sparkle | |
| 7 | Abandoning an overflow quest (e.g. quest 38) removes it from the log and from the server | |
| 8 | An overflow quest (e.g. quest 33) can be shared with a party member | |
| 9 | Turning in a quest in slots 1–25 moves an overflow quest up; it is shown once, with no duplicate | |
| 10 | After `/reload`, all quests are still listed | |
| 11 | Logging out to another character shows that character's own log, with no leftovers | |
| 12 | The Extra Quests window never appears | |
| 13 | After setting `Quests.LogSize = 30` and running `reload config` while logged in, the counter reads n/30 without a relog | |
| 14 | Against the old server build (`var\build\coa`), the client behaves as a 25-quest client: 25 in the log, the rest in the Extra Quests window | |
| 15 | Accepting a 26th quest shows "Quest accepted" once and fires `QUEST_ACCEPTED` once, with a single `QUEST_LOG_UPDATE` per packet | |
| 16 | No "Quest accepted" message appears at login or after `/reload` | |
| 17 | A quest in slot 25+ that fails (an escort, or a timed quest that runs out) shows as failed in the log, with the "quest failed" error | |
| 18 | A timed quest in slot 25+ shows in the quest timer frame and counts down | |
| 19 | Abandoning or turning in a quest in slot 25+ fires `QUEST_REMOVED` for it and clears its map point | |
| 20 | An auto-complete quest in slot 25+ completes on its own once its objectives are done | |
| 21 | The tooltip of a mob or object for a quest in slot 25+ shows the quest and its progress. Quest markers and `GetUnitQuestInfo` include it | |
| 22 | Quest links (shift-click) for quests in slots 25..63 show the right level and XP | |
| 23 | Zone headers 1–25 collapse and expand; headers past the 25th zone always show expanded (spec non-goal) | |
| 24 | Built with `-DASCENSION_QUEST_LOG_64=OFF` (`cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DASCENSION_QUEST_LOG_64=OFF -S . -B out\off`), against the same ql64 server: the genuine behaviour. The log holds 25 quests, the counter reads n/25, quests past 25 are in the Extra Quests window, and no CMSG `0x8F6` is sent | |
