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
  Ascension's own logon protocol. These are **not installed**, so the client authenticates with stock SRP6
  against an ordinary AzerothCore authserver. See `src/Ascension/AscMain.cpp`.
- **Character advancement initialization.** The native `0x726` known-entries handler synchronizes the
  build's unit parameters before notifying listeners and cloning its pending build. Waiting for the next
  manager tick leaves a new build at class and level zero during the first native update. This is an
  intentional compatibility correction; it uses the existing parameter helpers, packets and Lua bindings.
- **Tooltip line insertion.** Ascension's own `Tooltip:InsertLine` / `Tooltip:InsertLines` (patch-B
  `SharedXML\TypeExtensions\Tooltip.lua`) write inserted lines into font strings left over from a longer
  tooltip, so a keystone tooltip shows one Dungeon Modifier of several. After the world UI loads, the DLL wraps
  both methods so that lines past the tooltip's end are added instead (`src/Ascension/AscTooltipInsertFix.cpp`).
  `patch-B` itself is unchanged.

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
