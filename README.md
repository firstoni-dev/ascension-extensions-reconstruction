# Ascension Extensions Reconstruction

A function-by-function reconstruction of Project Ascension's client extension `Extensions.dll` for the
WoW 3.3.5a (build 12340) client `Ascension.exe`, built on the WotLK-Extensions scaffold
(`WotLKExtensions/`, MIT, see `UPSTREAM-README.md` and `LICENSE`).

The goal is an **exact** reconstruction: every Lua native, packet handler, client hook, CVar and code patch of
the genuine DLL, with the same behaviour, including its bugs. Original defects are reproduced and recorded
separately, not fixed.

- Lua natives: 1,277 / 1,277 (namespace, name) pairs
- Packet handlers: 530 / 530
- Client hooks: 400 / 400
- CVars: 93 / 93

## Reference

**[`docs/DLL_REFERENCE.md`](docs/DLL_REFERENCE.md)** documents the whole DLL for people who want to use it
without reading the code: every Lua function (state, arguments, returns), every server → client and client →
server packet, every custom event, CVar and client hook, grouped by module. Each entry gives the genuine DLL's
address and our `file:line`. The introduction explains how the DLL loads, registers its natives, routes
packets and fires events.

## Deliberate differences from the genuine DLL

- **Logon protocol.** The genuine DLL hooks `0x8CCE00`, `0x9A83E0` and `0x9A88C0` to replace SRP6 with
  Ascension's own logon protocol. **Not installed by default** -- the client authenticates with stock SRP6
  against an AzerothCore authserver. A native reconstruction of the protocol (X25519 / ChaCha20 /
  HMAC-SHA256, plus the four handshake length gates) now ships in `src/Ascension/AscNativeLogon.cpp`,
  compiled in only when the build enables `-DASCENSION_NATIVE_LOGON=ON` (see below); it exists so the
  client can be tested against a server that speaks the original protocol. See `src/Ascension/AscMain.cpp`.
- **Character advancement initialization.** The native `0x726` known-entries handler synchronizes the
  build's unit parameters before notifying listeners and cloning its pending build. Waiting for the next
  manager tick leaves a new build at class and level zero during the first native update. This is an
  intentional compatibility correction; it uses the existing parameter helpers, packets and Lua bindings.
- **Tooltip line insertion.** Ascension's own `Tooltip:InsertLine` / `Tooltip:InsertLines` (patch-B
  `SharedXML\TypeExtensions\Tooltip.lua`) write inserted lines into font strings left over from a longer
  tooltip, so a keystone tooltip shows one Dungeon Modifier of several. After the world UI loads, the DLL wraps
  both methods so that lines past the tooltip's end are added instead (`src/Ascension/AscTooltipInsertFix.cpp`).
  `patch-B` itself is unchanged.
- **Indoor minimap after a reload.** Indoors, after some `/reload`s, the minimap shows part of another UI texture
  (often the calendar). A new indoor render target from the reload can draw the wrong texture; the one made at login
  never does. The DLL keeps the login target for the whole session (`src/Ascension/AscMinimapTargetKeep.cpp`). The
  cause is not known; this is a workaround for the 32-bit client only. The 64-bit CoA64 client does not show the
  defect and does not need it.
- **Interface hot patches.** Not in the genuine DLL. Compiled in only with `-DASCENSION_HOT_PATCH=ON` (see below):
  a supported channel for server-sent interface Lua (SMSG `0x8F0`, with the capability and result packets
  CMSG `0x8F1` / `0x8F2`), and a check that refuses SMSG `0x5BC` challenge requirement-type rows whose formatters
  carry anything but a display expression. See `src/Ascension/AscHotPatch.cpp`.
- **Quest log of 64 quests.** Not in the genuine DLL, and **on by default** (unlike the other additions). The server
  sends quest log slots 25..63 in SMSG `0x8F7`, and the DLL announces CMSG `0x8F6` (u8 version 1, u8 64) after every
  world entry; a server that never answers keeps the native 25 slots. `AscQuestLog64::SlotEntry` is the client's
  quest log entry for any of the 64 slots. See `src/Ascension/AscQuestLog64.cpp`; `tests/quest_log_64.py` checks the
  store. Build with `-DASCENSION_QUEST_LOG_64=OFF` for the genuine 25-slot behaviour.

The portable regression in `tests/native_ca_packets.py` compiles the production native packet handlers
against client API doubles. It covers initial state, listener timing, subsequent updates, new preset slots
and missing local units. It does not launch the game or replace the 32-bit Windows DLL build.

`tests/native_callback_order.py` exercises the production cast, aura-removal and aura-application
registrars and detours, and the effect-filter and visual-hide registrars and consumer, against the
original DLL's measured linked-list traversal, including registration permutations and vetoes.
The original uses MSVC `unordered_set` callbacks: cast checks run aura requirements, stack requirements,
then class requirements; aura removal runs `AscAura137` before `AscSpellMods`; aura application runs
`AscAura137`, `AscSpellVisuals`, `AscSpellMods`, `AscAppearance`, then `AscSpawnVisibility`; effect
filtering and visual hiding run `AscSpellVisuals` before `AscQuestSpellHide`. These snapshots include
all corresponding callbacks in this source; the two emulated lists' native hash-bucket relationships
remain identical at every allocation-aligned 32-bit image base, so relocating the DLL preserves these
orders.
All five captured lists insert by the measured order (`InsertByCapturedOrder`): call-site ordering did
not reproduce them because registration execution order does not follow the call sites. The
aura-application list also carries one original-only callback that is not reconstructed - it runs after
the five captured entries, matching the rule that unknown callbacks follow the captured entries.

## Building

32-bit MSVC (Visual Studio 2022 Build Tools), CMake and Ninja, from an x86 developer prompt (`vcvars32.bat`):

```bat
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -S . -B build
cmake --build build
```

The output is `build\Extensions.dll`. Place it next to `Ascension.exe`, which loads it itself (there is no
injector).

### The live client DLL

The DLL shipped to players is built with the `ship` preset: interface hot patches, DBC streaming and the
64-slot quest log on, native logon off.

```bat
cmake --preset ship
cmake --build --preset ship
```

The output is `out-ship\Extensions.dll`.

### Optional: without the 64-quest log

The quest log of 64 quests is compiled in by default. `-DASCENSION_QUEST_LOG_64=OFF` leaves it out and gives the
genuine 25-slot behaviour.

### Optional: interface hot-patch build

```bat
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DASCENSION_HOT_PATCH=ON -S . -B build\hp
cmake --build build\hp
```

This compiles in `src/Ascension/AscHotPatch.cpp`. After every world entry the DLL announces CMSG `0x8F1`
(u32 protocol version 1, u8 mode), so a server that understands it sends interface Lua as SMSG `0x8F0` (sized patch
id, sized target addon, sized Lua; u32 length + bytes each) instead of smuggling it through a challenge formatter.
Each patch:

- must be source (no ESC byte, no `loadstring`, no `string.dump`), at most 32 KB, 128 KB per session;
- runs once per patch id and content hash per Lua session (a relog or `/reload` clears it);
- is kept until the next glue screen and runs again after a `/reload`, because the server sends patches only at login;
- runs at once with no taint source when it names no addon, or at that addon's `ADDON_LOADED` with the addon's
  taint (so it gets exactly the trust SMSG `0x94E` gave the addon) when it names one;
- may return `"applied"` or `"skipped: <reason>"`; the outcome goes back to the server as CMSG `0x8F2`.

Patches are allowed by default. `/hotpatch off` refuses them, `/hotpatch ask` asks once per realm,
`/hotpatch on` allows them again, and `/hotpatch` alone lists this session's patches. The choice is kept in
`WTF\HotPatch.txt`.

The build also drops any SMSG `0x5BC` (`ChallengeRequirementTypes`) row whose formatter is not a single
`return <expression>` over `value`, literals, operators and the seven functions the shipped table calls.
`tests/hot_patch_formatter.py` checks that every shipped formatter passes and that smuggling shapes are refused
(run it from an x86 developer prompt, or set `CXX`).

### Optional: native logon build (test against an Ascension-protocol server)

```bat
cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DASCENSION_NATIVE_LOGON=ON -S . -B build\nl
cmake --build build\nl
```

This compiles in `src/Ascension/AscNativeLogon.cpp`: it installs the original DLL's three logon hooks
(`0x8CCE00`, `0x9A83E0`, `0x9A88C0`) plus the four handshake length gates, replacing the client's stock SRP6
with Ascension's own handshake (X25519 + ChaCha20 + HMAC-SHA256). It only works against a server that
answers that protocol. The server's X25519 public key is a build setting,
`-DASCENSION_NATIVE_LOGON_SERVER_KEY=<64 hex chars>` (default: the key live-tested during the 2026-10
campaign; the server prints its key at startup). Run the client with `-extlog 1` and check
`Logs\Extensions.log` for the `native-logon:` lines.

Live-validated end to end on 2026-10-10 against a `feature/ascension-native-login` server
(`CoA.AscensionAuth = 1`, matching key pair): the client logged `native-logon: proof check PASSED
(HMAC-SHA256(key_derived, "OK"))`, the server answered `Ascension custom auth: proof replied, session
authorised`, and the client went on through `COP_AUTHENTICATE ... AUTH_OK` to the character list
(`COP_GET_CHARACTERS ... result=TRUE`). No client-side patches were involved: this build installs its own
trampolines and length gates.

Key generation, the server branch and its `coa.conf` settings, and troubleshooting are in
[`docs/NATIVE-LOGON.md`](docs/NATIVE-LOGON.md).

One server requirement found during that test: after the custom proof the authserver must persist the
handshake's 40-byte `key_session` as `account.session_key`. The worldserver validates the client's
`CMSG_AUTH_SESSION` digest and initialises its header cipher from that value - without it every world
connection fails its digest check immediately after the auth completes. The partner fork
(`feature/ascension-native-login`) handles this in `AuthSession.cpp` (`HandleLogonProof`, Ascension
client branch).

The DLL's own developer log (`Logs\Extensions.log`) is off by default; start the client with `-extlog 1` to
turn it on.

## Server: required change to `azerothcore-wotlk-coa`

Server: **[jealous-sound/azerothcore-wotlk-coa](https://github.com/jealous-sound/azerothcore-wotlk-coa)**

**Set `CoA.PlaintextWorldHeaders = 0`** in the worldserver's `etc/modules/coa.conf`, then restart the
worldserver:

```ini
CoA.PlaintextWorldHeaders = 0
```

The shipped default (`coa.conf.dist`) is `1`, which assumes the client keeps world packet headers plaintext
after `CMSG_AUTH_SESSION`. This client does not: it always encrypts world headers with the stock 3.3.5a header
cipher. With the default `1` the server can't read the client's headers, and the client stalls for 30 seconds
at "Authenticating" and then disconnects before character select.

`CoA.Enable = 1` is also required. For clients that do not connect from loopback, set
`CoA.AllowRemoteClients = 1` too, because both settings apply only to loopback otherwise.

The authserver side of the custom handshake additionally requires the `key_session` persistence described
in the native-logon section above (`AuthSession.cpp`, Ascension client branch).

## Client: required changes to `patch-B.MPQ`

The stock Ascension client's `Data\patch-B.MPQ` is built for the Ascension Live servers. To connect to any
other server, four glue files in it must be replaced and the realm list must point at your server.

### 1. Replace four files in `Data\patch-B.MPQ`

| File in the archive | Size | SHA-256 |
|---|---|---|
| `Interface\GlueXML\RealmData.lua` | 48,281 | `229f8f668e1e0df86404719555d935b38ddad8e05d4af4e9d2437f4af64f9b62` |
| `Interface\GlueXML\RealmList\RealmList.lua` | 18,860 | `6a754d08a9b38dbefa237fdd131dae84ec1511546205314584d46366af33ceea` |
| `Interface\GlueXML\RealmList\Realm.lua` | 10,605 | `09bbfe13356afb9f16c3027cfc2622134e0295890956c85cd9470a9137e7a50b` |
| `Interface\GlueXML\RealmList\RealmScrollListItem.lua` | 3,422 | `0a717d44a9bddea0d97b948cc4673724f65823d6d032bf38fca32acd4e5a55ba` |

These are the versions from the patched client prepared for `azerothcore-wotlk-coa`. `RealmList.lua`
identifies itself as `ASCENSION_LOCAL_REALM_LIST_PATCH = "20260913-advertised-realm-cards-v4"`. What each one
fixes:

- **`RealmData.lua`.** The Live version sets `GLOBAL_REALMLIST = "51.210.230.10"` (Ascension Live) when it
  loads, whatever `realmlist.wtf` or `Config.wtf` say, so the client always connects to Live. The replacement
  honours `realmlist.wtf`. The file looks obfuscated (it is a Lua-implemented bytecode VM) but it's ordinary
  Lua.
- **`RealmList.lua`, `Realm.lua`, `RealmScrollListItem.lua`.** The Live realm list only presents realms that
  appear in Ascension's built-in realm catalog. A realm with any other name falls into a hidden developer page,
  so the realm cards come up blank and the realm shows as down. The replacements build a normal realm card for
  any realm the server advertises that isn't in the catalog (`ReadAdvertisedRealms` /
  `AddAdvertisedRealmCards`).

The archive's files are encrypted. Ladik's MPQEditor can read and write them from the command line:

```bat
MPQEditor.exe add "<client>\Data\patch-B.MPQ" "<local>\RealmData.lua" "Interface\GlueXML\RealmData.lua"
```

Back up `patch-B.MPQ` first, and check each file after adding it by extracting it again
(`MPQEditor.exe extract ...`) and comparing its SHA-256 with the table.

### 2. Point the realm list at your server

`Data\enUS\realmlist.wtf` (use your locale's folder):

```
set realmlist 127.0.0.1
```

Put your authserver's address in place of `127.0.0.1`. The authserver's `realmlist` table must advertise the
worldserver's reachable address and port.
