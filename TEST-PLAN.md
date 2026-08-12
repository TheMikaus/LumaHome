# LumaHome 0.1.0-rc1 hardware test plan

Test this exact payload: `/luma/payloads/LumaHome010RC1.firm`.

Expected overlay title: `LUMAHOME 0.1 RC1`. Activation is `L + Y` only.
Do not run the legacy Cthulhu `.3dsx`; it is not part of this test.

If HOME Menu crashes, stops responding for more than 30 seconds, or develops
continuous flashing, power off and stop testing. Do not repeat the failing
operation. Reinsert the SD card so the crash dump and automatic logs can be
preserved.

## 1. Boot and identity

1. Chainload `LumaHome010RC1.firm` and enter HOME Menu.
2. Open the overlay with `L + Y`.
3. Confirm the title reads `LUMAHOME 0.1 RC1`.
4. Close it with `L + Y`.
5. Confirm HOME navigation, A, B, X, and the D-pad work normally while closed.

Pass: correct branding, responsive HOME Menu, and no visual corruption.

## 2. Overlay input ownership

1. Open the overlay.
2. Use Up/Down to change fields and Left/Right to change values.
3. While it is open, try navigating HOME Menu with the D-pad and pressing A.
4. Close the overlay and navigate HOME Menu again.

Pass: overlay controls respond on every press; HOME Menu does not navigate while
open and resumes immediately after closing.

## 3. Live title sorting in one session

Use a page with several titles whose alphabetical order is visually obvious.

1. Select A-Z and apply once.
2. Wait up to 15 seconds. Confirm icons move without rebooting.
3. Open the overlay and apply Z-A.
4. Confirm the same titles reverse without rebooting.
5. Apply A-Z again and confirm they return.
6. Apply A-Z a second time while already sorted.

Pass: all three direction changes occur live, the repeated A-Z is harmless, the
overlay remains responsive, and no sort reports `FAILED`.

## 4. Empty slots and multiple pages

1. Before sorting, leave at least one empty slot between titles if possible.
2. Apply Z-A and then A-Z.
3. Move across every HOME page and inspect icon positions.

Pass: no title disappears or duplicates; empty slots do not turn into blank
selectable applications; page navigation remains valid. Record whether holes
are preserved, because collapse-holes is not implemented yet.

## 5. Folder integrity and placement

Use a user-created folder containing at least three titles with clearly
different names.

1. Note the folder's name, location, and the order of titles inside it.
2. Set folder placement to Before Titles and apply A-Z.
3. Check whether the folder moves live; open it and check its contents/order.
4. Set folder placement to After Titles and apply Z-A.
5. Again check the folder location, identity, and contents.
6. Close and reopen the folder twice.

Pass for data safety: the folder is never deleted, renamed, emptied, duplicated,
or converted into a wrapped package. Record separately whether folder movement
and its contents update live; either may remain a release limitation even when
persistent placement is correct after reboot.

## 6. System-application resume

1. Open Notifications and return to HOME Menu.
2. Open LumaHome with `L + Y`.
3. Test every digital overlay control.
4. Apply the opposite sort and confirm live movement.
5. Close the overlay and confirm normal HOME input.

Pass: overlay rendering is normal-sized and solid, every button works, HOME
input is suppressed only while open, and sorting still works after resume.

## 7. Application launch and HOME return

1. Launch one installed game, then return to HOME Menu.
2. Open and close LumaHome; apply a sort.
3. Launch a system application other than Notifications, then return.
4. Repeat the overlay and sort check.

Pass: hooks and input work after both returns, with no flicker or missing icons.

## 8. Sleep and wake

1. Close the lid for at least 20 seconds and reopen it.
2. Open and close LumaHome.
3. Apply one sort.

Pass: display, controls, overlay, and live sorting work after wake.

## 9. HOME metadata regression

Inspect these items before and after the preceding tests:

- theme, background, and badges;
- unread/read Notifications state;
- folders and folder names;
- installed titles and their icons;
- software packages that have already been unwrapped;
- the normally empty/special HOME slot previously observed.

Pass: LumaHome changes ordering only. It does not reset a theme, mark old
notifications unread, rewrap software, create selectable blank icons, or alter
the special empty slot.

## 10. Power-off and persistent order

1. Finish in a clearly recognizable Z-A order.
2. Power off normally from HOME Menu.
3. Cold boot through `LumaHome010RC1.firm` again.
4. Confirm the Z-A order persists.
5. Confirm folder placement and contents after the cold boot.
6. Open LumaHome once more and confirm the displayed version.

Pass: power-off completes, cold boot succeeds, title order persists, folder data
is intact, and the overlay remains functional.

## 11. Evidence collection

After all tests, power off and reinsert the SD card. Do not delete logs. The
maintainer should inspect at least:

- `/3ds/LumaHome/live-map-0.1.0-rc1.txt`
- `/3ds/LumaHome/runtime.txt`
- `/3ds/Cthulhu/sort-journal.txt` (compatibility path)
- `/3ds/Cthulhu/sort-transaction.txt` (compatibility path)
- `/luma/dumps/arm11/`

Report each section as Pass, Fail, or Not tested, plus any visible symptom. A
single complete report and one final SD insertion are preferred over inserting
the card after every successful section.
