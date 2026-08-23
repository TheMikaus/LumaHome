# LumaHome AI Handoff

Last updated: 2026-08-23

## Project goal

LumaHome is an experimental Luma3DS fork that provides a reusable HOME Menu
hook/runtime. The first feature is alphabetical sorting from an on-device
overlay; planned features include alternate ordering, optional hole collapse,
folder policies, live sorting, and live search/filtering.

This file is append-oriented project context for another AI or developer. Keep
it current after every hardware iteration. Preserve prior findings when adding
new results; do not replace useful history with a short status summary.

## Repository state

- Remote: `https://github.com/TheMikaus/LumaHome.git`
- Development branch: `lumahome/home-menu-framework`
- Upstream base: Luma3DS `d30ac8d` (after v13.4)
- Current source commit: `9f980df` (`Add AI project handoff`)
- Current device build: RC47, derived from the V195 working runtime
- Overlay shortcut: `L + Y` only
- Current test console: USA HOME Menu, title ID `0004003000008F02`

The source still uses some `cthulhu_` names and compatibility paths inherited
from the prototype. Do not mechanically rename these while runtime behavior is
under investigation; they are part of the currently working ABI and logging.

## Safety and deployment

- Build output is `boot.firm`, but development builds must be renamed to a
  versioned `LumaHome*.firm` and chainloaded from `/luma/payloads`.
- Never overwrite the SD card's known-good root `/boot.firm` during testing.
- Verify the SD drive letter instead of assuming it.
- Bump the visible overlay version for every device build so the tester can
  prove which payload actually booted.
- Preserve automatic logs and ARM11 dumps before deploying another build.
- Stop after a crash, prolonged hang, continuous flashing, loss of HOME, or
  broken power-off; inspect logs/dumps before repeating the operation.

## Verified hardware behavior

- The top-screen overlay renders and opens with `L + Y`.
- Digital controls are responsive and HOME navigation is suppressed while the
  overlay is open.
- The hook and overlay recover after returning from Notifications.
- Power-off works in the present baseline.
- A-Z and Z-A title sorting have worked live in earlier iterations.
- Persistent sorting and folder before/after placement have worked across a
  reboot in tested iterations.
- Folder names and contents can be preserved when the safe persistent path is
  used.

Regressions previously seen include wrapped titles, stale icon maps, gaps,
folders disappearing or returning to old positions, incorrect first-page
placement, crashes during live refresh, and system applications invalidating
HOME hook/input state. Treat all persistent layout structures conservatively.

## Current implementation direction

The latest change observes HOME Menu's active renderer row state rather than
assuming or hardcoding an icon-grid width. The hook is at HOME runtime address
`0x0021D428`; the active controller row is read from `r4 + 0x1C4`. It publishes
the observed channel at shared-channel offset `+0x10C` and its generation at
`+0x110`. Row-major sorting currently fails with diagnostic `-125` when no
valid renderer observation has been captured.

The required ordering model is screen-bounded: fill every visible slot on the
current screen, then continue on the next screen. The dimensions must follow
HOME's currently selected icon layout and must be re-observed if the user
changes that layout. Do not use special title IDs, fixed slots, wrapped state,
or current occupancy as proxies for movable icons or grid dimensions.

## Next hardware iteration

Test RC47 and capture its automatic report:

1. Boot the versioned RC47 payload and verify the visible version.
2. Change HOME's icon-size/layout selection.
3. Apply row-major A-Z and inspect whether the observed renderer state is
   accepted or reports `-125`.
4. Verify screen-bounded placement across the first page boundary.
5. Change the HOME layout again without rebooting and repeat the sort.
6. Verify folder before/after behavior, resume through Notifications, and
   power-off after the sort.

If it fails, use the report's observed channel/generation and sort-stage values
to determine whether the renderer hook did not execute, the decoded state was
rejected, or placement consumed the wrong dimensions. Add diagnostics to the
automatic log rather than asking the tester to transcribe screen values.

## Related documentation

- `LUMAHOME.md` — public project overview and build instructions
- `TEST-PLAN.md` — older release-candidate test structure; update its version
  before using it verbatim
- The companion Cthulhu repository's `docs/AI-HANDOFF.md` and
  `docs/SORT-PLACEMENT.md` contain the longer prototype history and placement
  analysis.

## Iteration discipline

For every new hardware iteration:

1. Read this handoff and the latest relevant commits before changing hooks.
2. Keep the last known-good hook path available when testing a separate resume
   or renderer hook.
3. Add enough automatic diagnostics to distinguish discovery, calculation,
   persistent write, live publication, and redraw failures in one SD-card trip.
4. Build, record the payload hash, deploy under a unique visible version, and
   document exactly what the tester should exercise.
5. Append the result, conclusion, and next action here; commit the code and
   documentation together when practical.

## 2026-08-23 — code review and project-direction decisions

No hardware iteration. Source review of `9f980df` plus a documentation pass.
Full findings in `REVIEW.md`; the decisions below change the roadmap and
supersede parts of "Current implementation direction" above.

### Scope decisions (from the maintainer, 2026-08-23)

- **The project is intended to be shareable, not single-console.** This
  retires the hardcoded-offset static patch as an acceptable end state.
- **Sorting is the priority over search.**
- **Live application is a hard requirement.** Sort-applies-on-next-HOME-reload
  is not an acceptable fallback.
- **Test fleet is four consoles**: Old 3DS, New 3DS, 2DS, New 2DS XL — i.e.
  both the O3DS-family and N3DS-family hardware classes.
- **The maintainer is the tester on every iteration.** Round-trip cost is the
  dominant constraint on velocity and should be optimised aggressively.
- **This handoff must be updated at the end of every working session**, not
  only after hardware iterations.

### What the review confirmed is working and must not be regressed

The live-refresh mechanism is the project's genuinely novel contribution. The
ARM11 stub polls a shared command channel each frame and invokes HOME Menu's
own icon-refresh callback from HOME's own frame thread
(`home_menu_diagnostics.c:4497-4511`). Every comparable tool —
HomeMenuEditor3DS, Cthulhu, `3ds_homemenu_extdatatool` — instead goes through
the FS archive API, hits `0xc92044e7` "Resource locked" while HOME Menu is
resident, and therefore requires HOME Menu to be evicted or the console
rebooted. Living inside the process avoids the lock entirely. Preserve this
path across any refactor.

`CthulhuHomeMenu_Search` is likewise structurally sound and independent of the
fragile machinery: it builds its catalog from `AM_GetTitleList` plus SMDH
names out of the HOME icon cache and launches through
`PMAPP_TerminateCurrentApplication` + `PMAPP_LaunchTitle`. No address
patching, no layout arithmetic, no persistence.

### Blocking defect: `-125` is self-inflicted

`ApplySdSort` computes `persistedRows` from `Launcher.dat` offset `0xB51`
("rows on HOME Menu minus 1") and uses it directly in the column-major path.
In row-major mode it discards that value and fails hard when the renderer hook
has not yet published an observation:

```c
persistedRows = (u32)g_launcherRaw[CTH_HOME_ROWS_OFFSET] + 1;
homeRows = persistedRows;
if (rowMajor)
{
    if (g_activeRowsGeneration == 0 || g_activeRowsStored > 5)
        res = (Result)-125;
    else
        homeRows = g_activeRowsStored + 1;
}
```
*(`home_menu_diagnostics.c:2842-2848`)*

Action: demote the renderer observation from precondition to override. Fall
back to `persistedRows`, and record the source used in the `[TRAVERSAL]`
block. This unblocks RC47 on the next boot and produces the evidence needed to
decide whether the renderer hook at `0x0021D428` is required at all — the
savedata already carries the row count, and `0xB5E` (horizontal scroll,
÷ rows = hidden columns) may make the hardcoded
`columnsByRows[] = {0,3,3,5,6,8,10}` table redundant as well.

### Root cause of the RC1→RC47 iteration count

Roughly 5,000 lines of pure arithmetic — `TraversalKey`,
`SortPositionsForTraversal`, `CompactTraversalPosition`, `SortPositions`,
`SortRequestEntries`, folder placement, gap collapse, `ValidateLayout`,
`BuildSdGridFromRaw`, `MergePendingSortWithCurrent` — take a `.dat` buffer plus
a row count and return positions. None of it has any 3DS dependency, and none
of it is currently reachable without building a `.firm`, renaming it, swapping
an SD card, booting, sorting, powering off, and reading logs.

Every regression listed under "Verified hardware behavior" above — wrapped
titles, stale icon maps, gaps, folders returning to old positions, incorrect
first-page placement — is a pure-function bug expressible as a unit test.

Action: extract the layout core into a host-buildable library with an
`#ifdef __3DS__` shim, and drive it from a test suite built on real dumped
`.dat` fixtures. This is the highest-leverage change available and is a
prerequisite for the shareability work below, because four consoles × several
regions × multiple HOME Menu versions cannot be hand-validated.

### Shareability: what the decision costs

`CthulhuHomeStaticPatch_Apply` currently requires all of: title ID exactly
`0x0004003000008F02` (USA), 17 exact ARM instructions at 17 exact addresses,
and a zero-filled code cave at `0x00305174`. Any mismatch returns `false`
**silently** and the entire feature set is inert with no on-screen
explanation — a HOME Menu system update is therefore indistinguishable from a
code regression. `BuildCatalogInRosalina` separately hardcodes extdata ID
`0x8F`, and `ApplySdSort` carries hardcoded per-title name aliases
(`VirtuaNES`, TWiLight Menu++) that belong in a user-editable file on SD.

Region IDs needed for the runtime lookup (title-ID low / SD extdata ID /
NAND system-save ID): JPN `8202` / `0x82` / `0x00020082`, USA `8F02` / `0x8F` /
`0x0002008F`, EUR `9802` / `0x98` / `0x00020098`; CHN `A102`, KOR `A902`,
TWN `B102`. Note that `WriteLauncherFolderPositions` already uses
`0x0002008F` correctly, so the NAND save-ID pattern is confirmed in working
code. Console model does not affect any of these IDs — only region does.

Planned sequence, in dependency order:

1. **Reduce the hook surface.** Eleven hook points is eleven per-build
   liabilities. Audit which are load-bearing; the active-rows observation hook
   is the first candidate for removal (see the `-125` item above).
2. **Signature-scan instead of hardcoding offsets.** Locate each remaining
   hook by byte pattern with wildcards over the decompressed `.code` at load
   time. Reject on zero or multiple matches rather than guessing.
3. **Find the code cave dynamically.** `0x00305174` is not guaranteed free on
   another build; scan for a sufficiently long zero run in an executable
   section.
4. **Derive region at runtime** rather than compiling USA in.
5. **Ship a build-fingerprint table.** Hash the `.code`; record region, HOME
   version, hardware class, and resolved hook offsets. Fall back to scanning
   on unknown builds and write resolved offsets to SD so users can contribute
   them back.
6. **Fail loudly and specifically** at every step above.

### Fixtures: they may already exist on the SD card

LumaHome already writes both layout blobs to SD during a sort attempt:

- `/3ds/Cthulhu/pre-folder-commit-Launcher.dat` (`0x2490`, via
  `BackupCurrentLauncherData`)
- `/3ds/Cthulhu/pre-sort-SaveData.dat` (`0x2DA0`, via `BackupSdSaveData`)
- `/3ds/Cthulhu/post-sort-SaveData.dat` (via `SaveCommittedSnapshot`)

Caveat: `BackupSdSaveData` is called at `home_menu_diagnostics.c:3265`, well
after the `-125` grid check at line 2842, so a sort that fails on `-125` does
**not** produce a backup. Worth moving the backups to immediately after
`ReadSdSaveData()`/`ReadCurrentLauncherData()` so a failed run still yields
usable fixtures — that alone makes every future failed iteration scientifically
useful instead of wasted.

For a clean pre-LumaHome baseline, or for a console that has never run
LumaHome, GodMode9 is the extraction route; note that SD extdata files are
DIFF containers, so the friendly `user/SaveData.dat` path is a virtual FS view
and cannot be copied straight off the card with a PC reader.

### Documentation state

Version strings disagree across the repository: `LUMAHOME.md` says `rc8`,
`TEST-PLAN.md` says `rc1`, the code says `rc47`
(`CTH_SORT_BUILD_VERSION`). Generate the version from a single source.
`TEST-PLAN.md` cannot be run verbatim against the current build.

There is no CI. A compile-only devkitARM job on push would catch a class of
breakage that currently costs an SD-card round trip to discover.

### Next actions

1. `-125` fallback to `persistedRows` — unblocks the pending RC47 test.
2. Move the SD/Launcher backups before the grid check so failed runs still
   produce fixtures.
3. Extract the pure layout core and stand up host-side unit tests.
4. Loud, specific patch-failure diagnostics, then signature scanning.
5. Revisit search polish (NAND/cart titles are currently filtered out by
   `SearchEntryMatches`; the L/R alphabet input needs replacing) once sorting
   is off the critical path.
