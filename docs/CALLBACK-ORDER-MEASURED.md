# Measured callback-list order — aura application, effect filtering, visual hiding

The three lists that used to insert by call-site order were measured from the original's live list
images. Call-site order cannot reproduce them: registration execution order does not follow the call
sites — and one original-only callback on the aura-application list has no call site at all.

## Captured order (call order of the original's detours and consumers)

| list (native object) | order |
| --- | --- |
| `0x10BE2B6C` aura application (before `0x724820`) | `AscAura137::BeforeApply`, `AscSpellVisuals::BeforeAuraApply`, `AscSpellMods::AuraFlagOn`, `AscAppearance::BeforeAuraApply`, `AscSpawnVisibility::BeforeAuraApply`, *+ one original-only callback (below)* |
| `0x10BE2D78` effect filtering (`0x744AC0` replacement) | `AscSpellVisuals::RemoveEffect`, `AscQuestSpellHide::RemoveEffect` |
| `0x10BE2D98` visual hiding | `AscSpellVisuals::HideVisual`, `AscQuestSpellHide::HideVisual` |

Sites (reference image) are in `tests/fixtures/original_callback_order.json`; the registrars rank by them.
The two filter lists are exactly reversed vs ascending site order. The aura list is not any site sort:
`AscSpellMods` registered before `AscSpellVisuals`, and `AscSpawnVisibility` before `AscAppearance`,
although their sites are the other way round.

## The original-only sixth callback (aura application)

- Runs **last of six** (`AscSpawnVisibility`'s entry precedes it). The list count for `0x10BE2B6C` is 6.
- Registered through the original's runtime layer: no static registration site exists anywhere in the
  image; its address is only materialized at runtime (a VM lane cell, `.vm_sec` + `0xD70088`). It is not
  reconstructed; `InsertByCapturedOrder`'s *unknown callbacks follow the captured entries* rule
  reproduces its position.
- Behaviour — a spell veto called as `(unit, a, rec)`; `false` suppresses the aura application:

```c
bool AuraVeto(void* unit, uint32_t, uint32_t rec)
{
    if (*(uint32_t*)((char*)unit + 0x14) != 4) return true;            // interposes only on players
    uint32_t spell = *(uint32_t*)rec;
    uint32_t flags = *(uint32_t*)(*(uint32_t*)((char*)unit + 8) + 0x258);   // PLAYER_FLAGS
    if (spell == 0x79BB0 || spell == 0x79C3F)
        return unit != ActivePlayer() && (flags & 0x8000) == 0
            && ((flags & 8) != 0 || AccountInfo()[0x0C] != 0) && AccountInfo()[0x0E] == 0;
    if (spell == 0x79BC5)
        return (flags & 0x8000) == 0;
    if (spell == 0x79C40 || spell == 0x978960)
        return unit != ActivePlayer() && (flags & 0x8000) == 0
            && ((flags & 8) != 0 || AccountInfo()[0x0D] != 0) && AccountInfo()[0x0E] == 0;
    return true;
}
```

(Flag `8` = GM, `0x8000` = developer; `AccountInfo()` is the TLS account object accessor —
our `FUN_1008c870` class. All five spell ids are Ascension custom spells, absent from the shipped
`Spell.dbc`.)

## Measurement and validation

- Live walk of the original's list links in two independent game sessions of a sibling build
  (`md5 8b3dd390…`), identical results; direction follows the hooks: `node = *(head)` (the list
  head pointer), callback at `node+8`, next at `node+0`, stop when back at `head`.
- The same walk reproduces the two emulated fixtures (`Before71E930`, `After80B5D0`) byte-exactly in this
  reference image; addresses were translated with per-band deltas validated on all entries.
- `tests/native_callback_order.py` locks the orders in: the registrars run under all registration
  permutations, with vetoes, duplicate registrations, an off-array ("unknown") site, and — for visual
  hiding — the production consumer's first-true-wins short-circuit.
