# LumaHome AI Handoff

Last updated: 2026-08-22

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
- Current source commit: `11a0563` (`Observe active HOME renderer row state`)
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
