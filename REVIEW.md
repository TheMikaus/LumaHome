# LumaHome review — 2026-08-23

Reviewed at commit `9f980df` on `lumahome/home-menu-framework` (50 commits over
upstream Luma3DS `d30ac8d`, +6,987 lines).

## Summary

The hard part is done and it works. The thing making the project miserable is
not the 3DS — it's that a 5,000-line pile of pure layout arithmetic can only be
tested by physically booting a console. That's why you're on RC47.

Two of the three headline features are in far better shape than the docs imply:

- **Live refresh works.** The ARM11 stub polls a shared command channel each
  frame and calls HOME Menu's own icon-refresh callback from HOME's own frame
  thread (`home_menu_diagnostics.c:4497-4511`). Every external tool
  (HomeMenuEditor3DS, Cthulhu, 3ds_homemenu_extdatatool) requires HOME to be
  evicted or rebooted, because they go through the savedata API and hit
  `0xc92044e7` "Resource locked". You sidestepped that entirely by living
  inside the process. Do not lose this.
- **Search is essentially done and structurally clean.**
  `CthulhuHomeMenu_Search` touches none of the fragile machinery — it builds a
  catalog from `AM_GetTitleList` plus SMDH names out of the HOME icon cache,
  and launches via `PMAPP_TerminateCurrentApplication` + `PMAPP_LaunchTitle`.
  No address patching, no layout math, no persistence.

Sorting is the research project. It carries all the risk and all 47 RCs.

---

## Findings, in priority order

### 1. `-125` is a self-inflicted hard failure — fix this first

`ApplySdSort` already computes `persistedRows` from `Launcher.dat` offset
`0xB51` ("rows on HOME Menu minus 1") and uses it happily in the column-major
path. In row-major mode it throws that value away:

```c
persistedRows = (u32)g_launcherRaw[CTH_HOME_ROWS_OFFSET] + 1;
homeRows = persistedRows;
if (rowMajor)
{
    if (g_activeRowsGeneration == 0 || g_activeRowsStored > 5)
        res = (Result)-125;              // <-- hard fail
    else
        homeRows = g_activeRowsStored + 1;
}
```
*(`home_menu_diagnostics.c:2842-2848`)*

Make the renderer observation an override, not a precondition: fall back to
`persistedRows` and record which source was used in `[TRAVERSAL]`. This is a
three-line change that unblocks RC47 on the next boot and — more usefully —
tells you empirically whether the renderer hook at `0x0021D428` was ever
needed. The savedata already carries the layout; `0xB5E` (horizontal scroll,
÷ rows = hidden columns) may make the hardcoded
`columnsByRows[] = {0,3,3,5,6,8,10}` table redundant too.

**Effort: minutes. Unblocks the current blocker.**

### 2. No offline test harness — the actual root cause of RC47

All of this is pure arithmetic over a byte buffer:

- `TraversalKey`, `SortPositionsForTraversal`, `CompactTraversalPosition`
- `SortPositions`, `SortRequestEntries`, `CompareRequestEntries`
- folder placement, gap collapse, `WriteLauncherFolderPositions`
- `ValidateLayout`, `BuildSdGridFromRaw`, `MergePendingSortWithCurrent`

Input: a `Launcher.dat` / `SaveData.dat` blob plus a rows value. Output:
positions. Zero 3DS dependency. None of it needs a console, and all of it is
currently only reachable by building a `.firm`, renaming it, swapping an SD
card, booting, sorting, powering off, and reading logs.

Extract it into a host-buildable library (`layout/` + a thin
`#ifdef __3DS__` shim) with a test suite driven by real dumped `.dat` files.
Every regression in your handoff's list — wrapped titles, gaps, folders
returning to old positions, incorrect first-page placement — is a pure-function
bug expressible as a unit test.

**This converts a ~10-minute human-in-the-loop iteration into `make test` in
two seconds. It is the single highest-leverage change in the project.**

### 3. The static patch is a one-console binary

`cthulhu_home_static_patch.c` requires all of:

- title ID exactly `0x0004003000008F02` (USA HOME Menu)
- 17 exact ARM instructions at 17 exact addresses
- a zero-filled code cave at `0x00305174`

Any of those off by anything → `CthulhuHomeStaticPatch_Apply` returns `false`
**silently**, and the whole feature set is inert with no on-screen explanation.
`BuildCatalogInRosalina` separately hardcodes extdata ID `0x8F` (USA).

Region title-ID lows are JPN `8202`, USA `8F02`, EUR `9802`, CHN `A102`,
KOR `A902`, TWN `B102`; matching HOME extdata IDs are JPN `0x82`, USA `0x8F`,
EUR `0x98`.

Two things worth doing regardless of whether you ever ship this to anyone else:

- **Fail loudly.** Report *which* signature mismatched, on screen and in
  `runtime.txt`. Right now a HOME Menu system update silently bricks the
  feature and looks like a code regression.
- **Signature-scan instead of hardcoding offsets.** Same hooks, found by byte
  pattern over the decompressed `.code` at load time. Survives version bumps
  and makes the failure mode diagnosable.

Full multi-region support is a separate, larger decision — see Questions.

### 4. Persistence keeps three models in sync, and that's where the bugs live

23 of 50 commits are folder/persistence fixes. There are three parallel
representations: persistent `Launcher.dat`/`SaveData.dat`, the live "processed
grid", and the live icon-class map (`ScanLiveIconClassV010Rc8`, ~780 lines).
Every listed regression is a sync bug between them.

Compute the layout **once** as a pure function (per #2), then derive both the
persistent write and the live publication from that single result. Deferring
the persistent write to shutdown (`675caf0`) was the right instinct; finish the
job by removing the parallel path rather than guarding it.

### 5. Search: small fixes, large payoff

- `SearchEntryMatches` rejects everything with `mediaType != 1` — SD only. NAND
  titles, system applications, and cartridges are invisible.
- The input method is L/R scrolling through a 39-character alphabet. A d-pad
  grid keyboard is a couple of hours and transforms it.
- The catalog is read from `sort-request.bin` and goes stale after any install.
  Rebuild on open, or stamp it and warn.
- `matches[900]` and the `nandCount + sdCount > 900` cap truncate silently.
- `RequestTitleToAscii` renders any non-ASCII character as `?`.

### 6. Housekeeping

- Version drift: `LUMAHOME.md` says rc8, `TEST-PLAN.md` says rc1, the code says
  rc47. Generate the version from one place.
- No CI. Even a compile-only devkitARM job on push would catch a class of
  breakage before it costs an SD-card round trip.
- `TEST-PLAN.md` is written against rc1 and can't be run verbatim.

---

## Recommended order

1. `-125` fallback to `persistedRows` (minutes) — unblocks RC47
2. Extract the pure layout core + unit tests on real dumps (the big one)
3. Loud, diagnosable patch failure + CI compile job
4. Search polish: NAND/cart titles, keyboard, catalog freshness
5. Signature scanning to replace hardcoded offsets
6. Collapse the three-model persistence into one computed result

Steps 1, 3 and 4 are shippable without touching the parts that scare you.
