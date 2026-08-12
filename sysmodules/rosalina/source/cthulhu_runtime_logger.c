#include <3ds.h>
#include "MyThread.h"
#include "ifile.h"
#include "fmt.h"
#include "csvc.h"
#define font_size cthulhu_osd_font_size
#include "font.h"
#undef font_size
#include "menus/home_menu_diagnostics.h"
#include "cthulhu_runtime_logger.h"

#define CTH_HOME_TID 0x0004003000008F02ULL
#define CTH_CHANNEL 0x003827F0
#define CTH_CHANNEL_LOCAL 0x00100000
#define CTH_PANEL_LOCAL 0x00200000
#define CTH_PANEL_SIZE 0x0001C200
#define CTH_MARKER_V167 0x43545337
#define CTH_CHORD (KEY_L | KEY_Y)
#define CTH_CODE_LOCAL 0x00400000
#define CTH_FRAME_SITE 0x00101AAC
#define CTH_CAVE_PAGE 0x00305000
#define CTH_CAVE_ENTRY 0x00305174
#define CTH_FRAME_HOOK 0x0030537C
#define CTH_WATCH_CHANNEL_LOCAL 0x00500000
#define CTH_WATCH_CODE_LOCAL 0x00600000
#define CTH_RECOVERY_CHANNEL_LOCAL 0x00700000
#define CTH_RECOVERY_PANEL_LOCAL 0x00800000

enum { CTH_IDLE, CTH_APPLYING, CTH_SUCCESS, CTH_FAILED };

static MyThread runtimeThread;
static u8 CTR_ALIGN(0x1000) runtimeStack[0x2000];
static MyThread watchdogThread;
static u8 CTR_ALIGN(0x1000) watchdogStack[0x2000];
static MyThread inputProbeThread;
static u8 CTR_ALIGN(0x1000) inputProbeStack[0x1000];
static u32 watchdogHomeCount;
static u32 watchdogHomePids[4];
static u32 watchdogHomeMarkers[4];
static u32 watchdogHomeHeartbeats[4];
static volatile u32 probeHeld;
static volatile u32 probeLastNonzero;
static volatile u32 probeChordEdges;
static volatile u32 probeTransitions;
static volatile u32 probeIndex;
static volatile u32 watchdogActiveHomePid;
static volatile u32 primaryRuntimePid;
static volatile u32 runtimeAttachedPid;
static volatile u32 recoveryAttempts;
static volatile u32 recoverySuccesses;
static volatile Result recoveryLastResult;
static volatile u32 recoveryLastPid;
static volatile u32 recoveryOverlayBefore;
static volatile u32 recoveryOverlayAfter;
static volatile Result recoveryRenderResult;
static volatile u32 recoveryRenderCount;

static void renderMenu(u8 *panel, u32 selection, bool reverse,
                       bool foldersFirst, u32 state,
                       Result result, u32 mutations);

static u32 readLiveHidHeld(u32 *indexOut)
{
    volatile u8 *hid = (volatile u8 *)hidSharedMem;
    if (hid == NULL) return 0;
    u32 index = *(volatile u32 *)(hid + 0x10) & 7;
    if (indexOut != NULL) *indexOut = index;
    return *(volatile u32 *)(hid + 0x28 + index * 0x10);
}

static Result openHomePid(u32 pid, Handle *out)
{
    Handle process = 0;
    Result res = svcOpenProcess(&process, pid);
    if (R_FAILED(res)) return res;
    u64 tid = 0;
    res = svcGetProcessInfo((s64 *)&tid, process, 0x10001);
    if (R_FAILED(res) || tid != CTH_HOME_TID)
    {
        svcCloseHandle(process);
        return R_FAILED(res) ? res : (Result)-1;
    }
    *out = process;
    return 0;
}

static Result recoveryToggleActiveHome(u32 pid)
{
    Handle process = 0;
    Result res = svcOpenProcess(&process, pid);
    if (R_FAILED(res)) return res;
    u32 page = CTH_CHANNEL & ~0xFFF;
    res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE,
        CTH_RECOVERY_CHANNEL_LOCAL, process, page, 0x1000, 0);
    if (R_SUCCEEDED(res))
    {
        volatile u32 *v = (volatile u32 *)(CTH_RECOVERY_CHANNEL_LOCAL +
                                           (CTH_CHANNEL & 0xFFF));
        if (v[0] != CTH_MARKER_V167 || v[12] == 0)
            res = (Result)-2;
        else
        {
            recoveryOverlayBefore = v[8];
            recoveryRenderResult = 0;
            if (v[8] == 0)
            {
                u32 panelAddress = v[12];
                recoveryRenderResult = svcMapProcessMemoryEx(
                    CUR_PROCESS_HANDLE, CTH_RECOVERY_PANEL_LOCAL, process,
                    panelAddress, 0x20000, 0);
                if (R_SUCCEEDED(recoveryRenderResult))
                {
                    renderMenu((u8 *)CTH_RECOVERY_PANEL_LOCAL, 0, false, true,
                               CTH_IDLE, 0, 0);
                    svcFlushProcessDataCache(CUR_PROCESS_HANDLE,
                        CTH_RECOVERY_PANEL_LOCAL, CTH_PANEL_SIZE);
                    svcFlushProcessDataCache(process, panelAddress,
                        CTH_PANEL_SIZE);
                    recoveryRenderCount++;
                    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                        CTH_RECOVERY_PANEL_LOCAL, 0x20000);
                }
            }
            if (R_FAILED(recoveryRenderResult))
            {
                res = recoveryRenderResult;
            }
            else
            {
            v[8] ^= 1;
            recoveryOverlayAfter = v[8];
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE,
                                     CTH_RECOVERY_CHANNEL_LOCAL, 0x1000);
            svcFlushProcessDataCache(process, page, 0x1000);
            }
        }
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                                CTH_RECOVERY_CHANNEL_LOCAL, 0x1000);
    }
    svcCloseHandle(process);
    return res;
}

static void inputProbeMain(void)
{
    u32 previous = 0;
    svcSleepThread(5 * 1000 * 1000 * 1000LL);
    for (;;)
    {
        if (hidSharedMem != NULL)
        {
            u32 index = 0;
            u32 held = readLiveHidHeld(&index);
            probeIndex = index;
            probeHeld = held;
            if (held != 0) probeLastNonzero = held;
            if (held != previous) probeTransitions++;
            if ((held & CTH_CHORD) == CTH_CHORD &&
                (previous & CTH_CHORD) != CTH_CHORD)
            {
                probeChordEdges++;
                u32 activePid = watchdogActiveHomePid;
                if (activePid != 0 && activePid != runtimeAttachedPid)
                {
                    recoveryAttempts++;
                    recoveryLastPid = activePid;
                    recoveryLastResult = recoveryToggleActiveHome(activePid);
                    if (R_SUCCEEDED(recoveryLastResult)) recoverySuccesses++;
                }
            }
            previous = held;
        }
        svcSleepThread(16 * 1000 * 1000LL);
    }
}

static Result openHome(Handle *out, u32 *pidOut);

static u32 expectedBranch(u32 site, u32 target, bool link)
{
    s32 displacement = (s32)target - (s32)(site + 8);
    return (link ? 0xEB000000 : 0xEA000000) |
           (((u32)(displacement >> 2)) & 0x00FFFFFF);
}

static void writeLifecycle(u32 pid, const char *state, Result result,
                           u32 marker, u32 heartbeat, u32 panel,
                           u32 frameWord, u32 stubWord, u32 recoveries)
{
    char report[768];
    int n = sprintf(report,
        "Cthulhu HOME hook lifecycle log\n"
        "runtime_version=1.8.4\nmode=active-home-controller-v184\n"
        "pid=%lu\nstate=%s\nresult=%08lx\nmarker=%08lx\n"
        "heartbeat=%lu\npanel=%08lx\nframe_hook=%08lx\n"
        "expected_frame_hook=%08lx\nstub=%08lx\nrecoveries=%lu\n",
        pid, state, result, marker, heartbeat, panel, frameWord,
        expectedBranch(CTH_FRAME_SITE, CTH_FRAME_HOOK, true),
        stubWord, recoveries);
    IFile file;
    Result open = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/hook-lifecycle-v167.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(open))
    {
        u64 written = 0;
        IFile_Write(&file, &written, report, (u32)n, 0);
        IFile_SetSize(&file, (u64)n);
        IFile_Flush(&file);
        IFile_Close(&file);
    }
}

static bool inspectStaticHook(Handle process, u32 *frameWord, u32 *stubWord)
{
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CODE_LOCAL,
        process, CTH_FRAME_SITE & ~0xFFF, 0x1000, 0);
    if (R_FAILED(res)) return false;
    *frameWord = *(volatile u32 *)(CTH_CODE_LOCAL + (CTH_FRAME_SITE & 0xFFF));
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CODE_LOCAL, 0x1000);

    res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CODE_LOCAL,
        process, CTH_CAVE_PAGE, 0x1000, 0);
    if (R_FAILED(res)) return false;
    *stubWord = *(volatile u32 *)(CTH_CODE_LOCAL +
                                  (CTH_CAVE_ENTRY - CTH_CAVE_PAGE));
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CODE_LOCAL, 0x1000);
    return true;
}

static Result watchdogReadCode(Handle process, u32 remoteAddress, u32 *value)
{
    u32 page = remoteAddress & ~0xFFF;
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE,
        CTH_WATCH_CODE_LOCAL, process, page, 0x1000, 0);
    if (R_FAILED(res)) return res;
    *value = *(volatile u32 *)(CTH_WATCH_CODE_LOCAL +
                               (remoteAddress & 0xFFF));
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_WATCH_CODE_LOCAL, 0x1000);
    return 0;
}

static void watchdogEnumerateHomes(void)
{
    u32 pids[0x40];
    s32 count = 0;
    watchdogHomeCount = 0;
    memset(watchdogHomePids, 0, sizeof(watchdogHomePids));
    memset(watchdogHomeMarkers, 0, sizeof(watchdogHomeMarkers));
    memset(watchdogHomeHeartbeats, 0, sizeof(watchdogHomeHeartbeats));
    if (R_FAILED(svcGetProcessList(&count, pids, 0x40))) return;
    for (s32 i = 0; i < count; i++)
    {
        Handle process = 0;
        if (R_FAILED(svcOpenProcess(&process, pids[i]))) continue;
        u64 tid = 0;
        Result res = svcGetProcessInfo((s64 *)&tid, process, 0x10001);
        if (R_SUCCEEDED(res) && tid == CTH_HOME_TID)
        {
            u32 slot = watchdogHomeCount++;
            if (slot < 4)
            {
                watchdogHomePids[slot] = pids[i];
                u32 page = CTH_CHANNEL & ~0xFFF;
                res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE,
                    CTH_WATCH_CHANNEL_LOCAL, process, page, 0x1000, 0);
                if (R_SUCCEEDED(res))
                {
                    volatile u32 *v = (volatile u32 *)(CTH_WATCH_CHANNEL_LOCAL +
                                                       (CTH_CHANNEL & 0xFFF));
                    watchdogHomeMarkers[slot] = v[0];
                    watchdogHomeHeartbeats[slot] = v[2];
                    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                                            CTH_WATCH_CHANNEL_LOCAL, 0x1000);
                }
            }
        }
        svcCloseHandle(process);
    }
}

static void writeWatchdog(u32 pid, Result result, u32 marker, u32 heartbeat,
                          u32 previousHeartbeat, u32 stallSamples,
                          u32 frameWord, u32 stubWord, u32 pidChanges,
                          u32 samples)
{
    char report[1400];
    const u32 expected = expectedBranch(CTH_FRAME_SITE, CTH_FRAME_HOOK, true);
    const char *state = R_FAILED(result) ? "read-error" :
        frameWord != expected || stubWord != 0xE92D500F ? "hook-invalid" :
        marker != CTH_MARKER_V167 ? "marker-invalid" :
        stallSamples >= 3 ? "heartbeat-stalled" : "healthy";
    int n = sprintf(report,
        "Cthulhu independent HOME watchdog\n"
        "watchdog_version=1.8.4\nbase_overlay=V167\n"
        "state=%s\nresult=%08lx\n"
        "pid=%lu\npid_changes=%lu\nsamples=%lu\n"
        "marker=%08lx\nheartbeat=%lu\nprevious_heartbeat=%lu\n"
        "stall_samples=%lu\nframe_hook=%08lx\nexpected_frame_hook=%08lx\n"
        "stub=%08lx\nactive_home_pid=%lu\n"
        "probe_index=%lu\nprobe_held=%08lx\nprobe_last_nonzero=%08lx\n"
        "probe_transitions=%lu\nprobe_chord_edges=%lu\n",
        state, result, pid, pidChanges, samples, marker, heartbeat,
        previousHeartbeat, stallSamples, frameWord, expected, stubWord,
        watchdogActiveHomePid, probeIndex, probeHeld, probeLastNonzero,
        probeTransitions, probeChordEdges);
    n += sprintf(report + n,
        "primary_runtime_pid=%lu\nruntime_attached_pid=%lu\nrecovery_attempts=%lu\n"
        "recovery_successes=%lu\nrecovery_last_result=%08lx\n"
        "recovery_last_pid=%lu\nrecovery_overlay_before=%lu\n"
        "recovery_overlay_after=%lu\nrecovery_render_result=%08lx\n"
        "recovery_render_count=%lu\n",
        primaryRuntimePid, runtimeAttachedPid, recoveryAttempts, recoverySuccesses,
        recoveryLastResult, recoveryLastPid, recoveryOverlayBefore,
        recoveryOverlayAfter, recoveryRenderResult, recoveryRenderCount);
    n += sprintf(report + n, "home_count=%lu\n", watchdogHomeCount);
    for (u32 i = 0; i < watchdogHomeCount && i < 4; i++)
        n += sprintf(report + n, "home%lu_pid=%lu\nhome%lu_marker=%08lx\n"
                     "home%lu_heartbeat=%lu\n", i, watchdogHomePids[i], i,
                     watchdogHomeMarkers[i], i, watchdogHomeHeartbeats[i]);
    IFile file;
    Result open = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/watchdog-v184.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(open))
    {
        u64 written = 0;
        IFile_Write(&file, &written, report, (u32)n, 0);
        IFile_SetSize(&file, (u64)n);
        IFile_Flush(&file);
        IFile_Close(&file);
    }
}

static void watchdogMain(void)
{
    u32 lastPid = 0, lastHeartbeat = 0, stallSamples = 0;
    u32 pidChanges = 0, samples = 0;
    svcSleepThread(5 * 1000 * 1000 * 1000LL);
    for (;;)
    {
        Handle process = 0;
        u32 pid = 0, marker = 0, heartbeat = 0, frameWord = 0, stubWord = 0;
        Result res = openHome(&process, &pid);
        if (R_SUCCEEDED(res))
        {
            if (lastPid != 0 && pid != lastPid) pidChanges++;
            u32 remotePage = CTH_CHANNEL & ~0xFFF;
            res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE,
                CTH_WATCH_CHANNEL_LOCAL, process, remotePage, 0x1000, 0);
            if (R_SUCCEEDED(res))
            {
                volatile u32 *v = (volatile u32 *)(CTH_WATCH_CHANNEL_LOCAL +
                                                   (CTH_CHANNEL & 0xFFF));
                marker = v[0];
                heartbeat = v[2];
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                                        CTH_WATCH_CHANNEL_LOCAL, 0x1000);
                res = watchdogReadCode(process, CTH_FRAME_SITE, &frameWord);
                if (R_SUCCEEDED(res))
                    res = watchdogReadCode(process, CTH_CAVE_ENTRY, &stubWord);
            }
            svcCloseHandle(process);
        }
        if (pid == lastPid && heartbeat == lastHeartbeat) stallSamples++;
        else stallSamples = 0;
        samples++;
        static u32 priorPids[4], priorHeartbeats[4];
        watchdogEnumerateHomes();
        for (u32 i = 0; i < watchdogHomeCount && i < 4; i++)
        {
            for (u32 j = 0; j < 4; j++)
                if (priorPids[j] == watchdogHomePids[i] &&
                    watchdogHomeHeartbeats[i] != priorHeartbeats[j])
                    watchdogActiveHomePid = watchdogHomePids[i];
        }
        memcpy(priorPids, watchdogHomePids, sizeof(priorPids));
        memcpy(priorHeartbeats, watchdogHomeHeartbeats,
               sizeof(priorHeartbeats));
        writeWatchdog(pid, res, marker, heartbeat, lastHeartbeat, stallSamples,
                      frameWord, stubWord, pidChanges, samples);
        lastPid = pid;
        lastHeartbeat = heartbeat;
        svcSleepThread(1000 * 1000 * 1000LL);
    }
}

static Result openHome(Handle *out, u32 *pidOut)
{
    u32 pids[0x40];
    s32 count = 0;
    Result res = svcGetProcessList(&count, pids, 0x40);
    if (R_FAILED(res)) return res;
    for (s32 i = 0; i < count; i++)
    {
        Handle process = 0;
        if (R_FAILED(svcOpenProcess(&process, pids[i]))) continue;
        u64 tid = 0;
        res = svcGetProcessInfo((s64 *)&tid, process, 0x10001);
        if (R_SUCCEEDED(res) && tid == CTH_HOME_TID)
        {
            *out = process;
            *pidOut = pids[i];
            return 0;
        }
        svcCloseHandle(process);
    }
    return (Result)-1;
}

static void putPixel(u8 *panel, u32 x, u32 y, u8 r, u8 g, u8 b)
{
    if (x >= 160 || y >= 240) return;
    u8 *p = panel + x * 720 + (239 - y) * 3;
    p[0] = b;
    p[1] = g;
    p[2] = r;
}

static void drawChar(u8 *panel, u32 x, u32 y, char c, bool selected)
{
    if ((u8)c >= 128) c = '?';
    for (u32 row = 0; row < FONT_HEIGHT; row++)
    {
        u8 bits = font[(u8)c * FONT_HEIGHT + row];
        for (s32 col = 6; col >= 1; col--)
        {
            bool on = ((bits >> col) & 1) != 0;
            if (on)
                putPixel(panel, x + (5 - col), y + row,
                         selected ? 0x20 : 0xFF,
                         selected ? 0xE0 : 0xFF, 0xFF);
        }
    }
}

static void drawText(u8 *panel, u32 x, u32 y, const char *text, bool selected)
{
    u32 column = 0;
    for (u32 i = 0; text[i] != 0; i++)
    {
        if (text[i] == '\n') { y += 13; column = 0; continue; }
        drawChar(panel, x + column * 6, y, text[i], selected);
        column++;
        if (x + column * 6 + 6 >= 160) { y += 13; column = 0; }
    }
}

static void renderMenu(u8 *panel, u32 selection, bool reverse,
                       bool foldersFirst, u32 state,
                       Result result, u32 mutations)
{
    memset(panel, 0x20, CTH_PANEL_SIZE);
    drawText(panel, 5, 8, "CTHULHU SORT V167", false);
    drawText(panel, 5, 30, selection == 0 ? "> DIRECTION" : "  DIRECTION", selection == 0);
    drawText(panel, 5, 43, reverse ? "  Z-A" : "  A-Z", selection == 0);
    drawText(panel, 5, 64, selection == 1 ? "> FOLDER PLACEMENT" : "  FOLDER PLACEMENT", selection == 1);
    drawText(panel, 5, 77, foldersFirst ? "  BEFORE TITLES" : "  AFTER TITLES", selection == 1);
    drawText(panel, 5, 104, "UP/DOWN FIELD", false);
    drawText(panel, 5, 117, "LEFT/RIGHT CHANGE", false);
    drawText(panel, 5, 130, "A APPLY  L+Y CLOSE", false);
    if (state == CTH_APPLYING)
        drawText(panel, 5, 158, "APPLYING...", false);
    else if (state == CTH_SUCCESS)
    {
        char line[64];
        sprintf(line, "READY %lu", mutations);
        drawText(panel, 5, 158, line, false);
        drawText(panel, 5, 171, "POWER OFF", false);
        drawText(panel, 5, 184, "TO FINISH", false);
    }
    else if (state == CTH_FAILED)
    {
        char line[64];
        sprintf(line, "FAILED %08lx", result);
        drawText(panel, 5, 158, line, false);
        drawText(panel, 5, 171, "SEE SORT LOG", false);
    }
}

static void flushShared(Handle process, u32 panelAddress)
{
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, CTH_PANEL_LOCAL, CTH_PANEL_SIZE);
    svcFlushProcessDataCache(process, panelAddress, CTH_PANEL_SIZE);
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, CTH_CHANNEL_LOCAL, 0x1000);
    svcFlushProcessDataCache(process, CTH_CHANNEL & ~0xFFF, 0x1000);
}

static void writeSnapshot(u32 pid, volatile u32 *v, u32 held, u32 pressed,
                          u32 selection, bool reverse, bool foldersFirst,
                          u32 state, Result result,
                          u32 mutations)
{
    char report[1800];
    int n = sprintf(report,
        "Cthulhu HOME OSD automatic runtime log\n"
        "runtime_version=1.8.4\nmode=active-home-controller-v184\n"
        "pid=%lu\nmarker=%08lx\nmarker_ok=%u\n"
        "heartbeat=%lu\noverlay=%lu\nheld=%08lx\npressed=%08lx\n"
        "selection=%lu\ndirection=%s\nfolder_placement=%s\n"
        "state=%lu\nsort_result=%08lx\nmutations=%lu\n"
        "panel=%08lx\nallocation=%08lx\nrender_count=%lu\n"
        "display_hooks=%lu\ndispatcher_calls=%lu\ndispatcher_suppressed=%lu\n"
        "last_display_destination=%08lx\nlast_output_dimensions=%08lx\n"
        "top_transfer_count=%lu\ndma_result=%08lx\n"
        "top_fb0_left=%08lx\ntop_fb0_right=%08lx\n"
        "top_fb1_left=%08lx\ntop_fb1_right=%08lx\n",
        pid, v[0], v[0] == CTH_MARKER_V167, v[2], v[8], held, pressed,
        selection, reverse ? "Z-A" : "A-Z",
        foldersFirst ? "before" : "after",
        state, result, mutations, v[12], v[13], v[11], v[33],
        v[34], v[35], v[44], v[45], v[46], v[9],
        v[47], v[48], v[49], v[50]);
    n += sprintf(report + n,
        "rebuild_request=%lu\nrebuild_owner=%08lx\nrebuild_ack=%lu\n"
        "rebuild_calls=%lu\nrebuild_result=%08lx\npublish_owner=%08lx\n"
        "layout_object=%08lx\nlayout_event=%lu\nlayout_page=%lu\n"
        "layout_refresh_result=%08lx\nlayout_refresh_calls=%lu\n",
        v[0xCC / 4], v[0xD0 / 4], v[0xD4 / 4],
        v[0xD8 / 4], v[0xDC / 4], v[0xE0 / 4],
        v[0xE4 / 4], v[0xE8 / 4], v[0xEC / 4],
        v[0xF0 / 4], v[0xF4 / 4]);
    n += sprintf(report + n,
        "icon_refresh_owner=%08lx\nicon_refresh_calls=%lu\n"
        "icon_refresh_requested_owner=%08lx\nicon_refresh_completed=%lu\n",
        v[0xF8 / 4], v[0xFC / 4], v[0x100 / 4], v[0x104 / 4]);
    IFile file;
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/framework-live-v167.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
    {
        u64 written = 0;
        IFile_Write(&file, &written, report, (u32)n, 0);
        IFile_SetSize(&file, (u64)n);
        IFile_Flush(&file);
        IFile_Close(&file);
    }
}

static void runtimeMain(void)
{
    Result hidResult = hidInit();
    if (R_FAILED(hidResult)) return;
    svcSleepThread(4 * 1000 * 1000 * 1000LL);

    u32 lastPid = 0, recoveries = 0;
    for (;;)
    {
        Handle process = 0;
        u32 pid = 0;
        u32 preferredPid = watchdogActiveHomePid;
        Result openResult = preferredPid != 0 ?
            openHomePid(preferredPid, &process) : (Result)-1;
        if (R_SUCCEEDED(openResult)) pid = preferredPid;
        else openResult = openHome(&process, &pid);
        if (R_FAILED(openResult))
        {
            svcSleepThread(1000 * 1000 * 1000LL);
            continue;
        }
        if (primaryRuntimePid == 0) primaryRuntimePid = pid;

        u32 remotePage = CTH_CHANNEL & ~0xFFF;
        Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CHANNEL_LOCAL,
                                           process, remotePage, 0x1000, 0);
        if (R_FAILED(res)) { svcCloseHandle(process); continue; }
        volatile u32 *channel = (volatile u32 *)(CTH_CHANNEL_LOCAL +
                                                 (CTH_CHANNEL & 0xFFF));
        u32 frameWord = 0, stubWord = 0;
        bool hookValid = inspectStaticHook(process, &frameWord, &stubWord) &&
            frameWord == expectedBranch(CTH_FRAME_SITE, CTH_FRAME_HOOK, true) &&
            stubWord == 0xE92D500F;
        if (pid != lastPid)
        {
            writeLifecycle(pid, hookValid ? "new-home-hook-valid" :
                           "new-home-hook-invalid", 0, channel[0], channel[2],
                           channel[12], frameWord, stubWord, recoveries);
            lastPid = pid;
        }
        if (channel[0] != CTH_MARKER_V167 && hookValid && channel[12] != 0)
        {
            channel[0] = CTH_MARKER_V167;
            channel[1] = 0x00010000;
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE, CTH_CHANNEL_LOCAL, 0x1000);
            svcFlushProcessDataCache(process, remotePage, 0x1000);
            recoveries++;
            writeLifecycle(pid, "channel-republished", 0, channel[0], channel[2],
                           channel[12], frameWord, stubWord, recoveries);
        }
        if (channel[0] != CTH_MARKER_V167 || channel[12] == 0)
        {
            writeLifecycle(pid, hookValid ? "waiting-for-channel" :
                           "static-hook-invalid", 0, channel[0], channel[2],
                           channel[12], frameWord, stubWord, recoveries);
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CHANNEL_LOCAL, 0x1000);
            svcCloseHandle(process);
            svcSleepThread(500 * 1000 * 1000LL);
            continue;
        }

        u32 panelAddress = channel[12];
        res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_PANEL_LOCAL,
                                    process, panelAddress, 0x20000, 0);
        if (R_FAILED(res))
        {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CHANNEL_LOCAL, 0x1000);
            svcCloseHandle(process);
            continue;
        }

        u8 *panel = (u8 *)CTH_PANEL_LOCAL;
        runtimeAttachedPid = pid;
        CthulhuHomeMenu_WritePersistenceAudit();
        u32 previous = readLiveHidHeld(NULL), selection = 0, state = CTH_IDLE;
        bool reverse = false, foldersFirst = true;
        u32 mutations = 0, algorithm = 0;
        Result sortResult = 0;
        u32 repeatKey = 0, repeatFrames = 0, logFrames = 0;
        renderMenu(panel, selection, reverse, foldersFirst,
                   state, sortResult, mutations);
        flushShared(process, panelAddress);

        while (channel[0] == CTH_MARKER_V167)
        {
            u32 activePid = watchdogActiveHomePid;
            if (activePid != 0 && activePid != pid) break;
            u32 held = readLiveHidHeld(NULL);
            u32 pressed = held & ~previous;
            bool chordNow = (held & CTH_CHORD) == CTH_CHORD;
            bool chordBefore = (previous & CTH_CHORD) == CTH_CHORD;
            bool redraw = false;

            if (chordNow && !chordBefore)
            {
                channel[8] ^= 1;
                redraw = true;
            }

            if (channel[8] && state != CTH_APPLYING)
            {
                u32 nav = held & (KEY_UP | KEY_DOWN);
                bool repeat = false;
                if (nav == 0) { repeatKey = 0; repeatFrames = 0; }
                else if (nav != repeatKey) { repeatKey = nav; repeatFrames = 0; repeat = true; }
                else { repeatFrames++; repeat = repeatFrames == 24 ||
                        (repeatFrames > 24 && ((repeatFrames - 24) % 6) == 0); }
                if ((pressed & KEY_UP) || (repeat && (nav & KEY_UP)))
                { selection = selection == 0 ? 1 : 0; redraw = true; }
                else if ((pressed & KEY_DOWN) || (repeat && (nav & KEY_DOWN)))
                { selection = selection == 1 ? 0 : 1; redraw = true; }

                if (pressed & (KEY_LEFT | KEY_RIGHT))
                {
                    if (selection == 0) reverse = !reverse;
                    else foldersFirst = !foldersFirst;
                    state = CTH_IDLE;
                    redraw = true;
                }

                if (pressed & KEY_A)
                {
                    state = CTH_APPLYING;
                    renderMenu(panel, selection, reverse, foldersFirst,
                               state, sortResult, mutations);
                    flushShared(process, panelAddress);
                    u16 chosen = 0;
                    sortResult = CthulhuHomeMenu_RunBackgroundSort(
                        reverse ? 2 : 1, foldersFirst, channel,
                        &chosen, &mutations);
                    algorithm = chosen;
                    (void)algorithm;
                    state = R_SUCCEEDED(sortResult) ? CTH_SUCCESS : CTH_FAILED;
                    redraw = true;
                }
            }

            if (redraw)
            {
                renderMenu(panel, selection, reverse, foldersFirst,
                           state, sortResult, mutations);
                flushShared(process, panelAddress);
            }
            if (++logFrames >= 120)
            {
                writeSnapshot(pid, channel, held, pressed, selection,
                              reverse, foldersFirst, state, sortResult,
                              mutations);
                logFrames = 0;
            }
            previous = held;
            svcSleepThread(16 * 1000 * 1000LL);
        }

        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_PANEL_LOCAL, 0x20000);
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, CTH_CHANNEL_LOCAL, 0x1000);
        svcCloseHandle(process);
        runtimeAttachedPid = 0;
    }
}

MyThread *CthulhuRuntimeLogger_CreateThread(void)
{
    Result res = MyThread_Create(&runtimeThread, runtimeMain, runtimeStack,
                                 sizeof(runtimeStack), 0x20, 0);
    if (R_SUCCEEDED(res))
        MyThread_Create(&watchdogThread, watchdogMain, watchdogStack,
                        sizeof(watchdogStack), 0x21, 0);
    if (R_SUCCEEDED(res))
        MyThread_Create(&inputProbeThread, inputProbeMain, inputProbeStack,
                        sizeof(inputProbeStack), 0x22, 0);
    return R_FAILED(res) ? NULL : &runtimeThread;
}
