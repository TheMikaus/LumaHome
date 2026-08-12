#include <3ds.h>
#include <string.h>
#include "csvc.h"
#include "plugin/plgloader.h"
#include "plugin/cthulhu_postboot_plugin.h"

extern u32 g_savedGameInstr[2];
extern u8 cthulhuPostBootTrampolineStart[];
extern u8 cthulhuPostBootResumeLiteral[];
extern u8 cthulhuPostBootTrampolineEnd[];
static const char *g_lastStage = "not-started";
static const u32 CTHULHU_BOOT_READY = 0x43544832;
static const u32 CTHULHU_BOOT_FAILED = 0x4354483F;
const char *CthulhuPostBoot_LastStage(void) { return g_lastStage; }
static void RollBackPlugin(void) {
    MemoryBlock__UnmountFromProcess(); MemoryBlock__Free();
    PluginLoaderCtx.target = 0; PluginLoaderCtx.pluginIsHome = false;
    PLG__SetConfigMemoryStatus(PLG_CFG_NONE);
}

static Result RestoreProcessEntryPoint(Handle process)
{
    const u32 address = 0x00100000;
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, address, process,
                                       address, 0x1000, 0);
    if (R_FAILED(res)) return res;
    memcpy((void *)address, g_savedGameInstr, sizeof(g_savedGameInstr));
    svcFlushProcessDataCache(process, address, sizeof(g_savedGameInstr));
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, address, 0x1000);
    svcInvalidateEntireInstructionCache();
    return 0;
}

Result CthulhuPostBoot_LoadPlugin(Handle process, u32 pid, u32 *threadIdOut)
{
    g_lastStage = "validating-arguments";
    if (!process || !pid || !threadIdOut)
        return MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_LDR, RD_INVALID_ADDRESS);
    if (PLG__GetConfigMemoryStatus() != PLG_CFG_NONE || PluginLoaderCtx.target)
        return MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, RM_LDR, RD_ALREADY_INITIALIZED);
    g_lastStage = "loading-and-mapping-3gx";
    PluginLoaderCtx.target = process;
    PluginLoaderCtx.pluginIsHome = true;
    if (!TryToLoadPlugin(process, false)) {
        Result loadResult = PluginLoaderCtx.error.code;
        if (PluginLoaderCtx.error.message) g_lastStage = PluginLoaderCtx.error.message;
        PluginLoaderCtx.target = 0;
        PluginLoaderCtx.pluginIsHome = false;
        return R_FAILED(loadResult) ? loadResult :
            MAKERESULT(RL_PERMANENT, RS_NOTFOUND, RM_LDR, RD_NOT_FOUND);
    }
    PluginLoaderCtx.eventsSelfManaged = true;
    g_lastStage = "restoring-entry-point";
    Result res = RestoreProcessEntryPoint(process);
    if (R_FAILED(res)) { RollBackPlugin(); return res; }
    g_lastStage = "attaching-debugger";
    Handle debug = 0;
    res = svcDebugActiveProcess(&debug, pid);
    if (R_FAILED(res)) { RollBackPlugin(); return res; }
    g_lastStage = "selecting-home-thread";
    u32 selectedThread = 0;
    for (;;) {
        Result wait = svcWaitSynchronization(debug, 0);
        if (R_FAILED(wait)) break;
        DebugEventInfo info;
        res = svcGetProcessDebugEvent(&info, debug);
        if (R_FAILED(res)) break;
        if (info.type == DBGEVENT_ATTACH_THREAD && selectedThread == 0) {
            ThreadContext context;
            if (R_SUCCEEDED(svcGetDebugThreadContext(&context, debug, info.thread_id,
                                                     THREADCONTEXT_CONTROL_ALL)) &&
                context.cpu_registers.pc >= 0x00100000 &&
                context.cpu_registers.pc < 0x00400000)
                selectedThread = info.thread_id;
        }
    }
    if (selectedThread == 0) {
        svcContinueDebugEvent(debug, 0);
        svcCloseHandle(debug);
        RollBackPlugin();
        return MAKERESULT(RL_PERMANENT, RS_NOTFOUND, RM_LDR, RD_NOT_FOUND);
    }
    g_lastStage = "redirecting-thread-context";
    ThreadContext context;
    res = svcGetDebugThreadContext(&context, debug, selectedThread,
                                   THREADCONTEXT_CONTROL_ALL);
    if (R_SUCCEEDED(res)) {
        PluginHeader *header = MemoryBlock__GetMappedPluginHeader();
        u32 trampolineSize = cthulhuPostBootTrampolineEnd - cthulhuPostBootTrampolineStart;
        u32 resumeOffset = cthulhuPostBootResumeLiteral - cthulhuPostBootTrampolineStart;
        if (!header || trampolineSize > sizeof(header->reserved) ||
            resumeOffset + sizeof(u32) > trampolineSize)
            res = MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, RM_LDR, RD_TOO_LARGE);
        else {
            memcpy(header->reserved, cthulhuPostBootTrampolineStart, trampolineSize);
            *(u32 *)((u8 *)header->reserved + resumeOffset) = context.cpu_registers.pc;
            u32 trampolineVa = 0x07000000 + ((u8 *)header->reserved - (u8 *)header);
            svcFlushEntireDataCache();
            svcFlushProcessDataCache(process, trampolineVa, trampolineSize);
            context.cpu_registers.pc = trampolineVa;
        }
        context.cpu_registers.cpsr &= ~0x20;
        if (R_SUCCEEDED(res))
            res = svcSetDebugThreadContext(debug, selectedThread, &context,
                                           THREADCONTEXT_CONTROL_ALL);
    }
    g_lastStage = "resuming-home-menu";
    Result continueResult = svcContinueDebugEvent(debug, 0);
    svcCloseHandle(debug);
    if (R_SUCCEEDED(res)) res = continueResult;
    if (R_SUCCEEDED(res)) {
        *threadIdOut = selectedThread;
        PLG__SetConfigMemoryStatus(PLG_CFG_RUNNING);
        g_lastStage = "waiting-for-plugin-handshake";
        volatile u32 *handshake = (volatile u32 *)PA_FROM_VA_PTR(
            &MemoryBlock__GetMappedPluginHeader()->config[31]);
        u32 value = 0;
        for (u32 i = 0; i < 1000; i++) {
            value = *handshake;
            if (value == CTHULHU_BOOT_READY || value == CTHULHU_BOOT_FAILED) break;
            svcSleepThread(10 * 1000 * 1000LL);
        }
        if (value == CTHULHU_BOOT_READY) g_lastStage = "complete-framework-confirmed";
        else if (value == CTHULHU_BOOT_FAILED) {
            g_lastStage = "plugin-reported-hook-failure";
            res = MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, RM_LDR, RD_INVALID_ADDRESS);
        } else {
            g_lastStage = "plugin-handshake-timeout";
            res = MAKERESULT(RL_TEMPORARY, RS_WOULDBLOCK, RM_LDR, RD_TIMEOUT);
        }
    }
    return res;
}
