#include <3ds.h>
#include <string.h>
#include "cthulhu_home_static_patch.h"

#define CTH_HOME_TID 0x0004003000008F02ULL
#define CTH_IMAGE_BASE 0x00100000
#define CTH_ENTRY 0x00100000
#define CTH_CODE_CAVE 0x00305174
#define CTH_CAVE_LIMIT 0x00306000

extern const u8 cthulhuHomeStubStart[];
extern const u8 cthulhuHomeStubEnd[];
extern const u8 cthulhuFrameHook[];
extern const u8 cthulhuDisplayTransferHook[];
extern const u8 cthulhuKeysHeldHook[];
extern const u8 cthulhuKeysDownHook[];
extern const u8 cthulhuKeysUpHook[];
extern const u8 cthulhuInputDispatcherHook[];
extern const u8 cthulhuLayoutEventHook[];
extern const u8 cthulhuIconControllerInitHook[];
extern const u8 cthulhuIconRefreshObserveHook[];

static u32 ReadU32(const u8 *image, u32 address)
{
    u32 value;
    memcpy(&value, image + address - CTH_IMAGE_BASE, sizeof(value));
    return value;
}

static bool WriteBranch(u8 *image, u32 site, u32 target, bool link)
{
    s32 displacement = (s32)target - (s32)(site + 8);
    if ((displacement & 3) != 0 || displacement < -(1 << 25) ||
        displacement >= (1 << 25))
        return false;
    u32 branch = (link ? 0xEB000000 : 0xEA000000) |
                 (((u32)(displacement >> 2)) & 0x00FFFFFF);
    memcpy(image + site - CTH_IMAGE_BASE, &branch, sizeof(branch));
    return true;
}

bool CthulhuHomeStaticPatch_Apply(u64 titleId, u8 *image, u32 imageSize)
{
    if (titleId != CTH_HOME_TID)
        return false;

    const u32 stubSize = (u32)(cthulhuHomeStubEnd - cthulhuHomeStubStart);
    if (!image || CTH_CODE_CAVE + stubSize > CTH_CAVE_LIMIT ||
        CTH_CODE_CAVE + stubSize - CTH_IMAGE_BASE > imageSize ||
        CTH_ENTRY + 4 - CTH_IMAGE_BASE > imageSize)
        return false;

    static const struct { u32 address; u32 instruction; } signatures[] = {
        {0x00100000, 0xEB000007},
        {0x00131F30, 0xE1A00004},
        {0x00131F34, 0xEB0029D1},
        {0x0013C680, 0xE92D47F0},
        {0x002253CC, 0xE58D9004},
        {0x002253D0, 0xEBFFD9BF},
        {0x0021BAD4, 0xE92D4FF0},
        {0x00101AAC, 0xEB0001F9},
        {0x001498F8, 0xEB0008DE},
        {0x001F6BC8, 0xE92D4010},
        {0x001F6C14, 0xE92D4010},
        {0x001F6DC4, 0xE92D4010},
        {0x001039E0, 0xE92D47F0},
        {0x001BA594, 0xE3520008},
        {0x001D11A0, 0xEBFFA34B},
        {0x001CA504, 0xE92D5FF0},
    };
    for (u32 i = 0; i < sizeof(signatures) / sizeof(signatures[0]); i++)
        if (ReadU32(image, signatures[i].address) != signatures[i].instruction)
            return false;

    const u8 *cave = image + CTH_CODE_CAVE - CTH_IMAGE_BASE;
    for (u32 i = 0; i < stubSize; i++)
        if (cave[i] != 0)
            return false;

    memcpy((void *)cave, cthulhuHomeStubStart, stubSize);
    if (!WriteBranch(image, CTH_ENTRY, CTH_CODE_CAVE, true))
        return false;

    const u32 frameSite = 0x00101AAC;
    u32 target = CTH_CODE_CAVE + (u32)(cthulhuFrameHook - cthulhuHomeStubStart);
    if (!WriteBranch(image, frameSite, target, true))
        return false;

    const u32 displaySite = 0x001498F8;
    target = CTH_CODE_CAVE + (u32)(cthulhuDisplayTransferHook - cthulhuHomeStubStart);
    if (!WriteBranch(image, displaySite, target, true))
        return false;

    static const struct { u32 site; const u8 *hook; } inputHooks[] = {
        {0x001F6BC8, cthulhuKeysHeldHook},
        {0x001F6C14, cthulhuKeysDownHook},
        {0x001F6DC4, cthulhuKeysUpHook},
    };
    for (u32 i = 0; i < sizeof(inputHooks) / sizeof(inputHooks[0]); i++)
    {
        target = CTH_CODE_CAVE + (u32)(inputHooks[i].hook - cthulhuHomeStubStart);
        if (!WriteBranch(image, inputHooks[i].site, target, false))
            return false;
    }
    target = CTH_CODE_CAVE +
             (u32)(cthulhuInputDispatcherHook - cthulhuHomeStubStart);
    if (!WriteBranch(image, 0x001039E0, target, false))
        return false;
    target = CTH_CODE_CAVE +
             (u32)(cthulhuLayoutEventHook - cthulhuHomeStubStart);
    if (!WriteBranch(image, 0x001BA594, target, false))
        return false;
    target = CTH_CODE_CAVE +
             (u32)(cthulhuIconControllerInitHook - cthulhuHomeStubStart);
    if (!WriteBranch(image, 0x001D11A0, target, true))
        return false;
    target = CTH_CODE_CAVE +
             (u32)(cthulhuIconRefreshObserveHook - cthulhuHomeStubStart);
    if (!WriteBranch(image, 0x001CA504, target, false))
        return false;
    return true;
}
