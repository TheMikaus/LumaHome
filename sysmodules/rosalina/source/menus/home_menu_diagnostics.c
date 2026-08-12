#include <3ds.h>
#include <string.h>
#include "csvc.h"
#include "draw.h"
#include "fmt.h"
#include "ifile.h"
#include "menu.h"
#include "process_patches.h"
#include "home_menu_diagnostics.h"
#include "plugin/cthulhu_postboot_plugin.h"

#define CTH_LAYOUT_SLOTS 360
#define CTH_LAUNCHER_SIZE 0x2490
#define CTH_SD_LAYOUT_SIZE 0x2DA0
#define CTH_USA_HOME_MENU_TITLE_ID 0x0004003000008F02ULL
#define CTH_NAND_RAW_ADDRESS 0x3469F208
#define CTH_SD_RAW_ADDRESS 0x346CA1E0
#define CTH_SD_PROCESSED_ADDRESS 0x346CCF90
#define CTH_LAUNCHER_TO_SD_DELTA (CTH_SD_RAW_ADDRESS - CTH_NAND_RAW_ADDRESS)
#define CTH_REQUEST_MAGIC 0x53544843
#define CTH_REQUEST_VERSION 3
#define CTH_SORT_BUILD_VERSION "1.0.0-independent-folder-placement"
#define CTH_FRAMEWORK_BUILD_VERSION "0.7.7-multi-home"
#define CTH_FOLDER_POSITION_OFFSET 0x11DC
#define CTH_FOLDER_NAME_OFFSET 0x1560
#define CTH_FOLDER_NUMBER_OFFSET 0x1D58
#define CTH_FOLDER_COUNT 60
#define CTH_FOLDER_SLOTS 60
#define CTH_PROCESSED_ENTRIES (CTH_LAYOUT_SLOTS + CTH_FOLDER_COUNT * CTH_FOLDER_SLOTS)

#pragma pack(push, 1)
typedef struct
{
    u32 magic;
    u16 version;
    u16 algorithm;
    u32 entryCount;
    u32 payloadSize;
    u32 payloadCrc32;
} CthRequestHeader;

typedef struct
{
    u64 titleId;
    u8 mediaType;
    u8 flags;
    u16 reserved;
    u16 title[64];
} CthRequestEntry;
#pragma pack(pop)

typedef struct
{
    u16 slot;
    s16 oldPosition;
    s16 newPosition;
    s8 folder;
} CthSdMutation;

typedef struct
{
    u8 id;
    u32 number;
    s16 oldPosition;
    s16 newPosition;
} CthFolderMutation;

#pragma pack(push, 1)
typedef struct
{
    u32 magic;
    u16 version;
    u16 count;
    u16 algorithm;
    u16 reserved;
    u32 launcherAddress;
    CthFolderMutation folders[CTH_FOLDER_COUNT];
} CthFolderPlan;
#pragma pack(pop)

#define CTH_FOLDER_PLAN_MAGIC 0x50464843
#define CTH_FOLDER_PLAN_VERSION 2

typedef struct
{
    u32 address;
    u16 usedTitles;
    u16 applicationTitles;
    u16 systemTitles;
    u8 version;
    u8 media;
    Result dumpResult;
} CthLayoutCandidate;

static void ShowProgress(const char *stage, u32 current, u32 total)
{
    extern bool g_cthulhuBackgroundSort;
    if (g_cthulhuBackgroundSort)
        return;
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_DrawString(10, 10, COLOR_TITLE, "Cthulhu HOME Menu diagnostic");
    Draw_DrawString(10, 35, COLOR_WHITE, stage);
    if (total != 0)
    {
        Draw_DrawFormattedString(10, 60, COLOR_WHITE,
            "Progress: %lu / %lu (%lu%%)", current, total,
            (u32)(((u64)current * 100) / total));
    }
    else
    {
        Draw_DrawFormattedString(10, 60, COLOR_WHITE,
            "Progress: %lu", current);
    }
    Draw_DrawString(10, 90, COLOR_WHITE,
        "Read-only operation. Please wait...");
    Draw_FlushFramebuffer();
    Draw_Unlock();
}

bool g_cthulhuBackgroundSort = false;
static char g_layoutBackrefReport[12288];

static u64 ReadU64(const u8 *data, u32 offset)
{
    u64 value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static s16 ReadS16(const u8 *data, u32 offset)
{
    s16 value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static bool LooksLikeTitleId(u64 titleId)
{
    return titleId != UINT64_MAX && (u32)(titleId >> 48) == 4;
}

static bool ValidateLayout(const u8 *data, bool nand, u16 *usedTitles)
{
    const u32 positionOffset = nand ? 0xD9A : 0xCB0;
    const u32 folderOffset = nand ? 0x106A : 0xF80;
    u16 used = 0;

    if (data[0] > 0x20)
        return false;

    for (u32 slot = 0; slot < CTH_LAYOUT_SLOTS; slot++)
    {
        u64 titleId = ReadU64(data, 0x008 + slot * sizeof(u64));
        if (titleId == UINT64_MAX || titleId == 0)
            continue;
        if (!LooksLikeTitleId(titleId))
            return false;

        s16 position = ReadS16(data, positionOffset + slot * sizeof(s16));
        s8 folder = *(const s8 *)(data + folderOffset + slot);
        if (position < -1 || position >= CTH_LAYOUT_SLOTS || folder < -1 || folder > 59)
            return false;
        used++;
    }

    *usedTitles = used;
    return used >= 3;
}

static Result DumpCandidate(const u8 *data, u32 size, bool nand, u32 address)
{
    char path[96];
    sprintf(path, "/3ds/Cthulhu/candidate-%s-%08lx.bin",
            nand ? "Launcher" : "SaveData", address);
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                            fsMakePath(PATH_ASCII, path),
                            FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
    {
        u64 written;
        res = IFile_Write(&file, &written, data, size, 0);
        if (R_SUCCEEDED(res))
            res = IFile_SetSize(&file, size);
        IFile_Close(&file);
    }
    return res;
}

static int DumpProcessCode(Handle processHandle, char *report, int length)
{
    s64 textSize = 0;
    s64 rodataSize = 0;
    s64 dataSize = 0;
    s64 sourceAddress = 0;
    Result textResult = svcGetProcessInfo(&textSize, processHandle, 0x10002);
    Result rodataResult = svcGetProcessInfo(&rodataSize, processHandle, 0x10003);
    Result dataResult = svcGetProcessInfo(&dataSize, processHandle, 0x10004);
    Result addressResult = svcGetProcessInfo(&sourceAddress, processHandle, 0x10005);
    u32 totalSize = (u32)(textSize + rodataSize + dataSize);

    length += sprintf(report + length,
        "\n[HOME MENU CODE IMAGE]\n"
        "base=%08llx text=%08llx rodata=%08llx data=%08llx "
        "info=%08lx/%08lx/%08lx/%08lx\n",
        sourceAddress, textSize, rodataSize, dataSize,
        textResult, rodataResult, dataResult, addressResult);
    if (R_FAILED(textResult) || R_FAILED(rodataResult) ||
        R_FAILED(dataResult) || R_FAILED(addressResult) || totalSize == 0)
        return length;

    ShowProgress("Mapping HOME Menu code image", 0, totalSize);
    const u32 destinationAddress = 0x00100000;
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, destinationAddress,
                                       processHandle, (u32)sourceAddress,
                                       totalSize, 0);
    length += sprintf(report + length, "map=0x%08lx", res);
    if (R_SUCCEEDED(res))
    {
        IFile file = {0};
        res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                         fsMakePath(PATH_ASCII,
                                    "/3ds/Cthulhu/HomeMenu-USA-code.bin"),
                         FS_OPEN_CREATE | FS_OPEN_WRITE);
        length += sprintf(report + length, " open=0x%08lx", res);
        if (R_SUCCEEDED(res))
        {
            u64 written = 0;
            ShowProgress("Saving HOME Menu code image", 0, totalSize);
            res = IFile_Write(&file, &written, (const void *)destinationAddress,
                              totalSize, 0);
            if (R_SUCCEEDED(res))
                res = IFile_SetSize(&file, totalSize);
            IFile_Close(&file);
            length += sprintf(report + length,
                              " write=0x%08lx bytes=%08llx", res, written);
            ShowProgress("Saving HOME Menu code image", totalSize, totalSize);
        }
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, destinationAddress, totalSize);
    }
    return length + sprintf(report + length, "\n");
}

static void AnalyzeTitleTypes(const u8 *data, CthLayoutCandidate *candidate)
{
    candidate->applicationTitles = 0;
    candidate->systemTitles = 0;
    for (u32 slot = 0; slot < CTH_LAYOUT_SLOTS; slot++)
    {
        u64 titleId = ReadU64(data, 0x008 + slot * sizeof(u64));
        if (titleId == 0 || titleId == UINT64_MAX)
            continue;
        u32 category = (u32)(titleId >> 32);
        if (category == 0x00040000 || category == 0x00040002)
            candidate->applicationTitles++;
        else if ((category & 0xFFFFFF00) == 0x00040000 ||
                 (category & 0xFFFFFF00) == 0x00048000)
            candidate->systemTitles++;
    }
}

static u32 FindCandidates(const u8 *heap, u32 heapSize, bool nand,
                          u32 sourceAddress, CthLayoutCandidate *out, u32 capacity)
{
    const u32 layoutSize = nand ? CTH_LAUNCHER_SIZE : CTH_SD_LAYOUT_SIZE;
    u32 count = 0;
    if (heapSize < layoutSize)
        return 0;

    for (u32 offset = 0; offset <= heapSize - layoutSize && count < capacity; offset += 8)
    {
        if ((offset & 0xFFFFF) == 0)
            ShowProgress(nand ? "Scanning memory: Launcher.dat"
                              : "Scanning memory: SaveData.dat",
                         offset, heapSize - layoutSize);
        bool quickMatch = false;
        for (u32 slot = 0; slot < 8; slot++)
        {
            if (LooksLikeTitleId(ReadU64(heap + offset, 0x008 + slot * sizeof(u64))))
            {
                quickMatch = true;
                break;
            }
        }
        if (!quickMatch)
            continue;

        u16 usedTitles;
        if (ValidateLayout(heap + offset, nand, &usedTitles))
        {
            out[count].address = sourceAddress + offset;
            out[count].usedTitles = usedTitles;
            out[count].version = heap[offset];
            out[count].media = nand ? 0 : 1;
            AnalyzeTitleTypes(heap + offset, &out[count]);
            out[count].dumpResult = DumpCandidate(heap + offset, layoutSize, nand,
                                                  sourceAddress + offset);
            count++;
            offset += layoutSize - 8;
        }
    }
    return count;
}

static u8 g_fileProbeBuffer[CTH_SD_LAYOUT_SIZE];

static int ProbeFile(char *report, int length, FS_ArchiveID archiveId,
                     FS_Path archivePath, const char *path, u32 expectedSize)
{
    IFile file = {0};
    Result openResult = IFile_Open(&file, archiveId, archivePath,
                                   fsMakePath(PATH_ASCII, path), FS_OPEN_READ);
    length += sprintf(report + length, "file=%s open=0x%08lx", path, openResult);
    if (R_SUCCEEDED(openResult))
    {
        u64 size = 0;
        Result sizeResult = IFile_GetSize(&file, &size);
        length += sprintf(report + length, " sizeResult=0x%08lx size=%08llx",
                          sizeResult, size);
        if (R_SUCCEEDED(sizeResult) && size <= sizeof(g_fileProbeBuffer))
        {
            u64 read = 0;
            Result readResult = IFile_Read(&file, &read, g_fileProbeBuffer, (u32)size);
            length += sprintf(report + length, " read=0x%08lx bytes=%08llx",
                              readResult, read);
            if (R_SUCCEEDED(readResult) && expectedSize != 0)
            {
                u16 used = 0;
                bool valid = size >= expectedSize &&
                    ValidateLayout(g_fileProbeBuffer,
                                   expectedSize == CTH_LAUNCHER_SIZE, &used);
                length += sprintf(report + length, " layoutValid=%u titles=%u",
                                  valid, used);
            }
        }
        IFile_Close(&file);
    }
    return length + sprintf(report + length, "\n");
}

static int ProbeSavedataFiles(char *report, int length)
{
    static const u32 extdataIds[] = {0x82, 0x8F, 0x98, 0xA1, 0xA9, 0xB1};
    static const u32 systemIds[] = {0x00020082, 0x0002008F, 0x00020098,
                                    0x000200A1, 0x000200A9, 0x000200B1};

    length += sprintf(report + length, "\n[CTHULHU SD FILES]\n");
    ShowProgress("Checking Cthulhu files", 0, 3);
    FS_Path sdRoot = fsMakePath(PATH_EMPTY, "");
    length = ProbeFile(report, length, ARCHIVE_SDMC, sdRoot,
                       "/3ds/Cthulhu/sort-request.bin", 0);
    ShowProgress("Checking Cthulhu files", 1, 3);
    length = ProbeFile(report, length, ARCHIVE_SDMC, sdRoot,
                       "/3ds/Cthulhu/Cache.bak", 0);
    ShowProgress("Checking Cthulhu files", 2, 3);
    length = ProbeFile(report, length, ARCHIVE_SDMC, sdRoot,
                       "/3ds/Cthulhu/CacheD.bak", 0);
    ShowProgress("Checking Cthulhu files", 3, 3);

    length += sprintf(report + length, "\n[DIRECT NAND SYSTEM SAVEDATA]\n");
    for (u32 i = 0; i < sizeof(systemIds) / sizeof(systemIds[0]); i++)
    {
        ShowProgress("Trying NAND Launcher.dat archives", i,
                     sizeof(systemIds) / sizeof(systemIds[0]));
        u32 archiveData[2] = {MEDIATYPE_NAND, systemIds[i]};
        FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
        length += sprintf(report + length, "id=%08lx ", systemIds[i]);
        length = ProbeFile(report, length, ARCHIVE_SYSTEM_SAVEDATA, archivePath,
                           "/Launcher.dat", CTH_LAUNCHER_SIZE);
    }
    ShowProgress("Trying NAND Launcher.dat archives",
                 sizeof(systemIds) / sizeof(systemIds[0]),
                 sizeof(systemIds) / sizeof(systemIds[0]));

    length += sprintf(report + length, "\n[DIRECT HOME MENU SD EXTDATA]\n");
    for (u32 i = 0; i < sizeof(extdataIds) / sizeof(extdataIds[0]); i++)
    {
        ShowProgress("Trying SD HOME Menu extdata", i,
                     sizeof(extdataIds) / sizeof(extdataIds[0]));
        u32 archiveData[3] = {MEDIATYPE_SD, extdataIds[i], 0};
        FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
        length += sprintf(report + length, "id=%08lx ", extdataIds[i]);
        length = ProbeFile(report, length, ARCHIVE_EXTDATA, archivePath,
                           "/SaveData.dat", CTH_SD_LAYOUT_SIZE);
        length += sprintf(report + length, "id=%08lx ", extdataIds[i]);
        length = ProbeFile(report, length, ARCHIVE_EXTDATA, archivePath,
                           "/Cache.dat", 0);
    }
    ShowProgress("Trying SD HOME Menu extdata",
                 sizeof(extdataIds) / sizeof(extdataIds[0]),
                 sizeof(extdataIds) / sizeof(extdataIds[0]));
    return length;
}

static Result DumpReport(void)
{
    ShowProgress("Opening HOME Menu process", 0, 1);
    Handle processHandle;
    Result res = OpenProcessByName("menu", &processHandle);
    if (R_FAILED(res))
        return res;
    ShowProgress("Opening HOME Menu process", 1, 1);

    char report[8192];
    int length = sprintf(report,
        "Cthulhu HOME Menu comprehensive diagnostic v5\n"
        "All source access is read-only.\n\n[PROCESS]\nname=menu open=0x%08lx\n",
        res);
    length = ProbeSavedataFiles(report, length);
    length = DumpProcessCode(processHandle, report, length);

    CthLayoutCandidate candidates[16];
    u32 count = 0;
    u32 queriedAddress = 0x00100000;
    u32 mappedRegions = 0;
    u32 scannedBytes = 0;

    while (queriedAddress < 0x40000000 && count < 16)
    {
        ShowProgress("Enumerating writable memory regions",
                     queriedAddress - 0x00100000, 0x3FF00000);
        MemInfo mem;
        PageInfo page;
        res = svcQueryProcessMemory(&mem, &page, processHandle, queriedAddress);
        if (R_FAILED(res) || mem.size == 0)
            break;

        u32 nextAddress = mem.base_addr + mem.size;
        if (nextAddress <= queriedAddress)
            break;

        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            (mem.perm & MEMPERM_WRITE) && mem.size >= CTH_LAUNCHER_SIZE &&
            mem.size <= 0x04000000)
        {
            length += sprintf(report + length,
                "region base=%08lx size=%08lx perm=%08lx state=%08lx\n",
                mem.base_addr, mem.size, mem.perm, mem.state);
            res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                        processHandle, mem.base_addr, mem.size, 0);
            if (R_SUCCEEDED(res))
            {
                const u8 *mapped = (const u8 *)mem.base_addr;
                u32 nandCapacity = count < 8 ? 8 - count : 0;
                count += FindCandidates(mapped, mem.size, true, mem.base_addr,
                                        candidates + count, nandCapacity);
                count += FindCandidates(mapped, mem.size, false, mem.base_addr,
                                        candidates + count, 16 - count);
                mappedRegions++;
                scannedBytes += mem.size;
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
            }
            else
            {
                length += sprintf(report + length, " map=0x%08lx\n", res);
            }
        }
        queriedAddress = nextAddress;
    }

    ShowProgress("Writing diagnostic report", 0, 1);

    length += sprintf(report + length,
        "\n[MEMORY LAYOUT CANDIDATES]\nregions=%lu bytes=%08lx candidates=%lu\n",
        mappedRegions, scannedBytes, count);
    for (u32 i = 0; i < count; i++)
    {
        length += sprintf(report + length,
            "%s address=%08lx version=%u titles=%u apps=%u system=%u dump=0x%08lx\n",
            candidates[i].media == 0 ? "Launcher.dat" : "SaveData.dat",
            candidates[i].address, candidates[i].version, candidates[i].usedTitles,
            candidates[i].applicationTitles, candidates[i].systemTitles,
            candidates[i].dumpResult);
    }

    IFile file;
    bool fileOpen = false;
    res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                     fsMakePath(PATH_ASCII, "/3ds/Cthulhu/home-layout-report.txt"),
                     FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
    {
        fileOpen = true;
        u64 written;
        res = IFile_Write(&file, &written, report, (u32)length, 0);
        if (R_SUCCEEDED(res))
            res = IFile_SetSize(&file, (u32)length);
    }
    if (fileOpen)
        IFile_Close(&file);

    ShowProgress("Writing diagnostic report", 1, 1);

    svcCloseHandle(processHandle);
    return res;
}

void CthulhuHomeMenu_DumpLayoutReport(void)
{
    Result res = DumpReport();
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_FlushFramebuffer();
    Draw_Unlock();

    do
    {
        Draw_Lock();
        Draw_DrawString(10, 10, COLOR_TITLE, "HOME Menu layout scan");
        if (R_SUCCEEDED(res))
            Draw_DrawString(10, 30, COLOR_WHITE,
                "Read-only scan complete.\n\nReport: /3ds/Cthulhu/home-layout-report.txt");
        else
            Draw_DrawFormattedString(10, 30, COLOR_WHITE,
                "Scan failed (0x%08lx).\n\nMake sure HOME Menu is running.", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

static Result OpenUsaHomeMenu(Handle *out, u32 *pidOut)
{
    u32 pids[0x40];
    s32 count = 0;
    Result res = svcGetProcessList(&count, pids, 0x40);
    if (R_FAILED(res))
        return res;
    for (s32 i = 0; i < count; i++)
    {
        Handle process = 0;
        if (R_FAILED(svcOpenProcess(&process, pids[i])))
            continue;
        u64 titleId = 0;
        res = svcGetProcessInfo((s64 *)&titleId, process, 0x10001);
        if (R_SUCCEEDED(res) && titleId == CTH_USA_HOME_MENU_TITLE_ID)
        {
            *out = process;
            *pidOut = pids[i];
            return 0;
        }
        svcCloseHandle(process);
    }
    return (Result)-1;
}

static int CheckCodeSignatures(Handle process, char *report, int length)
{
    static const struct { u32 address; u32 expected; const char *name; } checks[] = {
        {0x00131F30, 0xE1A00004, "NAND argument"},
        {0x00131F34, 0xEB0029D1, "NAND call"},
        {0x0013C680, 0xE92D47F0, "NAND processor"},
        {0x002253CC, 0xE58D9004, "SD pre-call"},
        {0x002253D0, 0xEBFFD9BF, "SD call"},
        {0x0021BAD4, 0xE92D4FF0, "SD processor"},
    };
    s64 start = 0, text = 0, ro = 0, rw = 0;
    Result r0 = svcGetProcessInfo(&start, process, 0x10005);
    Result r1 = svcGetProcessInfo(&text, process, 0x10002);
    Result r2 = svcGetProcessInfo(&ro, process, 0x10003);
    Result r3 = svcGetProcessInfo(&rw, process, 0x10004);
    length += sprintf(report + length,
        "[CODE]\nbase=%08llx text=%08llx ro=%08llx rw=%08llx info=%08lx/%08lx/%08lx/%08lx\n",
        start, text, ro, rw, r0, r1, r2, r3);
    if (R_FAILED(r0) || R_FAILED(r1) || start != 0x00100000 || text < 0x126000)
        return length + sprintf(report + length, "map=skipped\n");
    Result res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, (u32)start, process,
                                       (u32)start, (u32)text, 0);
    length += sprintf(report + length, "map=%08lx\n", res);
    if (R_SUCCEEDED(res))
    {
        for (u32 i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
        {
            u32 actual = *(volatile u32 *)checks[i].address;
            length += sprintf(report + length,
                "%s address=%08lx expected=%08lx actual=%08lx match=%u\n",
                checks[i].name, checks[i].address, checks[i].expected, actual,
                actual == checks[i].expected);
        }
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, (u32)start, (u32)text);
    }
    return length;
}

static int CheckKnownLayout(Handle process, char *report, int length,
                            const char *name, u32 address, bool nand)
{
    MemInfo mem = {0};
    PageInfo page = {0};
    Result query = svcQueryProcessMemory(&mem, &page, process, address);
    length += sprintf(report + length,
        "%s address=%08lx query=%08lx region=%08lx+%08lx perm=%08lx state=%08lx",
        name, address, query, mem.base_addr, mem.size, mem.perm, mem.state);
    u32 size = nand ? CTH_LAUNCHER_SIZE : CTH_SD_LAYOUT_SIZE;
    if (R_FAILED(query) || address < mem.base_addr || address + size > mem.base_addr + mem.size)
        return length + sprintf(report + length, " valid=0\n");
    Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, process,
                                       mem.base_addr, mem.size, 0);
    u16 used = 0;
    bool valid = R_SUCCEEDED(map) && ValidateLayout((const u8 *)address, nand, &used);
    length += sprintf(report + length, " map=%08lx valid=%u titles=%u\n", map, valid, used);
    if (R_SUCCEEDED(map))
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
    return length;
}

static int FindLayoutReferences(Handle process, char *report, int length)
{
    u32 address = 0x00100000;
    u32 references = 0;
    length += sprintf(report + length, "\n[LAYOUT POINTER REFERENCES]\n");
    while (address < 0x40000000 && references < 64)
    {
        MemInfo mem = {0};
        PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, process, address);
        if (R_FAILED(query) || mem.size == 0)
            break;
        u32 next = mem.base_addr + mem.size;
        if (next <= address)
            break;
        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            (mem.perm & MEMPERM_WRITE) && mem.size <= 0x04000000)
        {
            Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                               process, mem.base_addr, mem.size, 0);
            if (R_SUCCEEDED(map))
            {
                const u32 *words = (const u32 *)mem.base_addr;
                for (u32 i = 0; i < mem.size / 4 && references < 64; i++)
                {
                    if (words[i] == CTH_NAND_RAW_ADDRESS || words[i] == CTH_SD_RAW_ADDRESS)
                    {
                        u32 nextWord = i + 1 < mem.size / 4 ? words[i + 1] : 0;
                        length += sprintf(report + length,
                            "ref=%08lx raw=%08lx next=%08lx media=%s\n",
                            mem.base_addr + i * 4, words[i], nextWord,
                            words[i] == CTH_NAND_RAW_ADDRESS ? "NAND" : "SD");
                        references++;
                    }
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
            }
        }
        address = next;
    }
    return length + sprintf(report + length, "references=%lu\n", references);
}

static bool BuildExpectedGrid(Handle process, u32 rawAddress, bool nand, u64 *grid,
                              u32 *occupiedOut)
{
    MemInfo mem = {0};
    PageInfo page = {0};
    u32 rawSize = nand ? CTH_LAUNCHER_SIZE : CTH_SD_LAYOUT_SIZE;
    if (R_FAILED(svcQueryProcessMemory(&mem, &page, process, rawAddress)) ||
        rawAddress + rawSize > mem.base_addr + mem.size)
        return false;
    Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, process,
                                       mem.base_addr, mem.size, 0);
    if (R_FAILED(map))
        return false;
    const u8 *raw = (const u8 *)rawAddress;
    u32 positionOffset = nand ? 0xD9A : 0xCB0;
    u32 folderOffset = nand ? 0x106A : 0xF80;
    for (u32 i = 0; i < CTH_LAYOUT_SLOTS; i++)
        grid[i] = UINT64_MAX;
    u32 occupied = 0;
    for (u32 slot = 0; slot < CTH_LAYOUT_SLOTS; slot++)
    {
        u64 titleId = ReadU64(raw, 8 + slot * 8);
        s16 position = ReadS16(raw, positionOffset + slot * 2);
        s8 folder = *(const s8 *)(raw + folderOffset + slot);
        if (LooksLikeTitleId(titleId) && folder == -1 && position >= 0 &&
            position < CTH_LAYOUT_SLOTS && grid[position] == UINT64_MAX)
        {
            grid[position] = titleId;
            occupied++;
        }
    }
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
    *occupiedOut = occupied;
    return occupied >= 3;
}

static int FindExactGrid(Handle process, char *report, int length, const char *name,
                         const u64 *expected, u32 occupied)
{
    u32 anchor = 0;
    while (anchor < CTH_LAYOUT_SLOTS && expected[anchor] == UINT64_MAX)
        anchor++;
    u32 address = 0x00100000;
    u32 matches = 0;
    length += sprintf(report + length, "%s occupied=%lu anchor=%lu", name, occupied, anchor);
    if (anchor == CTH_LAYOUT_SLOTS)
        return length + sprintf(report + length, " matches=0\n");
    while (address < 0x40000000 && matches < 8)
    {
        MemInfo mem = {0};
        PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, process, address);
        if (R_FAILED(query) || mem.size == 0)
            break;
        u32 next = mem.base_addr + mem.size;
        if (next <= address)
            break;
        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            (mem.perm & MEMPERM_WRITE) && mem.size >= CTH_LAYOUT_SLOTS * 8 &&
            mem.size <= 0x04000000)
        {
            Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                               process, mem.base_addr, mem.size, 0);
            if (R_SUCCEEDED(map))
            {
                const u64 *words = (const u64 *)mem.base_addr;
                u32 wordCount = mem.size / 8;
                for (u32 i = anchor; i < wordCount && matches < 8; i++)
                {
                    if (words[i] != expected[anchor] || i < anchor)
                        continue;
                    u32 baseIndex = i - anchor;
                    if (baseIndex + CTH_LAYOUT_SLOTS <= wordCount &&
                        memcmp(words + baseIndex, expected, CTH_LAYOUT_SLOTS * 8) == 0)
                    {
                        length += sprintf(report + length, " address=%08lx",
                                          mem.base_addr + baseIndex * 8);
                        matches++;
                    }
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
            }
        }
        address = next;
    }
    return length + sprintf(report + length, " matches=%lu\n", matches);
}

static int FindOccupiedGrid(Handle process, char *report, int length,
                            const char *name, const u64 *expected, u32 occupied)
{
    u32 anchor = 0;
    while (anchor < CTH_LAYOUT_SLOTS && expected[anchor] == UINT64_MAX)
        anchor++;
    u32 address = 0x00100000;
    u32 candidates = 0;
    length += sprintf(report + length, "%s occupied-only", name);
    if (anchor == CTH_LAYOUT_SLOTS)
        return length + sprintf(report + length, " candidates=0\n");
    while (address < 0x40000000 && candidates < 16)
    {
        MemInfo mem = {0};
        PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, process, address);
        if (R_FAILED(query) || mem.size == 0)
            break;
        u32 next = mem.base_addr + mem.size;
        if (next <= address)
            break;
        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            (mem.perm & MEMPERM_WRITE) && mem.size >= CTH_LAYOUT_SLOTS * 8 &&
            mem.size <= 0x04000000)
        {
            Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                               process, mem.base_addr, mem.size, 0);
            if (R_SUCCEEDED(map))
            {
                const u64 *words = (const u64 *)mem.base_addr;
                u32 wordCount = mem.size / 8;
                for (u32 i = anchor; i < wordCount && candidates < 16; i++)
                {
                    if (words[i] != expected[anchor])
                        continue;
                    u32 baseIndex = i - anchor;
                    if (baseIndex + CTH_LAYOUT_SLOTS > wordCount)
                        continue;
                    u32 score = 0;
                    for (u32 pos = 0; pos < CTH_LAYOUT_SLOTS; pos++)
                        if (expected[pos] != UINT64_MAX &&
                            words[baseIndex + pos] == expected[pos])
                            score++;
                    if (score == occupied)
                    {
                        length += sprintf(report + length, " address=%08lx score=%lu/%lu",
                            mem.base_addr + baseIndex * 8, score, occupied);
                        candidates++;
                    }
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
            }
        }
        address = next;
    }
    return length + sprintf(report + length, " candidates=%lu\n", candidates);
}

static int FindProcessedGrids(Handle process, char *report, int length)
{
    static u64 nandGrid[CTH_LAYOUT_SLOTS];
    static u64 sdGrid[CTH_LAYOUT_SLOTS];
    u32 nandOccupied = 0, sdOccupied = 0;
    bool nandOk = BuildExpectedGrid(process, CTH_NAND_RAW_ADDRESS, true,
                                    nandGrid, &nandOccupied);
    bool sdOk = BuildExpectedGrid(process, CTH_SD_RAW_ADDRESS, false,
                                  sdGrid, &sdOccupied);
    length += sprintf(report + length, "\n[EXACT PROCESSED GRIDS]\n");
    if (nandOk)
    {
        length = FindExactGrid(process, report, length, "NAND", nandGrid, nandOccupied);
        length = FindOccupiedGrid(process, report, length, "NAND", nandGrid, nandOccupied);
    }
    else
        length += sprintf(report + length, "NAND build=failed\n");
    if (sdOk)
        length = FindExactGrid(process, report, length, "SD", sdGrid, sdOccupied);
    else
        length += sprintf(report + length, "SD build=failed\n");
    return length;
}

static Result AttachRuntimeReport(void)
{
    ShowProgress("Locating USA HOME Menu", 0, 7);
    Handle process = 0;
    u32 pid = 0;
    Result res = OpenUsaHomeMenu(&process, &pid);
    static char report[12288];
    int length = sprintf(report,
        "Cthulhu post-boot HOME Menu attachment diagnostic v2\n"
        "Read-only: no hooks or positions are modified.\n\n"
        "[PROCESS]\nopen=%08lx pid=%lu expectedTitleId=%016llx\n",
        res, pid, CTH_USA_HOME_MENU_TITLE_ID);
    if (R_SUCCEEDED(res))
    {
        ShowProgress("Validating HOME Menu code", 1, 7);
        length = CheckCodeSignatures(process, report, length);
        ShowProgress("Validating NAND layout", 2, 7);
        length += sprintf(report + length, "\n[KNOWN LIVE LAYOUTS]\n");
        length = CheckKnownLayout(process, report, length, "Launcher.dat",
                                  CTH_NAND_RAW_ADDRESS, true);
        ShowProgress("Validating SD layout", 3, 7);
        length = CheckKnownLayout(process, report, length, "SaveData.dat",
                                  CTH_SD_RAW_ADDRESS, false);
        ShowProgress("Finding stable layout references", 4, 7);
        length = FindLayoutReferences(process, report, length);
        ShowProgress("Finding exact processed grids", 5, 7);
        length = FindProcessedGrids(process, report, length);
        svcCloseHandle(process);
    }
    ShowProgress("Writing attachment report", 6, 7);
    IFile file = {0};
    Result open = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/home-runtime-attach.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(open))
    {
        u64 written = 0;
        Result write = IFile_Write(&file, &written, report, (u32)length, 0);
        if (R_SUCCEEDED(write))
            write = IFile_SetSize(&file, (u32)length);
        IFile_Close(&file);
        if (R_SUCCEEDED(res))
            res = write;
    }
    else if (R_SUCCEEDED(res))
        res = open;
    ShowProgress("Attachment diagnostic complete", 7, 7);
    return res;
}

void CthulhuHomeMenu_AttachRuntime(void)
{
    Result res = AttachRuntimeReport();
    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Cthulhu HOME Menu runtime");
        if (R_SUCCEEDED(res))
            Draw_DrawString(10, 35, COLOR_WHITE,
                "Read-only attachment complete.\n\n"
                "Report: /3ds/Cthulhu/home-runtime-attach.txt\n\n"
                "Press B to return.");
        else
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Attachment failed: 0x%08lx\n\nPress B to return.", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

static Result StartFrameworkPostBoot(u32 *pidOut, u32 *threadOut)
{
    Handle process = 0;
    u32 pid = 0;
    Result res = OpenUsaHomeMenu(&process, &pid);
    if (R_FAILED(res))
        return res;

    static char signatureReport[2048];
    int length = CheckCodeSignatures(process, signatureReport, 0);
    bool signaturesOk = length > 0 && strstr(signatureReport, "match=0") == NULL;
    if (!signaturesOk)
        res = MAKERESULT(RL_PERMANENT, RS_INVALIDSTATE, RM_LDR, RD_INVALID_ADDRESS);
    else
        res = CthulhuPostBoot_LoadPlugin(process, pid, threadOut);

    *pidOut = pid;
    if (R_FAILED(res))
        svcCloseHandle(process);
    // On success PluginLoaderCtx owns the process handle until HOME Menu exits.
    return res;
}

static void WriteFrameworkAttachResult(Result result, u32 pid, u32 thread)
{
    char report[512];
    int length = sprintf(report,
        "Cthulhu HOME framework post-boot attachment v1\n"
        "framework_version=" CTH_FRAMEWORK_BUILD_VERSION "\n"
        "result=%08lx\nstage=%s\npid=%lu\nthread=%lu\n"
        "mutations=disabled\n",
        result, CthulhuPostBoot_LastStage(), pid, thread);
    IFile file = {0};
    if (R_SUCCEEDED(IFile_Open(&file, ARCHIVE_SDMC,
        fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/framework-attach.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        u64 written = 0;
        if (R_SUCCEEDED(IFile_Write(&file, &written, report, length, FS_WRITE_FLUSH)))
            IFile_SetSize(&file, length);
        IFile_Close(&file);
    }
}

void CthulhuHomeMenu_StartFramework(void)
{
    u32 pid = 0;
    u32 thread = 0;
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_DrawString(10, 10, COLOR_TITLE, "Start Cthulhu HOME framework");
    Draw_DrawString(10, 35, COLOR_WHITE,
        "Read-only diagnostic v" CTH_FRAMEWORK_BUILD_VERSION "\n\n"
        "This validates HOME Menu, maps the staged 3GX,\n"
        "and starts it through a one-shot post-boot\n"
        "trampoline. No layout positions are modified.\n\n"
        "Press A to attach or B to cancel.");
    Draw_FlushFramebuffer();
    Draw_Unlock();

    u32 input;
    do input = waitInput(); while (!(input & (KEY_A | KEY_B)) && !menuShouldExit);
    if (!(input & KEY_A) || menuShouldExit)
        return;

    ShowProgress("Opening HOME Menu", 0, 4);
    ShowProgress("Validating code signatures", 1, 4);
    ShowProgress("Mapping diagnostic plugin", 2, 4);
    Result res = StartFrameworkPostBoot(&pid, &thread);
    WriteFrameworkAttachResult(res, pid, thread);
    ShowProgress("Resuming HOME Menu", 3, 4);
    ShowProgress("Post-boot attachment complete", 4, 4);

    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Cthulhu framework result");
        if (R_SUCCEEDED(res))
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Framework bootstrap succeeded.\n\n"
                "PID: %lu  Thread: %lu\n"
                "Version: " CTH_FRAMEWORK_BUILD_VERSION "\n\n"
                "Exit Rosalina, then reopen it to inspect\n"
                "the Cthulhu plugin status.\n\nPress B to return.",
                pid, thread);
        else
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Attachment failed: 0x%08lx\nStage: %s\n"
                "PID: %lu  Thread: %lu\n\n"
                "No layout changes were attempted.\n"
                "Press B to return.", res, CthulhuPostBoot_LastStage(), pid, thread);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void CthulhuHomeMenu_CheckStaticHook(void)
{
    const u32 callAddress = 0x00100000;
    const u32 caveAddress = 0x00305174;
    const u32 markerAddress = 0x003827F0;
    const u32 expectedMarker = 0x43545337;
    const s32 displacement = (s32)caveAddress - (s32)(callAddress + 8);
    const u32 expectedBranch = 0xEB000000 |
        (((u32)(displacement >> 2)) & 0x00FFFFFF);
    Handle process = 0;
    u32 pid = 0, call = 0, stub = 0, marker = 0, channel[15] = {0}, graphics[18] = {0};
    Result res = OpenUsaHomeMenu(&process, &pid);
    bool installed = false, executed = false;
    if (R_SUCCEEDED(res))
    {
        MemInfo mem = {0}; PageInfo page = {0};
        res = svcQueryProcessMemory(&mem, &page, process, callAddress);
        if (R_SUCCEEDED(res))
        {
            res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                process, mem.base_addr, mem.size, 0);
            if (R_SUCCEEDED(res))
            {
                call = *(volatile u32 *)callAddress;
                stub = *(volatile u32 *)caveAddress;
            installed = call == expectedBranch && stub == 0xE92D500F;
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
            }
        }
        if (R_SUCCEEDED(res))
        {
            res = svcQueryProcessMemory(&mem, &page, process, markerAddress);
            if (R_SUCCEEDED(res))
            {
                res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                    process, mem.base_addr, mem.size, 0);
                if (R_SUCCEEDED(res))
                {
                    marker = *(volatile u32 *)markerAddress;
                    for (u32 i = 0; i < 15; i++) channel[i] = *(volatile u32 *)(markerAddress + 4 + 4*i);
                    for (u32 i = 0; i < 18; i++) graphics[i] = *(volatile u32 *)(markerAddress + 0x40 + 4*i);
                    executed = marker == expectedMarker;
                    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
                }
            }
        }
    }
    static char gxReport[1024];
    int gxLength = sprintf(gxReport,
        "Cthulhu HOME OSD sorting runtime v1.6.7\n"
        "result=%08lx\npid=%lu\ninstalled=%lu\nexecuted=%lu\n"
        "call=%08lx\nstub=%08lx\nmarker=%08lx\nabi=%08lx\nheartbeat=%lu\ncommand=%lu\n"
        "gx_header=%08lx\nsubmit_result=%08lx\nchord_latch=%lu\nrender_count=%lu\n"
        "panel_buffer=%08lx\nallocation_result=%08lx\n"
        "second_submit_result=%08lx\npanel_ready=%lu\ndisplay_hook_count=%lu\n"
        "memory_fill=%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n"
        "display_transfer=%08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
        res, pid, (u32)installed, (u32)executed, call, stub, marker, channel[0], channel[1], channel[7],
        graphics[16], channel[8], channel[9], channel[10], channel[11], channel[12],
        channel[13], channel[14], graphics[17],
        graphics[0], graphics[1], graphics[2], graphics[3], graphics[4],
        graphics[5], graphics[6], graphics[7], graphics[8], graphics[9], graphics[10],
        graphics[11], graphics[12], graphics[13], graphics[14], graphics[15]);
    Result logResult = MAKERESULT(RL_PERMANENT, RS_NOTFOUND, RM_FS, RD_NOT_FOUND);
    IFile gxFile = {0};
    if (gxLength > 0)
    {
        logResult = IFile_Open(&gxFile, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
            fsMakePath(PATH_ASCII, "/3ds/Cthulhu/framework-gx-v110.txt"),
            FS_OPEN_CREATE | FS_OPEN_WRITE);
        if (R_SUCCEEDED(logResult))
        {
            u64 written = 0;
            logResult = IFile_Write(&gxFile, &written, gxReport, (u32)gxLength, FS_WRITE_FLUSH);
            if (R_SUCCEEDED(logResult)) logResult = IFile_SetSize(&gxFile, (u32)gxLength);
            IFile_Close(&gxFile);
        }
    }
    do {
        Draw_Lock(); Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE,
            "Cthulhu HOME OSD sorting runtime v1.6.7");
        Draw_DrawFormattedString(10, 35, COLOR_WHITE,
            "Installed: %s   Executed: %s\nMarker: %08lx\n"
            "Buffer: %08lx  Alloc: %08lx Ready:%lu\n"
            "Command:%lu DMA:%08lx/%08lx Hook:%lu\nLog:%08lx\n\n"
            "Select+Y toggles the dark sidebar.\n"
            "Log: /3ds/Cthulhu/framework-gx-v110.txt\n\n"
            "Press B to return.",
            installed ? "YES" : "NO", executed ? "YES" : "NO",
            marker, channel[11], channel[12], channel[14], channel[7],
            channel[8], channel[13], graphics[17], logResult);
        Draw_FlushFramebuffer(); Draw_Unlock();
    } while (!(waitInput() & KEY_B) && !menuShouldExit);
    if (process) svcCloseHandle(process);
}

static u8 g_sortRequest[0x20000];
static u8 g_sortRaw[CTH_SD_LAYOUT_SIZE];
static u8 g_sortOriginal[CTH_SD_LAYOUT_SIZE];
static u8 g_sortCommitted[CTH_SD_LAYOUT_SIZE];
static u8 g_launcherRaw[CTH_LAUNCHER_SIZE];
static u8 g_launcherOriginal[CTH_LAUNCHER_SIZE];
static u32 g_launcherCandidateAddresses[16];
static u32 g_launcherCandidateCount;
static u64 g_sortGrid[CTH_PROCESSED_ENTRIES];
static CthSdMutation g_sortMutations[CTH_LAYOUT_SLOTS];
static CthFolderMutation g_folderMutations[CTH_FOLDER_COUNT];
static char g_sortDetails[0xC000];
static u32 g_sortDetailsLength;
static CthFolderPlan g_folderPlan;
static u32 g_lastRawAddress;
static u32 g_lastProcessedAddress;

static Result DiscoverLauncherRuntime(Handle process, u32 preferredAddress,
                                      u32 *addressOut,
                                      MemInfo *regionOut, u32 *matchesOut);

static u32 CountCredibleLiveFolders(const u8 *candidate)
{
    u32 count = 0;
    for (u32 folder = 0; folder < CTH_FOLDER_COUNT; folder++)
    {
        s16 position = ReadS16(candidate,
            CTH_FOLDER_POSITION_OFFSET - 8 + folder * 2);
        u32 number = 0;
        memcpy(&number, candidate + CTH_FOLDER_NUMBER_OFFSET - 8 +
               folder * 4, 4);
        u16 firstName = 0;
        memcpy(&firstName, candidate + CTH_FOLDER_NAME_OFFSET - 8 +
               folder * 34, 2);
        if (position >= 0 && position < CTH_LAYOUT_SLOTS &&
            number > 0 && number < 0x10000 && firstName != 0 &&
            firstName != 0xFFFF)
            count++;
    }
    return count;
}

static void WriteSortJournal(const char *stage, Result result)
{
    g_launcherCandidateCount = 0;
    char text[256];
    int length = sprintf(text,
        "Cthulhu persistent sort journal v1\n"
        "sorter_version=" CTH_SORT_BUILD_VERSION "\n"
        "stage=%s\nresult=%08lx\n"
        "A final result of ffffffff means the stage had started.\n",
        stage, result);
    IFile file = {0};
    if (R_SUCCEEDED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/sort-journal.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        u64 written = 0;
        Result write = IFile_Write(&file, &written, text, length, FS_WRITE_FLUSH);
        if (R_SUCCEEDED(write)) IFile_SetSize(&file, length);
        IFile_Close(&file);
    }
}

static Result __attribute__((unused))
WriteFolderPlan(u16 algorithm, u32 folderCount, u32 launcherAddress)
{
    memset(&g_folderPlan, 0, sizeof(g_folderPlan));
    g_folderPlan.magic = CTH_FOLDER_PLAN_MAGIC;
    g_folderPlan.version = CTH_FOLDER_PLAN_VERSION;
    g_folderPlan.count = (u16)folderCount;
    g_folderPlan.algorithm = algorithm;
    g_folderPlan.launcherAddress = launcherAddress;
    memcpy(g_folderPlan.folders, g_folderMutations,
           folderCount * sizeof(CthFolderMutation));
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-folder-plan.bin"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, &g_folderPlan, sizeof(g_folderPlan),
                      FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != sizeof(g_folderPlan)) res = (Result)-60;
    if (R_SUCCEEDED(res)) res = IFile_SetSize(&file, sizeof(g_folderPlan));
    IFile_Close(&file);
    return res;
}

static Result __attribute__((unused)) ReadFolderPlan(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-folder-plan.bin"),
        FS_OPEN_READ);
    if (R_FAILED(res)) return res;
    u64 size = 0, read = 0;
    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res) && size != sizeof(g_folderPlan)) res = (Result)-61;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, &g_folderPlan, sizeof(g_folderPlan));
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && read != sizeof(g_folderPlan)) res = (Result)-62;
    if (R_SUCCEEDED(res) &&
        (g_folderPlan.magic != CTH_FOLDER_PLAN_MAGIC ||
         g_folderPlan.version != CTH_FOLDER_PLAN_VERSION ||
         g_folderPlan.count > CTH_FOLDER_COUNT))
        res = (Result)-63;
    return res;
}

static Result DisarmFolderPlan(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-folder-plan.bin"),
        FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    u32 clearedMagic = 0;
    u64 written = 0;
    res = IFile_Write(&file, &written, &clearedMagic, sizeof(clearedMagic),
                      FS_WRITE_FLUSH);
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && written != sizeof(clearedMagic)) res = (Result)-66;
    return res;
}

static Result WriteSdSaveData(void);

static Result ArmPendingSortCommit(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-sort-commit.bin"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, g_sortRaw, sizeof(g_sortRaw),
                      FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != sizeof(g_sortRaw)) res = (Result)-82;
    if (R_SUCCEEDED(res)) res = IFile_SetSize(&file, sizeof(g_sortRaw));
    IFile_Close(&file);
    return res;
}

static Result ReadPendingSortCommit(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-sort-commit.bin"),
        FS_OPEN_READ);
    if (R_FAILED(res)) return res;
    u64 size = 0, read = 0;
    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res) && size != sizeof(g_sortRaw)) res = (Result)-83;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, g_sortRaw, sizeof(g_sortRaw));
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && read != sizeof(g_sortRaw)) res = (Result)-84;
    u16 used = 0;
    if (R_SUCCEEDED(res) && !ValidateLayout(g_sortRaw, false, &used))
        res = (Result)-85;
    return res;
}

static Result DisarmPendingSortCommit(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-sort-commit.bin"),
        FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    res = IFile_SetSize(&file, 0);
    IFile_Close(&file);
    return res;
}

static Result __attribute__((unused)) ArmPendingLauncherCommit(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-launcher-commit.bin"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, g_launcherRaw, CTH_LAUNCHER_SIZE,
                      FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != CTH_LAUNCHER_SIZE) res = (Result)-86;
    if (R_SUCCEEDED(res)) res = IFile_SetSize(&file, CTH_LAUNCHER_SIZE);
    IFile_Close(&file);
    return res;
}

static Result __attribute__((unused)) ReadPendingLauncherCommit(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-launcher-commit.bin"),
        FS_OPEN_READ);
    if (R_FAILED(res)) return res;
    u64 size = 0, read = 0;
    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res) && size != CTH_LAUNCHER_SIZE) res = (Result)-87;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, g_launcherRaw, CTH_LAUNCHER_SIZE);
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && read != CTH_LAUNCHER_SIZE) res = (Result)-88;
    u16 used = 0;
    if (R_SUCCEEDED(res) && !ValidateLayout(g_launcherRaw, true, &used))
        res = (Result)-89;
    return res;
}

static Result DisarmPendingLauncherCommit(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pending-launcher-commit.bin"),
        FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    res = IFile_SetSize(&file, 0);
    IFile_Close(&file);
    return res;
}

static Result ReadCurrentLauncherData(void)
{
    u32 archiveData[2] = {MEDIATYPE_NAND, 0x0002008F};
    FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SYSTEM_SAVEDATA, archivePath,
        fsMakePath(PATH_ASCII, "/Launcher.dat"), FS_OPEN_READ);
    u64 read = 0;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, g_launcherRaw, CTH_LAUNCHER_SIZE);
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && read != CTH_LAUNCHER_SIZE)
        res = (Result)-94;
    return res;
}

static Result BackupCurrentLauncherData(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII,
                   "/3ds/Cthulhu/pre-folder-commit-Launcher.dat"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, g_launcherRaw, CTH_LAUNCHER_SIZE,
                      FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != CTH_LAUNCHER_SIZE)
        res = (Result)-96;
    if (R_SUCCEEDED(res)) res = IFile_SetSize(&file, CTH_LAUNCHER_SIZE);
    IFile_Close(&file);
    return res;
}

static Result WriteLauncherFolderPositions(void)
{
    u32 archiveData[2] = {MEDIATYPE_NAND, 0x0002008F};
    FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
    FS_Archive archive = {0};
    Handle file = 0;
    Result res = FSUSER_OpenArchive(&archive, ARCHIVE_SYSTEM_SAVEDATA,
                                    archivePath);
    Result openArchive = res, openFile = (Result)-1;
    u32 writes = 0, verified = 0;
    if (R_SUCCEEDED(res))
        res = openFile = FSUSER_OpenFile(&file, archive,
            fsMakePath(PATH_ASCII, "/Launcher.dat"),
            FS_OPEN_READ | FS_OPEN_WRITE, 0);
    for (u32 i = 0; R_SUCCEEDED(res) && i < g_folderPlan.count; i++)
    {
        CthFolderMutation *folder = &g_folderPlan.folders[i];
        u64 positionOffset = CTH_FOLDER_POSITION_OFFSET + folder->id * 2;
        u64 numberOffset = CTH_FOLDER_NUMBER_OFFSET + folder->id * 4;
        s16 currentPosition = -1, readbackPosition = -1;
        u32 currentNumber = 0, transferred = 0;
        res = FSFILE_Read(file, &transferred, positionOffset,
                          &currentPosition, 2);
        if (R_SUCCEEDED(res) && transferred != 2) res = (Result)-98;
        if (R_SUCCEEDED(res))
            res = FSFILE_Read(file, &transferred, numberOffset,
                              &currentNumber, 4);
        if (R_SUCCEEDED(res) && transferred != 4) res = (Result)-99;
        if (R_SUCCEEDED(res) &&
            ((folder->oldPosition >= 0 &&
              currentPosition != folder->oldPosition) ||
             currentNumber != folder->number))
            res = (Result)-97;
        if (R_SUCCEEDED(res))
            res = FSFILE_Write(file, &transferred, positionOffset,
                               &folder->newPosition, 2, FS_WRITE_FLUSH);
        if (R_SUCCEEDED(res) && transferred != 2) res = (Result)-100;
        if (R_SUCCEEDED(res)) writes++;
        if (R_SUCCEEDED(res))
            res = FSFILE_Read(file, &transferred, positionOffset,
                              &readbackPosition, 2);
        if (R_SUCCEEDED(res) &&
            (transferred != 2 || readbackPosition != folder->newPosition))
            res = (Result)-101;
        if (R_SUCCEEDED(res)) verified++;
    }
    Result closeFile = file ? FSFILE_Close(file) : (Result)-1;
    Result control = (Result)-1;
    if (R_SUCCEEDED(res) && R_FAILED(closeFile)) res = closeFile;
    if (R_SUCCEEDED(res))
    {
        control = FSUSER_ControlArchive(archive,
            ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        if (R_FAILED(control) && control != (Result)0xE0C046F8) res = control;
    }
    Result closeArchive = FSUSER_CloseArchive(archive);
    if (R_SUCCEEDED(res) && R_FAILED(closeArchive)) res = closeArchive;
    char report[512];
    int length = sprintf(report,
        "Cthulhu targeted folder commit report\n"
        "sorter_version=" CTH_SORT_BUILD_VERSION "\n"
        "open_archive=%08lx\nopen_file=%08lx\nplanned=%u\n"
        "writes=%lu\nverified=%lu\nclose_file=%08lx\ncontrol=%08lx\n"
        "close_archive=%08lx\nresult=%08lx\n",
        openArchive, openFile, g_folderPlan.count, (unsigned long)writes,
        (unsigned long)verified, closeFile, control, closeArchive, res);
    IFile reportFile = {0};
    if (R_SUCCEEDED(IFile_Open(&reportFile, ARCHIVE_SDMC,
        fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/launcher-commit.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        u64 reportWritten = 0;
        IFile_Write(&reportFile, &reportWritten, report, length,
                    FS_WRITE_FLUSH);
        IFile_SetSize(&reportFile, length);
        IFile_Close(&reportFile);
    }
    return res;
}

static Result __attribute__((unused)) WriteLauncherData(void)
{
    u32 archiveData[2] = {MEDIATYPE_NAND, 0x0002008F};
    FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
    FS_Archive archive = {0};
    Handle file = 0;
    Result openArchive = FSUSER_OpenArchive(&archive,
        ARCHIVE_SYSTEM_SAVEDATA, archivePath);
    Result res = openArchive;
    bool archiveOpened = R_SUCCEEDED(res);
    Result openFile = (Result)-1, write = (Result)-1;
    Result closeFile = (Result)-1, control = (Result)-1;
    Result closeArchive = (Result)-1, readback = (Result)-1;
    u32 written = 0;
    if (R_SUCCEEDED(res))
        res = openFile = FSUSER_OpenFile(&file, archive,
            fsMakePath(PATH_ASCII, "/Launcher.dat"),
            FS_OPEN_READ | FS_OPEN_WRITE, 0);
    if (R_SUCCEEDED(res))
        res = write = FSFILE_Write(file, &written, 0, g_launcherRaw,
                                   CTH_LAUNCHER_SIZE, FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != CTH_LAUNCHER_SIZE) res = (Result)-90;
    closeFile = file ? FSFILE_Close(file) : (Result)-1;
    if (R_SUCCEEDED(res) && R_FAILED(closeFile)) res = closeFile;
    if (R_SUCCEEDED(res))
    {
        control = FSUSER_ControlArchive(archive,
            ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        if (R_FAILED(control) && control != (Result)0xE0C046F8) res = control;
    }
    closeArchive = archiveOpened ? FSUSER_CloseArchive(archive) : (Result)-1;
    if (R_SUCCEEDED(res) && R_FAILED(closeArchive)) res = closeArchive;

    bool matches = false;
    if (R_SUCCEEDED(res))
    {
        memcpy(g_launcherOriginal, g_launcherRaw, CTH_LAUNCHER_SIZE);
        IFile verify = {0};
        readback = IFile_Open(&verify, ARCHIVE_SYSTEM_SAVEDATA, archivePath,
            fsMakePath(PATH_ASCII, "/Launcher.dat"), FS_OPEN_READ);
        u64 read = 0;
        if (R_SUCCEEDED(readback))
            readback = IFile_Read(&verify, &read, g_launcherRaw,
                                  CTH_LAUNCHER_SIZE);
        IFile_Close(&verify);
        matches = R_SUCCEEDED(readback) && read == CTH_LAUNCHER_SIZE &&
            memcmp(g_launcherRaw, g_launcherOriginal, CTH_LAUNCHER_SIZE) == 0;
        memcpy(g_launcherRaw, g_launcherOriginal, CTH_LAUNCHER_SIZE);
        if (R_FAILED(readback)) res = readback;
        else if (!matches) res = (Result)-91;
    }

    char report[640];
    int length = sprintf(report,
        "Cthulhu Launcher commit report\n"
        "sorter_version=" CTH_SORT_BUILD_VERSION "\n"
        "open_archive=%08lx\nopen_file=%08lx\nwrite=%08lx\nwritten=%lu\n"
        "close_file=%08lx\ncontrol=%08lx\nclose_archive=%08lx\n"
        "readback=%08lx\nreadback_matches=%u\nresult=%08lx\n",
        openArchive, openFile, write, (unsigned long)written, closeFile,
        control, closeArchive, readback, matches, res);
    IFile reportFile = {0};
    if (R_SUCCEEDED(IFile_Open(&reportFile, ARCHIVE_SDMC,
        fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/launcher-commit.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        u64 reportWritten = 0;
        IFile_Write(&reportFile, &reportWritten, report, length,
                    FS_WRITE_FLUSH);
        IFile_SetSize(&reportFile, length);
        IFile_Close(&reportFile);
    }
    return res;
}

void CthulhuHomeMenu_HandleShutdownNotification(u32 notificationId)
{
    (void)notificationId;
    Result pendingResult = ReadPendingSortCommit();
    if (R_SUCCEEDED(pendingResult))
    {
        Result folderPending = ReadFolderPlan();
        bool hasFolderPlan = R_SUCCEEDED(folderPending) &&
                             g_folderPlan.count > 0;
        WriteSortJournal("shutdown-sort-delay", (Result)-1);
        svcSleepThread(1500 * 1000 * 1000LL);
        pendingResult = WriteSdSaveData();
        if (R_SUCCEEDED(pendingResult) && hasFolderPlan)
            pendingResult = ReadCurrentLauncherData();
        if (R_SUCCEEDED(pendingResult) && hasFolderPlan)
            pendingResult = BackupCurrentLauncherData();
        if (R_SUCCEEDED(pendingResult) && hasFolderPlan)
            pendingResult = WriteLauncherFolderPositions();
        Result commitResult = pendingResult;
        (void)DisarmPendingSortCommit();
        if (R_SUCCEEDED(folderPending)) (void)DisarmFolderPlan();
        (void)DisarmPendingLauncherCommit();
        WriteSortJournal(R_SUCCEEDED(commitResult) ?
            "shutdown-sort-committed" : "shutdown-sort-failed",
            commitResult);
    }
    Result observation = ReadFolderPlan();
    if (R_SUCCEEDED(observation) && g_folderPlan.algorithm == 0xFFFE &&
        g_folderPlan.count > 0)
    {
        WriteSortJournal("shutdown-folder-position-delay", (Result)-1);
        svcSleepThread(1500 * 1000 * 1000LL);
        observation = ReadCurrentLauncherData();
        if (R_SUCCEEDED(observation))
            observation = BackupCurrentLauncherData();
        if (R_SUCCEEDED(observation))
            observation = WriteLauncherFolderPositions();
        (void)DisarmFolderPlan();
        WriteSortJournal(R_SUCCEEDED(observation) ?
            "shutdown-folder-position-committed" :
            "shutdown-folder-position-failed", observation);
    }
    observation = ReadFolderPlan();
    if (R_SUCCEEDED(observation) && g_folderPlan.algorithm == 0xFFFF &&
        g_folderPlan.count == 0)
    {
        WriteSortJournal("shutdown-observation-delay", (Result)-1);
        svcSleepThread(1500 * 1000 * 1000LL);
        observation = ReadCurrentLauncherData();
        if (R_SUCCEEDED(observation))
            observation = BackupCurrentLauncherData();
        (void)DisarmFolderPlan();
        WriteSortJournal(R_SUCCEEDED(observation) ?
            "shutdown-observation-captured" :
            "shutdown-observation-failed", observation);
    }
}

static u32 CthCrc32(const void *data, u32 size)
{
    const u8 *bytes = (const u8 *)data;
    u32 crc = 0xFFFFFFFF;
    for (u32 i = 0; i < size; i++)
    {
        crc ^= bytes[i];
        for (u32 bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

static Result ReadSortRequest(CthRequestHeader **headerOut,
                              CthRequestEntry **entriesOut)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/sort-request.bin"), FS_OPEN_READ);
    if (R_FAILED(res))
        return res;
    u64 size = 0, read = 0;
    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res) && (size < sizeof(CthRequestHeader) || size > sizeof(g_sortRequest)))
        res = (Result)-2;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, g_sortRequest, (u32)size);
    IFile_Close(&file);
    if (R_FAILED(res) || read != size)
        return R_FAILED(res) ? res : (Result)-3;
    CthRequestHeader *header = (CthRequestHeader *)g_sortRequest;
    if (header->magic != CTH_REQUEST_MAGIC || header->version != CTH_REQUEST_VERSION ||
        header->entryCount > 900 ||
        header->payloadSize != header->entryCount * sizeof(CthRequestEntry) ||
        size != sizeof(*header) + header->payloadSize ||
        CthCrc32(g_sortRequest + sizeof(*header), header->payloadSize) != header->payloadCrc32)
        return (Result)-4;
    CthRequestEntry *entries = (CthRequestEntry *)(g_sortRequest + sizeof(*header));
    for (u32 i = 0; i < header->entryCount; i++)
    {
        if (entries[i].mediaType > 1 || !LooksLikeTitleId(entries[i].titleId))
            return (Result)-5;
        for (u32 j = 0; j < i; j++)
            if (entries[i].mediaType == entries[j].mediaType &&
                entries[i].titleId == entries[j].titleId)
                return (Result)-6;
    }
    *headerOut = header;
    *entriesOut = entries;
    return 0;
}

static Result ReadSdSaveData(void)
{
    u32 archiveData[3] = {MEDIATYPE_SD, 0x8F, 0};
    FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_EXTDATA, archivePath,
                            fsMakePath(PATH_ASCII, "/SaveData.dat"), FS_OPEN_READ);
    if (R_FAILED(res))
        return res;
    u64 size = 0, read = 0;
    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res) && size != CTH_SD_LAYOUT_SIZE)
        res = (Result)-10;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, g_sortRaw, sizeof(g_sortRaw));
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && read != sizeof(g_sortRaw))
        res = (Result)-11;
    return res;
}

static Result WriteSdSaveData(void)
{
    u32 archiveData[3] = {MEDIATYPE_SD, 0x8F, 0};
    FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
    FS_Archive archive = {0};
    Handle file = 0;
    bool archiveOpened = false;
    Result openArchiveResult = FSUSER_OpenArchive(&archive, ARCHIVE_EXTDATA,
                                                   archivePath);
    Result res = openArchiveResult;
    archiveOpened = R_SUCCEEDED(res);
    Result openFileResult = (Result)-1;
    if (R_SUCCEEDED(res))
        res = openFileResult = FSUSER_OpenFile(&file, archive,
            fsMakePath(PATH_ASCII, "/SaveData.dat"),
            FS_OPEN_READ | FS_OPEN_WRITE, 0);
    u32 written = 0;
    Result writeResult = (Result)-1;
    if (R_SUCCEEDED(res))
        res = writeResult = FSFILE_Write(file, &written, 0, g_sortRaw,
                                         sizeof(g_sortRaw), FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != sizeof(g_sortRaw))
        res = (Result)-12;
    Result closeFileResult = file ? FSFILE_Close(file) : (Result)-1;
    if (R_SUCCEEDED(res) && R_FAILED(closeFileResult)) res = closeFileResult;
    Result controlResult = (Result)-1;
    if (R_SUCCEEDED(res))
    {
        controlResult = FSUSER_ControlArchive(archive,
            ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);
        /* This FS implementation returns the documented stub response for
           archive control. Original Cthulhu ignores it and closes normally. */
        if (R_FAILED(controlResult) &&
            controlResult != (Result)0xE0C046F8)
            res = controlResult;
    }
    Result closeArchiveResult = archiveOpened ?
        FSUSER_CloseArchive(archive) : (Result)-1;
    if (R_SUCCEEDED(res) && R_FAILED(closeArchiveResult))
        res = closeArchiveResult;
    Result readbackResult = (Result)-1;
    bool readbackMatches = false;
    if (R_SUCCEEDED(res))
    {
        memcpy(g_sortCommitted, g_sortRaw, sizeof(g_sortCommitted));
        res = readbackResult = ReadSdSaveData();
        readbackMatches = R_SUCCEEDED(res) &&
            memcmp(g_sortRaw, g_sortCommitted, sizeof(g_sortRaw)) == 0;
        if (R_SUCCEEDED(res) && !readbackMatches)
            res = (Result)-81;
        memcpy(g_sortRaw, g_sortCommitted, sizeof(g_sortRaw));
    }

    char report[640];
    int reportLength = sprintf(report,
        "Cthulhu extdata commit report\n"
        "sorter_version=" CTH_SORT_BUILD_VERSION "\n"
        "open_archive=%08lx\nopen_file=%08lx\nwrite=%08lx\n"
        "written=%lu\nclose_file=%08lx\ncontrol=%08lx\n"
        "control_stub_tolerated=%u\nclose_archive=%08lx\n"
        "readback=%08lx\nreadback_matches=%u\nresult=%08lx\n",
        openArchiveResult, openFileResult, writeResult,
        (unsigned long)written, closeFileResult, controlResult,
        controlResult == (Result)0xE0C046F8, closeArchiveResult,
        readbackResult, readbackMatches, res);
    IFile reportFile = {0};
    if (R_SUCCEEDED(IFile_Open(&reportFile, ARCHIVE_SDMC,
        fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/extdata-commit.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        u64 reportWritten = 0;
        IFile_Write(&reportFile, &reportWritten, report,
                    reportLength, FS_WRITE_FLUSH);
        IFile_SetSize(&reportFile, reportLength);
        IFile_Close(&reportFile);
    }
    return res;
}

static Result BackupSdSaveData(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pre-sort-SaveData.dat"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res))
        return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, g_sortOriginal, sizeof(g_sortOriginal), FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != sizeof(g_sortRaw))
        res = (Result)-13;
    if (R_SUCCEEDED(res))
        res = IFile_SetSize(&file, sizeof(g_sortRaw));
    IFile_Close(&file);
    return res;
}

static Result SaveCommittedSnapshot(void)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/post-sort-SaveData.dat"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res))
        return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, g_sortRaw, sizeof(g_sortRaw), FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != sizeof(g_sortRaw))
        res = (Result)-14;
    if (R_SUCCEEDED(res))
        res = IFile_SetSize(&file, sizeof(g_sortRaw));
    IFile_Close(&file);
    return res;
}

static int FindRawTitleSlot(const u8 *raw, u64 titleId, u16 *slotOut,
                            s16 *positionOut, s8 *folderOut)
{
    int found = 0;
    for (u32 slot = 0; slot < CTH_LAYOUT_SLOTS; slot++)
    {
        if (ReadU64(raw, 8 + slot * 8) != titleId)
            continue;
        found++;
        *slotOut = (u16)slot;
        *positionOut = ReadS16(raw, 0xCB0 + slot * 2);
        *folderOut = *(const s8 *)(raw + 0xF80 + slot);
    }
    return found;
}

static void SortPositions(s16 *positions, u32 count)
{
    for (u32 i = 1; i < count; i++)
    {
        s16 value = positions[i];
        u32 j = i;
        while (j > 0 && positions[j - 1] > value)
        {
            positions[j] = positions[j - 1];
            j--;
        }
        positions[j] = value;
    }
}

static u16 FoldRequestCharacter(u16 value)
{
    return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static int CompareRequestEntries(const CthRequestEntry *left,
                                 const CthRequestEntry *right, u16 algorithm)
{
    int comparison = 0;
    if (algorithm == 6)
        comparison = left->titleId < right->titleId ? -1 :
                     left->titleId > right->titleId ? 1 : 0;
    else
    {
        bool leftNamed = (left->flags & 1) != 0;
        bool rightNamed = (right->flags & 1) != 0;
        /* Unknown names stay last in both directions. */
        if (leftNamed != rightNamed) return leftNamed ? -1 : 1;
        for (u32 i = 0; comparison == 0 && i < 64; i++)
        {
            u16 a = FoldRequestCharacter(left->title[i]);
            u16 b = FoldRequestCharacter(right->title[i]);
            if (a != b) comparison = a < b ? -1 : 1;
            if (a == 0) break;
        }
        if (comparison == 0 && left->titleId != right->titleId)
            comparison = left->titleId < right->titleId ? -1 : 1;
        if (algorithm == 2) comparison = -comparison;
    }
    return comparison;
}

static void SortRequestEntries(CthRequestEntry *entries, u32 count, u16 algorithm)
{
    for (u32 i = 1; i < count; i++)
    {
        CthRequestEntry value = entries[i];
        u32 j = i;
        while (j > 0 && CompareRequestEntries(&entries[j - 1], &value,
                                               algorithm) > 0)
        {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = value;
    }
}

static Result BuildCatalogInRosalina(u32 *countOut, u32 *namedOut)
{
    Result res = amInit();
    if (R_FAILED(res)) return res;
    u32 nandCount = 0, sdCount = 0;
    res = AM_GetTitleCount(MEDIATYPE_NAND, &nandCount);
    if (R_SUCCEEDED(res)) res = AM_GetTitleCount(MEDIATYPE_SD, &sdCount);
    if (R_FAILED(res) || nandCount + sdCount > 900)
    {
        amExit();
        return R_FAILED(res) ? res : (Result)-70;
    }
    static u64 titleIds[900];
    if (R_SUCCEEDED(res))
        res = AM_GetTitleList(&nandCount, MEDIATYPE_NAND, nandCount, titleIds);
    if (R_SUCCEEDED(res))
        res = AM_GetTitleList(&sdCount, MEDIATYPE_SD, sdCount,
                              titleIds + nandCount);
    amExit();
    if (R_FAILED(res)) return res;

    CthRequestHeader *header = (CthRequestHeader *)g_sortRequest;
    CthRequestEntry *entries = (CthRequestEntry *)(header + 1);
    u32 total = nandCount + sdCount, named = 0;
    u32 archiveData[3] = {MEDIATYPE_SD, 0x8F, 0};
    FS_Path archivePath = {PATH_BINARY, sizeof(archiveData), archiveData};
    IFile cacheIndex = {0}, cacheIcons = {0};
    static u8 cacheMap[0x1688];
    res = IFile_Open(&cacheIndex, ARCHIVE_EXTDATA, archivePath,
                     fsMakePath(PATH_ASCII, "/Cache.dat"), FS_OPEN_READ);
    u64 cacheRead = 0;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&cacheIndex, &cacheRead, cacheMap, sizeof(cacheMap));
    IFile_Close(&cacheIndex);
    if (R_SUCCEEDED(res) && cacheRead != sizeof(cacheMap)) res = (Result)-72;
    if (R_SUCCEEDED(res))
        res = IFile_Open(&cacheIcons, ARCHIVE_EXTDATA, archivePath,
                         fsMakePath(PATH_ASCII, "/CacheD.dat"), FS_OPEN_READ);
    if (R_FAILED(res)) return res;
    memset(g_sortRequest, 0, sizeof(CthRequestHeader) +
                              total * sizeof(CthRequestEntry));
    for (u32 i = 0; i < total; i++)
    {
        ShowProgress("Reading installed title names", i, total);
        entries[i].titleId = titleIds[i];
        entries[i].mediaType = i < nandCount ? 0 : 1;
        s32 cacheSlot = -1;
        for (u32 slot = 0; slot < CTH_LAYOUT_SLOTS; slot++)
            if (ReadU64(cacheMap, 8 + slot * 16) == titleIds[i])
            {
                cacheSlot = (s32)slot;
                break;
            }
        if (cacheSlot < 0) continue;
        static u8 smdhPrefix[0x288];
        u64 read = 0;
        cacheIcons.pos = (u64)cacheSlot * 0x36C0;
        Result readRes = IFile_Read(&cacheIcons, &read, smdhPrefix,
                                    sizeof(smdhPrefix));
        if (R_SUCCEEDED(readRes) && read == sizeof(smdhPrefix) &&
            memcmp(smdhPrefix, "SMDH", 4) == 0)
        {
            entries[i].flags = 1;
            memcpy(entries[i].title, smdhPrefix + 8 + 0x200, 0x80);
            entries[i].title[63] = 0;
            named++;
        }
    }
    IFile_Close(&cacheIcons);
    if (named == 0) return (Result)-73;
    header->magic = CTH_REQUEST_MAGIC;
    header->version = CTH_REQUEST_VERSION;
    header->algorithm = 1;
    header->entryCount = total;
    header->payloadSize = total * sizeof(CthRequestEntry);
    header->payloadCrc32 = CthCrc32(entries, header->payloadSize);
    IFile output = {0};
    res = IFile_Open(&output, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/sort-request.bin"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_SUCCEEDED(res))
    {
        u32 size = sizeof(*header) + header->payloadSize;
        u64 written = 0;
        res = IFile_Write(&output, &written, g_sortRequest, size, FS_WRITE_FLUSH);
        if (R_SUCCEEDED(res) && written != size) res = (Result)-71;
        if (R_SUCCEEDED(res)) res = IFile_SetSize(&output, size);
        IFile_Close(&output);
    }
    *countOut = total;
    *namedOut = named;
    return res;
}

void CthulhuHomeMenu_RefreshCatalog(void)
{
    WriteSortJournal("refresh-catalog-start", (Result)-1);
    u32 count = 0, named = 0;
    Result res = BuildCatalogInRosalina(&count, &named);
    WriteSortJournal("refresh-catalog-result", res);
    do
    {
        Draw_Lock(); Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE,
                        "Refresh catalog v" CTH_SORT_BUILD_VERSION);
        if (R_SUCCEEDED(res))
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Catalog generated inside Rosalina.\n\nTitles: %lu\nNamed: %lu\n\nPress B.",
                count, named);
        else
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Catalog refresh failed: 0x%08lx\n\n"
                "Existing catalog was not intentionally removed.\nPress B.", res);
        Draw_FlushFramebuffer(); Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

static bool SearchEntryMatches(const CthRequestEntry *entry, const char *query)
{
    if (entry->mediaType != 1 || !(entry->flags & 1)) return false;
    if (!query[0]) return true;
    for (u32 start = 0; start < 64 && entry->title[start]; start++)
    {
        u32 q = 0;
        while (query[q] && start + q < 64 && entry->title[start + q])
        {
            u16 a = FoldRequestCharacter(entry->title[start + q]);
            u16 b = FoldRequestCharacter((u8)query[q]);
            if (a != b) break;
            q++;
        }
        if (!query[q]) return true;
    }
    return false;
}

static void RequestTitleToAscii(const CthRequestEntry *entry, char *output,
                                u32 capacity)
{
    u32 i = 0;
    for (; i + 1 < capacity && i < 64 && entry->title[i]; i++)
    {
        u16 value = entry->title[i];
        output[i] = value >= 0x20 && value < 0x7F ? (char)value : '?';
    }
    output[i] = 0;
}

void CthulhuHomeMenu_Search(void)
{
    CthRequestHeader *header = NULL;
    CthRequestEntry *entries = NULL;
    Result res = ReadSortRequest(&header, &entries);
    if (R_FAILED(res))
    {
        WriteSortJournal("search-catalog-failed", res);
        do
        {
            Draw_Lock(); Draw_ClearFramebuffer();
            Draw_DrawString(10, 10, COLOR_TITLE, "Cthulhu search");
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Catalog error: 0x%08lx\n\nRefresh the catalog first.\nPress B.", res);
            Draw_FlushFramebuffer(); Draw_Unlock();
        } while (!(waitInput() & KEY_B) && !menuShouldExit);
        return;
    }
    static const char alphabet[] = " abcdefghijklmnopqrstuvwxyz0123456789-'";
    char query[17] = {0};
    u32 queryLength = 0, character = 1, selected = 0;
    static u16 matches[900];
    while (!menuShouldExit)
    {
        u32 matchCount = 0;
        for (u32 i = 0; i < header->entryCount && matchCount < 900; i++)
            if (SearchEntryMatches(&entries[i], query))
                matches[matchCount++] = (u16)i;
        if (matchCount == 0) selected = 0;
        else if (selected >= matchCount) selected = matchCount - 1;
        Draw_Lock(); Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE,
                        "Cthulhu search v" CTH_SORT_BUILD_VERSION);
        Draw_DrawFormattedString(10, 34, COLOR_WHITE,
            "Query: %s%c\nMatches: %lu\n", query, alphabet[character], matchCount);
        for (u32 row = 0; row < 7 && row < matchCount; row++)
        {
            u32 result = selected < 3 ? row : selected - 3 + row;
            if (result >= matchCount) break;
            char title[44];
            RequestTitleToAscii(&entries[matches[result]], title, sizeof(title));
            Draw_DrawFormattedString(10, 80 + row * 18,
                result == selected ? COLOR_TITLE : COLOR_WHITE,
                "%c %s", result == selected ? '>' : ' ', title);
        }
        Draw_DrawString(10, 215, COLOR_WHITE,
            "L/R char  A add  X erase  Up/Down result\nSTART launch  B exit");
        Draw_FlushFramebuffer(); Draw_Unlock();
        u32 input = waitInput();
        if (input & KEY_B) return;
        if (input & KEY_L) character = character ? character - 1 : sizeof(alphabet) - 2;
        if (input & KEY_R) character = (character + 1) % (sizeof(alphabet) - 1);
        if ((input & KEY_A) && queryLength < sizeof(query) - 1)
        {
            query[queryLength++] = alphabet[character];
            query[queryLength] = 0;
            selected = 0;
        }
        if ((input & KEY_X) && queryLength)
            query[--queryLength] = 0;
        if ((input & KEY_DOWN) && selected + 1 < matchCount) selected++;
        if ((input & KEY_UP) && selected) selected--;
        if ((input & KEY_START) && matchCount)
        {
            CthRequestEntry *entry = &entries[matches[selected]];
            FS_ProgramInfo info = {0};
            info.programId = entry->titleId;
            info.mediaType = entry->mediaType ? MEDIATYPE_SD : MEDIATYPE_NAND;
            WriteSortJournal("search-terminate-home", (Result)-1);
            menuLeave();
            res = PMAPP_TerminateCurrentApplication(5LL * 1000 * 1000 * 1000);
            if (R_SUCCEEDED(res))
                res = PMAPP_LaunchTitle(&info,
                    PMLAUNCHFLAG_NORMAL_APPLICATION | PMLAUNCHFLAG_LOAD_DEPENDENCIES);
            WriteSortJournal("search-launch-result", res);
            return;
        }
    }
}

static bool BuildSdGridFromRaw(const u8 *raw, u64 *grid)
{
    for (u32 i = 0; i < CTH_PROCESSED_ENTRIES; i++)
        grid[i] = UINT64_MAX;
    for (u32 slot = 0; slot < CTH_LAYOUT_SLOTS; slot++)
    {
        u64 titleId = ReadU64(raw, 8 + slot * 8);
        if (titleId == UINT64_MAX || titleId == 0)
            continue;
        s16 position = ReadS16(raw, 0xCB0 + slot * 2);
        s8 folder = *(const s8 *)(raw + 0xF80 + slot);
        u32 gridIndex;
        if (folder == -1 && position >= 0 && position < CTH_LAYOUT_SLOTS)
            gridIndex = (u32)position;
        else if (folder >= 0 && folder < CTH_FOLDER_COUNT &&
                 position >= 0 && position < CTH_FOLDER_SLOTS)
            gridIndex = CTH_LAYOUT_SLOTS + (u32)folder * CTH_FOLDER_SLOTS +
                        (u32)position;
        else
            continue;
        if (grid[gridIndex] != UINT64_MAX)
            return false;
        grid[gridIndex] = titleId;
    }
    return true;
}

static Result DiscoverSdRuntime(Handle process, const u8 *expectedRaw,
                                u32 *rawOut, u32 *processedOut, MemInfo *regionOut)
{
    static u64 expectedGrid[CTH_PROCESSED_ENTRIES];
    if (!BuildSdGridFromRaw(expectedRaw, expectedGrid))
        return (Result)-30;
    u32 anchor = 0;
    while (anchor < CTH_PROCESSED_ENTRIES && expectedGrid[anchor] == UINT64_MAX)
        anchor++;
    if (anchor == CTH_PROCESSED_ENTRIES)
        return (Result)-31;

    u32 rawAddress = 0, processedAddress = 0;
    u32 rawMatches = 0, processedMatches = 0;
    u32 address = 0x00100000;
    while (address < 0x40000000)
    {
        MemInfo mem = {0};
        PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, process, address);
        if (R_FAILED(query) || mem.size == 0)
            break;
        u32 next = mem.base_addr + mem.size;
        if (next <= address)
            break;
        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            (mem.perm & MEMPERM_WRITE) && mem.size >= CTH_SD_LAYOUT_SIZE &&
            mem.size <= 0x04000000)
        {
            Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                               process, mem.base_addr, mem.size, 0);
            if (R_SUCCEEDED(map))
            {
                const u8 *bytes = (const u8 *)mem.base_addr;
                for (u32 offset = 0; offset + CTH_SD_LAYOUT_SIZE <= mem.size; offset += 8)
                {
                    if (bytes[offset] == expectedRaw[0] &&
                        memcmp(bytes + offset, expectedRaw, CTH_SD_LAYOUT_SIZE) == 0)
                    {
                        rawAddress = mem.base_addr + offset;
                        rawMatches++;
                    }
                }
                const u64 *words = (const u64 *)bytes;
                u32 wordCount = mem.size / 8;
                for (u32 i = anchor; i < wordCount; i++)
                {
                    if (words[i] != expectedGrid[anchor])
                        continue;
                    u32 base = i - anchor;
                    if (base + CTH_PROCESSED_ENTRIES <= wordCount &&
                        memcmp(words + base, expectedGrid, sizeof(expectedGrid)) == 0)
                    {
                        processedAddress = mem.base_addr + base * 8;
                        processedMatches++;
                    }
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
            }
        }
        address = next;
    }
    if (rawMatches != 1 || processedMatches != 1)
        return (Result)-32;
    MemInfo region = {0};
    PageInfo page = {0};
    Result res = svcQueryProcessMemory(&region, &page, process, rawAddress);
    if (R_FAILED(res) || processedAddress < region.base_addr ||
        processedAddress + sizeof(expectedGrid) > region.base_addr + region.size)
        return R_FAILED(res) ? res : (Result)-33;
    *rawOut = rawAddress;
    *processedOut = processedAddress;
    *regionOut = region;
    return 0;
}

static Result DiscoverLauncherRuntime(Handle process, u32 preferredAddress,
                                      u32 *addressOut,
                                      MemInfo *regionOut, u32 *matchesOut)
{
    u32 foundAddress = 0;
    MemInfo foundRegion = {0};
    u32 matches = 0;
    u32 bestDistance = UINT32_MAX;
    u32 bestFolderCount = 0;
    u32 address = 0x00100000;
    while (address < 0x40000000)
    {
        MemInfo mem = {0};
        PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, process, address);
        if (R_FAILED(query) || mem.size == 0)
            break;
        u32 next = mem.base_addr + mem.size;
        if (next <= address)
            break;
        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            (mem.perm & MEMPERM_WRITE) && mem.size >= CTH_LAUNCHER_SIZE &&
            mem.size <= 0x04000000)
        {
            Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                               process, mem.base_addr, mem.size, 0);
            if (R_SUCCEEDED(map))
            {
                const u8 *bytes = (const u8 *)mem.base_addr;
                for (u32 offset = 0; offset + CTH_LAUNCHER_SIZE <= mem.size;
                     offset += 8)
                {
                    bool quickMatch = false;
                    for (u32 slot = 0; slot < 8; slot++)
                        if (LooksLikeTitleId(ReadU64(bytes + offset,
                                                    8 + slot * sizeof(u64))))
                        {
                            quickMatch = true;
                            break;
                        }
                    if (!quickMatch)
                        continue;
                    u16 used = 0;
                    if (ValidateLayout(bytes + offset, true, &used))
                    {
                        u32 candidateAddress = mem.base_addr + offset;
                        if (g_launcherCandidateCount < 16)
                            g_launcherCandidateAddresses[g_launcherCandidateCount++] =
                                candidateAddress;
                        u32 distance = candidateAddress > preferredAddress ?
                            candidateAddress - preferredAddress :
                            preferredAddress - candidateAddress;
                        u32 credibleFolders =
                            CountCredibleLiveFolders(bytes + offset);
                        if (foundAddress == 0 ||
                            credibleFolders > bestFolderCount ||
                            (credibleFolders == bestFolderCount &&
                             preferredAddress != 0 && distance < bestDistance))
                        {
                            foundAddress = candidateAddress;
                            foundRegion = mem;
                            bestDistance = distance;
                            bestFolderCount = credibleFolders;
                        }
                        matches++;
                        offset += CTH_LAUNCHER_SIZE - 8;
                    }
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
            }
        }
        address = next;
    }
    if (matchesOut) *matchesOut = matches;
    if (preferredAddress != 0 && matches != 0)
    {
        *addressOut = foundAddress;
        *regionOut = foundRegion;
        return 0;
    }
    if (matches != 1)
        return (Result)-74;
    *addressOut = foundAddress;
    *regionOut = foundRegion;
    return 0;
}

static void DumpLauncherCandidates(Handle process)
{
    g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
        "[LAUNCHER_CANDIDATES]\nstored=%lu\n",
        (unsigned long)g_launcherCandidateCount);
    for (u32 i = 0; i < g_launcherCandidateCount; i++)
    {
        u32 address = g_launcherCandidateAddresses[i];
        MemInfo mem = {0};
        PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, process, address);
        Result map = query;
        u32 folders = 0;
        if (R_SUCCEEDED(query) && address >= mem.base_addr &&
            address + CTH_LAUNCHER_SIZE <= mem.base_addr + mem.size)
            map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                        process, mem.base_addr, mem.size, 0);
        if (R_SUCCEEDED(map))
        {
            const u8 *candidate = (const u8 *)address;
            folders = CountCredibleLiveFolders(candidate);
            DumpCandidate(candidate, CTH_LAUNCHER_SIZE, true, address);
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
        }
        g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
            "candidate=%lu address=%08lx query=%08lx map=%08lx folders=%lu\n",
            (unsigned long)i, address, query, map, (unsigned long)folders);
    }
    g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength, "\n");
}

static int CompareFolderNames(const u8 *launcher, u8 left, u8 right)
{
    const u32 names = CTH_FOLDER_NAME_OFFSET;
    for (u32 i = 0; i < 17; i++)
    {
        u16 a, b;
        memcpy(&a, launcher + names + left * 0x22 + i * 2, 2);
        memcpy(&b, launcher + names + right * 0x22 + i * 2, 2);
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return a < b ? -1 : 1;
        if (a == 0) break;
    }
    return left < right ? -1 : left > right ? 1 : 0;
}

static void SortFolderIndexes(u8 *ids, u32 count, const u8 *launcher, bool reverse)
{
    for (u32 i = 1; i < count; i++)
    {
        u8 value = ids[i];
        u32 j = i;
        while (j > 0)
        {
            int comparison = CompareFolderNames(launcher, ids[j - 1], value);
            if ((!reverse && comparison <= 0) || (reverse && comparison >= 0))
                break;
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = value;
    }
}

static Result WriteSortTransactionReport(Result result, u16 algorithm, u32 mutations,
                                         u32 folders, u32 rawAddress,
                                         u32 processedAddress)
{
    char report[512];
    int length = sprintf(report,
        "Cthulhu HOME Menu sort transaction v3\n"
        "sorter_version=" CTH_SORT_BUILD_VERSION "\n"
        "result=%08lx\nalgorithm=%u\ntitle_mutations=%lu\nfolder_mutations=%lu\n"
        "raw=%08lx\nprocessed=%08lx\n"
        "sd_backup=/3ds/Cthulhu/pre-sort-SaveData.dat\n"
        "launcher_backup=/3ds/Cthulhu/pre-sort-Launcher.dat\n\n",
        result, algorithm, mutations, folders, (unsigned long)rawAddress,
        (unsigned long)processedAddress);
    IFile file = {0};
    Result open = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/sort-transaction.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(open))
        return open;
    u64 written = 0;
    Result write = IFile_Write(&file, &written, report, length, 0);
    u64 detailWritten = 0;
    if (R_SUCCEEDED(write) && g_sortDetailsLength != 0)
        write = IFile_Write(&file, &detailWritten, g_sortDetails,
                            g_sortDetailsLength, FS_WRITE_FLUSH);
    if (R_SUCCEEDED(write))
        write = IFile_SetSize(&file, length + g_sortDetailsLength);
    IFile_Close(&file);
    return write;
}

static Result ApplySdSort(u16 selectedAlgorithm, bool stageFolders,
                          bool foldersFirst,
                          u16 *algorithmOut, u32 *mutationsOut)
{
    CthRequestHeader *header = NULL;
    CthRequestEntry *entries = NULL;
    Result res = ReadSortRequest(&header, &entries);
    if (R_FAILED(res))
        return res;
    /* Some homebrew CIAs do not expose a usable cached short title.  Keep
     * known fallbacks here until user-configurable aliases are implemented. */
    for (u32 i = 0; i < header->entryCount; i++)
        if (entries[i].mediaType == 1 &&
            entries[i].titleId == 0x0004000000384A00ULL &&
            !(entries[i].flags & 1))
        {
            static const char alias[] = "VirtuaNES";
            for (u32 c = 0; c < sizeof(alias); c++)
                entries[i].title[c] = (u8)alias[c];
            entries[i].flags |= 1;
        }
    if (selectedAlgorithm == 1 || selectedAlgorithm == 2 || selectedAlgorithm == 6)
        header->algorithm = selectedAlgorithm;
    SortRequestEntries(entries, header->entryCount, header->algorithm);
    *algorithmOut = header->algorithm;
    bool sdCommitted = false;
    g_sortDetailsLength = sprintf(g_sortDetails,
        "[REQUEST]\nentries=%lu algorithm=%u\n\n",
        (unsigned long)header->entryCount, header->algorithm);
    ShowProgress("Reading HOME Menu SD extdata", 1, 7);
    WriteSortJournal("read-sd-extdata", (Result)-1);
    if (R_FAILED(res = ReadSdSaveData()))
        return res;
    memcpy(g_sortOriginal, g_sortRaw, sizeof(g_sortOriginal));
    u16 used = 0;
    if (!ValidateLayout(g_sortRaw, false, &used))
        return (Result)-20;

    Handle process = 0;
    u32 pid = 0;
    ShowProgress("Validating live HOME Menu", 2, 7);
    WriteSortJournal("discover-live-buffers", (Result)-1);
    if (R_FAILED(res = OpenUsaHomeMenu(&process, &pid)))
        return res;
    u32 rawAddress = 0, processedAddress = 0;
    MemInfo mem = {0};
    res = DiscoverSdRuntime(process, g_sortRaw, &rawAddress, &processedAddress, &mem);
    if (R_FAILED(res))
    {
        u32 fallbackRaw = g_lastRawAddress != 0 ? g_lastRawAddress :
                          CTH_SD_RAW_ADDRESS;
        u32 fallbackProcessed = g_lastProcessedAddress != 0 ?
                                g_lastProcessedAddress :
                                CTH_SD_PROCESSED_ADDRESS;
        PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, process,
                                             fallbackRaw);
        if (R_SUCCEEDED(query) && mem.state != MEMSTATE_FREE &&
            (mem.perm & MEMPERM_READ) && (mem.perm & MEMPERM_WRITE) &&
            fallbackProcessed >= mem.base_addr &&
            fallbackProcessed + sizeof(g_sortGrid) <=
                mem.base_addr + mem.size)
        {
            rawAddress = fallbackRaw;
            processedAddress = fallbackProcessed;
            res = 0;
            g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
                "[LIVE_FALLBACK]\nsource=previous-known-model raw=%08lx processed=%08lx\n\n",
                rawAddress, processedAddress);
        }
    }
    if (R_FAILED(res))
    {
        svcCloseHandle(process);
        return res;
    }
    g_lastRawAddress = rawAddress;
    g_lastProcessedAddress = processedAddress;
    u32 launcherAddress = 0, launcherMatches = 0;
    MemInfo launcherMem = {0};
    /* NAND and SD layout objects do not relocate together. */
    u32 preferredLauncher = CTH_NAND_RAW_ADDRESS;
    res = DiscoverLauncherRuntime(process, preferredLauncher,
                                  &launcherAddress, &launcherMem,
                                  &launcherMatches);
    g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
        "[LIVE_DISCOVERY]\nraw=%08lx processed=%08lx launcher=%08lx "
        "preferred_launcher=%08lx launcher_matches=%lu launcher_result=%08lx\n\n",
        rawAddress, processedAddress, launcherAddress,
        preferredLauncher, (unsigned long)launcherMatches, res);
    if (R_FAILED(res))
    {
        svcCloseHandle(process);
        WriteSortTransactionReport(res, header->algorithm, 0, 0,
                                   rawAddress, processedAddress);
        WriteSortJournal("discover-launcher-buffer-failed", res);
        return res;
    }
    if (stageFolders)
        DumpLauncherCandidates(process);
    res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, process,
                                mem.base_addr, mem.size, 0);
    if (R_FAILED(res))
    {
        svcCloseHandle(process);
        return res;
    }
    if (memcmp((const void *)rawAddress, g_sortRaw, sizeof(g_sortRaw)) != 0)
    {
        u16 liveUsed = 0;
        if (!ValidateLayout((const u8 *)rawAddress, false, &liveUsed) ||
            !BuildSdGridFromRaw((const u8 *)rawAddress, g_sortGrid))
            res = (Result)-22;
        else
        {
            memcpy(g_sortRaw, (const void *)rawAddress, sizeof(g_sortRaw));
            g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
                "[LIVE_SOURCE]\nsource=resident-sorted-model titles=%u\n\n",
                liveUsed);
        }
    }
    else if (!BuildSdGridFromRaw(g_sortRaw, g_sortGrid))
        res = (Result)-22;

    u16 launcherUsed = 0;
    bool launcherMappedSeparately = false;
    if (R_SUCCEEDED(res) && launcherMem.base_addr != mem.base_addr)
    {
        res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, launcherMem.base_addr,
                                    process, launcherMem.base_addr,
                                    launcherMem.size, 0);
        launcherMappedSeparately = R_SUCCEEDED(res);
    }
    if (R_SUCCEEDED(res))
    {
        /* The resident object begins at Launcher.dat file offset +8.  Align it
         * to the documented file layout for planning only; it is never used as
         * a complete persisted Launcher image. */
        memset(g_launcherRaw, 0, CTH_LAUNCHER_SIZE);
        g_launcherRaw[0] = 4;
        memcpy(g_launcherRaw + 8, (const void *)launcherAddress,
               CTH_LAUNCHER_SIZE - 8);
    }
    if (R_SUCCEEDED(res) && !ValidateLayout(g_launcherRaw, true, &launcherUsed))
        res = (Result)-43;
    if (R_SUCCEEDED(res))
        memcpy(g_launcherOriginal, g_launcherRaw, CTH_LAUNCHER_SIZE);

    u32 mutationCount = 0;
    if (R_SUCCEEDED(res))
    {
        for (u32 i = 0; i < header->entryCount; i++)
        {
            if (entries[i].mediaType != 1)
                continue;
            u16 slot = 0;
            s16 position = -1;
            s8 folder = -1;
            int matches = FindRawTitleSlot(g_sortRaw, entries[i].titleId,
                                           &slot, &position, &folder);
            if (matches > 1)
            {
                res = (Result)-23;
                break;
            }
            s16 positionLimit = folder == -1 ? CTH_LAYOUT_SLOTS : CTH_FOLDER_SLOTS;
            if (matches == 1 && folder >= -1 && folder < CTH_FOLDER_COUNT &&
                position >= 0 && position < positionLimit)
            {
                g_sortMutations[mutationCount].slot = slot;
                g_sortMutations[mutationCount].oldPosition = position;
                g_sortMutations[mutationCount].folder = folder;
                g_sortMutations[mutationCount].newPosition = position;
                mutationCount++;
            }
        }
    }
    if (R_SUCCEEDED(res) && mutationCount == 0)
        res = (Result)-24;
    if (R_SUCCEEDED(res))
    {
        static s16 positions[CTH_LAYOUT_SLOTS];
        static u16 mutationIndexes[CTH_LAYOUT_SLOTS];
        for (s32 group = -1; R_SUCCEEDED(res) && group < CTH_FOLDER_COUNT; group++)
        {
            u32 groupCount = 0;
            for (u32 i = 0; i < mutationCount; i++)
            {
                if (g_sortMutations[i].folder != group)
                    continue;
                mutationIndexes[groupCount] = (u16)i;
                positions[groupCount] = g_sortMutations[i].oldPosition;
                groupCount++;
            }
            SortPositions(positions, groupCount);
            for (u32 i = 1; i < groupCount; i++)
                if (positions[i] == positions[i - 1])
                    res = (Result)-25;
            for (u32 i = 0; R_SUCCEEDED(res) && i < groupCount; i++)
            {
                CthSdMutation *mutation = &g_sortMutations[mutationIndexes[i]];
                mutation->newPosition = positions[i];
                memcpy(g_sortRaw + 0xCB0 + mutation->slot * 2,
                       &mutation->newPosition, sizeof(s16));
            }
        }
        if (!BuildSdGridFromRaw(g_sortRaw, g_sortGrid))
            res = (Result)-26;
    }

    u32 folderCount = 0;
    u32 topLevelTitleCount = 0;
    if (R_SUCCEEDED(res) && stageFolders &&
        (header->algorithm == 1 || header->algorithm == 2))
    {
        static u8 folderIds[CTH_FOLDER_COUNT];
        s16 firstTitlePosition = CTH_LAYOUT_SLOTS;
        s16 lastTitlePosition = -1;
        for (u32 i = 0; i < mutationCount; i++)
        {
            if (g_sortMutations[i].folder != -1)
                continue;
            topLevelTitleCount++;
            if (g_sortMutations[i].newPosition < firstTitlePosition)
                firstTitlePosition = g_sortMutations[i].newPosition;
            if (g_sortMutations[i].newPosition > lastTitlePosition)
                lastTitlePosition = g_sortMutations[i].newPosition;
        }
        for (u32 folder = 0; folder < CTH_FOLDER_COUNT; folder++)
        {
            s16 position = ReadS16(g_launcherRaw,
                                   CTH_FOLDER_POSITION_OFFSET + folder * 2);
            u32 number = 0;
            memcpy(&number, g_launcherRaw + CTH_FOLDER_NUMBER_OFFSET + folder * 4, 4);
            if (number == 0)
                continue;
            if (position < 0 || position >= CTH_LAYOUT_SLOTS)
            {
                res = (Result)-44;
                break;
            }
            folderIds[folderCount] = (u8)folder;
            g_folderMutations[folderCount].id = (u8)folder;
            g_folderMutations[folderCount].number = number;
            /* The live position is stale after HOME drag operations.  The
             * shutdown writer verifies identity against the real file. */
            g_folderMutations[folderCount].oldPosition = -1;
            g_folderMutations[folderCount].newPosition = position;
            folderCount++;
        }
        SortFolderIndexes(folderIds, folderCount, g_launcherRaw,
                          header->algorithm == 2);
        if (R_SUCCEEDED(res) &&
            (firstTitlePosition >= CTH_LAYOUT_SLOTS || lastTitlePosition < 0))
            res = (Result)-103;
        for (u32 i = 0; R_SUCCEEDED(res) && i < folderCount; i++)
        {
            s16 target = foldersFirst ? firstTitlePosition + i :
                         lastTitlePosition + 1 + i;
            if (target < 0 || target >= CTH_LAYOUT_SLOTS)
            {
                res = (Result)-104;
                break;
            }
            u8 id = folderIds[i];
            for (u32 j = 0; j < folderCount; j++)
                if (g_folderMutations[j].id == id)
                    g_folderMutations[j].newPosition = target;
        }
        for (u32 i = 0; R_SUCCEEDED(res) && i < mutationCount; i++)
            memcpy(g_sortRaw + 0xCB0 + g_sortMutations[i].slot * 2,
                   &g_sortMutations[i].newPosition, 2);
        for (u32 i = 0; R_SUCCEEDED(res) && i < folderCount; i++)
            memcpy(g_launcherRaw + CTH_FOLDER_POSITION_OFFSET +
                   g_folderMutations[i].id * 2,
                   &g_folderMutations[i].newPosition, 2);
        if (R_SUCCEEDED(res) && !BuildSdGridFromRaw(g_sortRaw, g_sortGrid))
            res = (Result)-46;
    }

    g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
        "[GROUPS]\nfolder_count=%lu top_level_titles=%lu\n\n[FOLDERS]\n",
        (unsigned long)folderCount,
        (unsigned long)topLevelTitleCount);
    for (u32 i = 0; i < folderCount; i++)
        g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
            "folder=%u number=%lu old=%d new=%d\n", g_folderMutations[i].id,
            (unsigned long)g_folderMutations[i].number,
            g_folderMutations[i].oldPosition, g_folderMutations[i].newPosition);
    g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
                                   "\n[TITLES]\n");
    for (u32 i = 0; i < mutationCount; i++)
    {
        u64 titleId = ReadU64(g_sortRaw, 8 + g_sortMutations[i].slot * 8);
        char title[65] = {0};
        for (u32 request = 0; request < header->entryCount; request++)
        {
            if (entries[request].titleId != titleId || entries[request].mediaType != 1)
                continue;
            for (u32 c = 0; c < 64 && entries[request].title[c] != 0; c++)
            {
                u16 value = entries[request].title[c];
                title[c] = value >= 0x20 && value < 0x7F ? (char)value : '?';
            }
            break;
        }
        g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
            "slot=%u folder=%d old=%d new=%d title=%016llx name=%s\n",
            g_sortMutations[i].slot, g_sortMutations[i].folder,
            g_sortMutations[i].oldPosition, g_sortMutations[i].newPosition,
            titleId, title);
    }
    ShowProgress("Backing up SaveData.dat", 3, 7);
    WriteSortJournal("write-backups", (Result)-1);
    if (R_SUCCEEDED(res))
        res = BackupSdSaveData();
    if (R_SUCCEEDED(res))
    {
        IFile backup = {0};
        res = IFile_Open(&backup, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
            fsMakePath(PATH_ASCII, "/3ds/Cthulhu/pre-sort-Launcher.dat"),
            FS_OPEN_CREATE | FS_OPEN_WRITE);
        if (R_SUCCEEDED(res))
        {
            u64 written = 0;
            res = IFile_Write(&backup, &written, g_launcherOriginal,
                              CTH_LAUNCHER_SIZE, FS_WRITE_FLUSH);
            if (R_SUCCEEDED(res) && written != CTH_LAUNCHER_SIZE)
                res = (Result)-47;
            if (R_SUCCEEDED(res))
                res = IFile_SetSize(&backup, CTH_LAUNCHER_SIZE);
            IFile_Close(&backup);
        }
    }
    ShowProgress("Committing sorted extdata", 4, 7);
    WriteSortJournal("commit-sd-extdata", (Result)-1);
    if (R_SUCCEEDED(res))
    {
        memcpy(g_sortCommitted, g_sortRaw, sizeof(g_sortCommitted));
        res = WriteSdSaveData();
        if (R_SUCCEEDED(res))
            sdCommitted = true;
    }
    if (R_SUCCEEDED(res))
    {
        res = ReadSdSaveData();
        if (R_SUCCEEDED(res) &&
            memcmp(g_sortRaw, g_sortCommitted, sizeof(g_sortRaw)) != 0)
            res = (Result)-27;
        if (R_SUCCEEDED(res))
            res = SaveCommittedSnapshot();
        if (R_SUCCEEDED(res))
            res = ArmPendingSortCommit();
    }
    if (R_SUCCEEDED(res) && stageFolders)
        res = WriteFolderPlan(header->algorithm, folderCount, launcherAddress);
    if (R_FAILED(res) && stageFolders)
    {
        (void)DisarmPendingSortCommit();
        (void)DisarmPendingLauncherCommit();
        (void)DisarmFolderPlan();
    }
    if (R_FAILED(res) && sdCommitted)
    {
        Result originalFailure = res;
        memcpy(g_sortRaw, g_sortOriginal, CTH_SD_LAYOUT_SIZE);
        Result sdRollback = WriteSdSaveData();
        g_sortDetailsLength += sprintf(g_sortDetails + g_sortDetailsLength,
            "\n[ROLLBACK]\noriginal_failure=%08lx sd=%08lx\n",
            originalFailure, sdRollback);
        res = R_FAILED(sdRollback) ? sdRollback : originalFailure;
    }
    ShowProgress("Updating live HOME Menu", 5, 7);
    WriteSortJournal("update-live-buffers", (Result)-1);
    if (R_SUCCEEDED(res))
    {
        memcpy((void *)rawAddress, g_sortRaw, sizeof(g_sortRaw));
        memcpy((void *)processedAddress, g_sortGrid, sizeof(g_sortGrid));
        svcFlushProcessDataCache(process, rawAddress, sizeof(g_sortRaw));
        svcFlushProcessDataCache(process, processedAddress, sizeof(g_sortGrid));
        *mutationsOut = mutationCount;
    }
    if (launcherMappedSeparately)
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, launcherMem.base_addr,
                                launcherMem.size);
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
    svcCloseHandle(process);
    ShowProgress("Writing transaction report", 6, 7);
    WriteSortTransactionReport(res, header->algorithm, mutationCount, folderCount,
                               rawAddress, processedAddress);
    WriteSortJournal(R_SUCCEEDED(res) ? "ready-for-graceful-reboot" : "failed", res);
    ShowProgress("Sort transaction complete", 7, 7);
    return res;
}

static u32 ScanLiveIconClassV195(Handle home)
{
    char *report = g_layoutBackrefReport;
    int length = sprintf(report,
        "Cthulhu live icon class scan\n"
        "scan_version=1.9.5\nraw=%08lx\nprocessed=%08lx\n"
        "wrapper=003827d8\nrebuild_subobject=003827e4\n",
        g_lastRawAddress, g_lastProcessedAddress);
    const u32 localWindow = 0x00900000;
    const u32 chunkLimit = 0x00010000;
    u32 address = 0x08000000;
    u32 regions = 0, words = 0, rawRefs = 0, gridRefs = 0;
    u32 wrapperRefs = 0, rebuildRefs = 0;
    u32 classRefs = 0, functionRefs = 0;
    u32 iconOwner = 0, iconModel = 0;
    u32 wrapperRefAddresses[16] = {0};
    while (address < 0x40000000 &&
           rawRefs + gridRefs + wrapperRefs + rebuildRefs +
           classRefs + functionRefs < 96)
    {
        MemInfo mem = {0}; PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, home, address);
        if (R_FAILED(query) || mem.size == 0) break;
        u32 next = mem.base_addr + mem.size;
        if (next <= address) break;
        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            mem.size <= 0x04000000)
        {
            bool mappedRegion = false;
            for (u32 offset = 0; offset < mem.size &&
                 rawRefs + gridRefs + wrapperRefs + rebuildRefs +
                 classRefs + functionRefs < 96; )
            {
                u32 chunk = mem.size - offset;
                if (chunk > chunkLimit) chunk = chunkLimit;
                Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE,
                    localWindow, home, mem.base_addr + offset, chunk, 0);
                if (R_FAILED(map)) break;
                mappedRegion = true;
                const u32 *base = (const u32 *)localWindow;
                u32 count = chunk / 4;
                words += count;
                for (u32 i = 0; i < count &&
                     rawRefs + gridRefs + wrapperRefs + rebuildRefs +
                     classRefs + functionRefs < 96; i++)
                {
                    const char *kind = NULL;
                    if (base[i] == g_lastRawAddress) { kind = "raw"; rawRefs++; }
                    else if (base[i] == g_lastProcessedAddress)
                    { kind = "grid"; gridRefs++; }
                    else if (base[i] == 0x003827D8)
                    {
                        kind = "wrapper";
                        if (wrapperRefs < 16)
                            wrapperRefAddresses[wrapperRefs] =
                                mem.base_addr + offset + i * 4;
                        wrapperRefs++;
                    }
                    else if (base[i] == 0x003827E4)
                    { kind = "rebuild"; rebuildRefs++; }
                    else if (base[i] == 0x0030AC50 ||
                             base[i] == 0x0030AC58 ||
                             base[i] == 0x0030AC60)
                    { kind = "class"; classRefs++; }
                    else if (base[i] == 0x001CA504)
                    {
                        kind = "function";
                        functionRefs++;
                        if (i >= 0x120 / 4 && i + 2 < count)
                        {
                            u32 candidate = mem.base_addr + offset + i * 4 - 0x120;
                            u32 model = base[i + 2];
                            if (candidate >= 0x08000000 &&
                                model >= 0x08000000 && model < 0x40000000)
                            {
                                iconOwner = candidate;
                                iconModel = model;
                            }
                        }
                    }
                    if (kind != NULL)
                    {
                        u32 a = mem.base_addr + offset + i * 4;
                        u32 m4 = i >= 1 ? base[i - 1] : 0;
                        u32 m8 = i >= 2 ? base[i - 2] : 0;
                        u32 m12 = i >= 3 ? base[i - 3] : 0;
                        u32 m16 = i >= 4 ? base[i - 4] : 0;
                        u32 p4 = i + 1 < count ? base[i + 1] : 0;
                        u32 p8 = i + 2 < count ? base[i + 2] : 0;
                        u32 p12 = i + 3 < count ? base[i + 3] : 0;
                        u32 p16 = i + 4 < count ? base[i + 4] : 0;
                        length += sprintf(report + length,
                            "%s_ref=%08lx m16=%08lx m12=%08lx m8=%08lx "
                            "m4=%08lx p4=%08lx p8=%08lx p12=%08lx p16=%08lx\n",
                            kind, a, m16, m12, m8, m4, p4, p8, p12, p16);
                        if (length > (int)sizeof(g_layoutBackrefReport) - 256) break;
                    }
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                                         localWindow, chunk);
                offset += chunk;
            }
            if (mappedRegion) regions++;
        }
        address = next;
    }
    u32 ownerRefs = 0;
    address = 0x08000000;
    while (address < 0x40000000 && ownerRefs < 64 &&
           length < (int)sizeof(g_layoutBackrefReport) - 256)
    {
        MemInfo mem = {0}; PageInfo page = {0};
        Result query = svcQueryProcessMemory(&mem, &page, home, address);
        if (R_FAILED(query) || mem.size == 0) break;
        u32 next = mem.base_addr + mem.size;
        if (next <= address) break;
        if (mem.state != MEMSTATE_FREE && (mem.perm & MEMPERM_READ) &&
            mem.size <= 0x04000000)
        {
            for (u32 offset = 0; offset < mem.size && ownerRefs < 64; )
            {
                u32 chunk = mem.size - offset;
                if (chunk > chunkLimit) chunk = chunkLimit;
                Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE,
                    localWindow, home, mem.base_addr + offset, chunk, 0);
                if (R_FAILED(map)) break;
                const u32 *base = (const u32 *)localWindow;
                u32 count = chunk / 4;
                for (u32 i = 0; i < count && ownerRefs < 64; i++)
                {
                    for (u32 target = 0;
                         target < wrapperRefs && target < 16; target++)
                    {
                        if (base[i] != wrapperRefAddresses[target]) continue;
                        u32 a = mem.base_addr + offset + i * 4;
                        length += sprintf(report + length,
                            "owner_ref=%08lx target=%08lx m8=%08lx m4=%08lx "
                            "p4=%08lx p8=%08lx\n", a, base[i],
                            i >= 2 ? base[i - 2] : 0,
                            i >= 1 ? base[i - 1] : 0,
                            i + 1 < count ? base[i + 1] : 0,
                            i + 2 < count ? base[i + 2] : 0);
                        ownerRefs++;
                        break;
                    }
                    if (length > (int)sizeof(g_layoutBackrefReport) - 256) break;
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow, chunk);
                offset += chunk;
            }
        }
        address = next;
    }
    length += sprintf(report + length,
        "regions=%lu\nwords=%lu\nraw_refs=%lu\ngrid_refs=%lu\n"
        "wrapper_refs=%lu\nrebuild_refs=%lu\nclass_refs=%lu\n"
        "function_refs=%lu\nowner_refs=%lu\n"
        "icon_owner=%08lx\nicon_model=%08lx\n",
        (unsigned long)regions, (unsigned long)words,
        (unsigned long)rawRefs, (unsigned long)gridRefs,
        (unsigned long)wrapperRefs, (unsigned long)rebuildRefs,
        (unsigned long)classRefs, (unsigned long)functionRefs,
        (unsigned long)ownerRefs, iconOwner, iconModel);
    if (iconModel != 0)
    {
        u32 matchedGrid[24] = {0};
        u32 matchedRecord[24] = {0};
        u32 matchedRecords = 0;
        u16 desiredRecords[CTH_PROCESSED_ENTRIES] = {0};
        u32 desiredCount = 0, desiredMissing = 0;
        const u32 pointerAddress = iconModel + 0x398F8;
        const u32 pointerPage = pointerAddress & ~0xFFF;
        Result map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow,
            home, pointerPage, 0x1000, 0);
        u32 records = 0;
        if (R_SUCCEEDED(map))
        {
            records = *(volatile u32 *)(localWindow +
                                         (pointerAddress & 0xFFF));
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow, 0x1000);
        }
        length += sprintf(report + length,
            "model_records_pointer=%08lx\nmodel_pointer_result=%08lx\n",
            records, map);
        if (R_SUCCEEDED(map) && records >= 0x08000000 && records < 0x40000000)
        {
            u32 recordPage = records & ~0xFFF;
            u32 recordOffset = records & 0xFFF;
            u32 recordMapSize = 0x3000;
            if (recordOffset + 12 * 0x230 > recordMapSize)
                recordMapSize = 0x4000;
            map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow,
                home, recordPage, recordMapSize, 0);
            if (R_SUCCEEDED(map))
            {
                const u8 *recordBase = (const u8 *)(localWindow + recordOffset);
                for (u32 i = 0; i < 12; i++)
                {
                    const u32 *w = (const u32 *)(recordBase + i * 0x230);
                    length += sprintf(report + length,
                        "model%02lu=%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx,%08lx "
                        "grid=%08lx%08lx\n", i, w[0], w[1], w[2], w[3],
                        w[4], w[5], w[6], w[14],
                        (u32)(g_sortGrid[i] >> 32), (u32)g_sortGrid[i]);
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                                        localWindow, recordMapSize);
            }
            length += sprintf(report + length,
                "model_records_result=%08lx\n", map);
            const u32 fullMapSize = 0x3B000;
            map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow,
                home, recordPage, fullMapSize, 0);
            u32 matched = 0;
            if (R_SUCCEEDED(map))
            {
                const u8 *allRecords = (const u8 *)(localWindow + recordOffset);
                for (u32 gridIndex = 0;
                     gridIndex < CTH_PROCESSED_ENTRIES; gridIndex++)
                {
                    u64 titleId = g_sortGrid[gridIndex];
                    if (titleId == UINT64_MAX || titleId == 0) continue;
                    bool found = false;
                    for (u32 recordIndex = 0; recordIndex < 420; recordIndex++)
                    {
                        const u32 *w = (const u32 *)(allRecords +
                                                     recordIndex * 0x230);
                        u64 recordTitle = ((u64)w[1] << 32) | w[0];
                        if (recordTitle != titleId) continue;
                        if (desiredCount < CTH_PROCESSED_ENTRIES)
                            desiredRecords[desiredCount++] = (u16)recordIndex;
                        if (matched < 24)
                        {
                            length += sprintf(report + length,
                                "match%02lu grid=%lu record=%lu title=%08lx%08lx "
                                "w2=%08lx w3=%08lx w4=%08lx w5=%08lx w6=%08lx "
                                "w7=%08lx w14=%08lx\n", matched, gridIndex,
                                recordIndex, w[1], w[0], w[2], w[3], w[4], w[5],
                                w[6], w[7], w[14]);
                            matchedGrid[matched] = gridIndex;
                            matchedRecord[matched] = recordIndex;
                            matched++;
                        }
                        found = true;
                        break;
                    }
                    if (!found) desiredMissing++;
                }
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                                        localWindow, fullMapSize);
            }
            length += sprintf(report + length,
                "matched_sd_records=%lu\ndesired_records=%lu\n"
                "desired_missing=%lu\nfull_model_result=%08lx\n",
                matched, desiredCount, desiredMissing, map);
            matchedRecords = matched;
        }

        /* HOME's normal UI accessors translate menu/grid positions through
           the signed-16-bit map beginning at model+0x4450E.  Capture that
           map and the adjacent header/pointer at +0x44508 before attempting
           any model mutation. */
        const u32 indexHeaderAddress = iconModel + 0x44500;
        const u32 indexHeaderPage = indexHeaderAddress & ~0xFFF;
        map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow,
            home, indexHeaderPage, 0x1000, 0);
        u32 indirectMap = 0;
        if (R_SUCCEEDED(map))
        {
            const volatile u8 *header = (const volatile u8 *)(localWindow +
                (indexHeaderAddress & 0xFFF));
            const volatile u32 *headerWords = (const volatile u32 *)header;
            indirectMap = headerWords[2];
            length += sprintf(report + length,
                "index_header=%08lx,%08lx,%08lx,%08lx\n"
                "index_indirect=%08lx\n", headerWords[0], headerWords[1],
                headerWords[2], headerWords[3], indirectMap);
            volatile s16 *inlineMap = (volatile s16 *)(header + 0x0E);
            for (u32 i = 0; i < 60; i += 10)
                length += sprintf(report + length,
                    "inline%02lu=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i,
                    inlineMap[i + 0], inlineMap[i + 1], inlineMap[i + 2],
                    inlineMap[i + 3], inlineMap[i + 4], inlineMap[i + 5],
                    inlineMap[i + 6], inlineMap[i + 7], inlineMap[i + 8],
                    inlineMap[i + 9]);
            for (u32 match = 0; match < matchedRecords; match++)
            {
                s32 first = -1;
                u32 occurrences = 0;
                for (u32 i = 0; i < 420; i++)
                    if (inlineMap[i] == (s16)matchedRecord[match])
                    {
                        if (first < 0) first = (s32)i;
                        occurrences++;
                    }
                length += sprintf(report + length,
                    "inline_match%02lu grid=%lu record=%lu first=%ld count=%lu\n",
                    match, matchedGrid[match], matchedRecord[match],
                    (long)first, occurrences);
            }
            u16 positions[420] = {0};
            u16 ordered[420] = {0};
            u32 positionCount = 0, orderedCount = 0;
            for (u32 i = 0; i < 420; i++)
                for (u32 d = 0; d < desiredCount; d++)
                    if (inlineMap[i] == (s16)desiredRecords[d])
                    {
                        positions[positionCount++] = (u16)i;
                        break;
                    }
            for (u32 d = 0; d < desiredCount; d++)
                for (u32 i = 0; i < positionCount; i++)
                    if (inlineMap[positions[i]] == (s16)desiredRecords[d])
                    {
                        ordered[orderedCount++] = desiredRecords[d];
                        break;
                    }
            u32 inlineChanged = 0;
            if (positionCount > 1 && positionCount == orderedCount)
                for (u32 i = 0; i < positionCount; i++)
                    if (inlineMap[positions[i]] != (s16)ordered[i])
                    {
                        inlineMap[positions[i]] = (s16)ordered[i];
                        inlineChanged++;
                    }
            if (inlineChanged != 0)
            {
                svcFlushProcessDataCache(CUR_PROCESS_HANDLE,
                    localWindow, 0x1000);
                svcFlushProcessDataCache(home, indexHeaderPage, 0x1000);
            }
            length += sprintf(report + length,
                "inline_positions=%lu\ninline_ordered=%lu\n"
                "inline_changed=%lu\n", positionCount, orderedCount,
                inlineChanged);
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow, 0x1000);
        }
        length += sprintf(report + length,
            "index_header_result=%08lx\n", map);
        if (indirectMap >= 0x08000000 && indirectMap < 0x40000000)
        {
            const u32 indirectPage = indirectMap & ~0xFFF;
            map = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, localWindow,
                home, indirectPage, 0x1000, 0);
            if (R_SUCCEEDED(map))
            {
                volatile s16 *indices = (volatile s16 *)(localWindow +
                    (indirectMap & 0xFFF));
                for (u32 i = 0; i < 60; i += 10)
                    length += sprintf(report + length,
                        "indirect%02lu=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i,
                        indices[i + 0], indices[i + 1], indices[i + 2],
                        indices[i + 3], indices[i + 4], indices[i + 5],
                        indices[i + 6], indices[i + 7], indices[i + 8],
                        indices[i + 9]);
                for (u32 match = 0; match < matchedRecords; match++)
                {
                    s32 first = -1;
                    u32 occurrences = 0;
                    for (u32 i = 0; i < 420; i++)
                        if (indices[i] == (s16)matchedRecord[match])
                        {
                            if (first < 0) first = (s32)i;
                            occurrences++;
                        }
                    length += sprintf(report + length,
                        "indirect_match%02lu grid=%lu record=%lu first=%ld count=%lu\n",
                        match, matchedGrid[match], matchedRecord[match],
                        (long)first, occurrences);
                }
                u16 positions[420] = {0};
                u16 ordered[420] = {0};
                u32 positionCount = 0, orderedCount = 0;
                for (u32 i = 0; i < 420; i++)
                    for (u32 d = 0; d < desiredCount; d++)
                        if (indices[i] == (s16)desiredRecords[d])
                        {
                            positions[positionCount++] = (u16)i;
                            break;
                        }
                for (u32 d = 0; d < desiredCount; d++)
                    for (u32 i = 0; i < positionCount; i++)
                        if (indices[positions[i]] == (s16)desiredRecords[d])
                        {
                            ordered[orderedCount++] = desiredRecords[d];
                            break;
                        }
                u32 indirectChanged = 0;
                if (positionCount > 1 && positionCount == orderedCount)
                    for (u32 i = 0; i < positionCount; i++)
                        if (indices[positions[i]] != (s16)ordered[i])
                        {
                            indices[positions[i]] = (s16)ordered[i];
                            indirectChanged++;
                        }
                if (indirectChanged != 0)
                {
                    svcFlushProcessDataCache(CUR_PROCESS_HANDLE,
                        localWindow, 0x1000);
                    svcFlushProcessDataCache(home, indirectPage, 0x1000);
                }
                length += sprintf(report + length,
                    "indirect_positions=%lu\nindirect_ordered=%lu\n"
                    "indirect_changed=%lu\n", positionCount, orderedCount,
                    indirectChanged);
                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE,
                                        localWindow, 0x1000);
            }
            length += sprintf(report + length,
                "index_indirect_result=%08lx\n", map);
        }
    }
    IFile file = {0};
    if (R_SUCCEEDED(IFile_Open(&file, ARCHIVE_SDMC,
        fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/icon-model-v195.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        u64 written = 0;
        IFile_Write(&file, &written, report, (u32)length, FS_WRITE_FLUSH);
        IFile_SetSize(&file, (u64)length);
        IFile_Close(&file);
    }
    return iconOwner;
}

Result CthulhuHomeMenu_RunBackgroundSort(u16 selectedAlgorithm,
                                         bool foldersFirst,
                                         volatile u32 *commandChannel,
                                         u16 *algorithmOut,
                                         u32 *mutationsOut)
{
    g_cthulhuBackgroundSort = true;
    Handle home = 0;
    u32 pid = 0;
    Result res = OpenUsaHomeMenu(&home, &pid);
    Result lockResult = R_SUCCEEDED(res) ?
        svcControlProcess(home, PROCESSOP_SCHEDULE_THREADS, 1, 0) : res;
    WriteSortJournal("atomic-home-lock", lockResult);
    if (R_SUCCEEDED(lockResult))
    {
        svcSleepThread(20 * 1000 * 1000LL);
        res = ApplySdSort(selectedAlgorithm, true, foldersFirst,
                          algorithmOut, mutationsOut);
        u32 iconOwnerAddress = R_SUCCEEDED(res) ?
            ScanLiveIconClassV195(home) : 0;
        (void)iconOwnerAddress;
        u32 rebuildOwnerAddress = 0x003827E4;
        u32 publishOwnerAddress = 0x003827D8;
        u32 ownerMatches = 1;
        /* The object at 0x003827D8 is HOME's wrapper, not an SD-layout
           buffer. ApplySdSort already updates the confirmed serialized
           subobject at wrapper+0x0C. Let HOME's native methods interpret
           the wrapper instead of dereferencing its private fields here. */
        u32 wrapperWords[6] = {0};
        if (commandChannel != NULL)
            for (u32 i = 0; i < 6; i++) wrapperWords[i] = commandChannel[(s32)i - 6];
        Result unlockResult = svcControlProcess(
            home, PROCESSOP_SCHEDULE_THREADS, 0, 0);
        WriteSortJournal(R_SUCCEEDED(unlockResult) ?
                         "atomic-home-unlocked" : "atomic-home-unlock-failed",
                         unlockResult);
        if (R_FAILED(unlockResult)) res = unlockResult;
        if (R_SUCCEEDED(res) && commandChannel != NULL)
        {
            u32 request = commandChannel[0xCC / 4] + 1;
            if (request == 0) request = 1;
            commandChannel[0xD0 / 4] = rebuildOwnerAddress;
            commandChannel[0xE0 / 4] = publishOwnerAddress;
            /* V195 only requests the already-validated refresh callback after
               its membership-preserving live index-map permutation. */
            commandChannel[0x100 / 4] = iconOwnerAddress;
            commandChannel[0xCC / 4] = request;
            svcFlushProcessDataCache(CUR_PROCESS_HANDLE,
                                     (u32)commandChannel & ~0xFFF, 0x1000);
            svcFlushProcessDataCache(home, 0x00382000, 0x1000);
            u32 waits = 0;
            while (commandChannel[0xD4 / 4] != request && waits++ < 180)
                svcSleepThread(16 * 1000 * 1000LL);
            if (commandChannel[0xD4 / 4] != request)
                res = (Result)-78;
            char nativeReport[768];
            int nativeLength = sprintf(nativeReport,
                "Cthulhu HOME native rebuild\n"
                "sorter_version=" CTH_SORT_BUILD_VERSION "\n"
                "rebuild_owner=%08lx\npublish_owner=%08lx\n"
                "owner_matches=%lu\nrequest=%lu\nack=%lu\n"
                "wrapper_d8=%08lx\nwrapper_dc=%08lx\nwrapper_e0=%08lx\n"
                "serialized_raw_e4=%08lx\nserialized_grid_e8=%08lx\nwrapper_ec=%08lx\n"
                "calls=%lu\nnative_result=%08lx\nresult=%08lx\n",
                rebuildOwnerAddress, publishOwnerAddress,
                (unsigned long)ownerMatches, request,
                commandChannel[0xD4 / 4], wrapperWords[0], wrapperWords[1],
                wrapperWords[2], wrapperWords[3], wrapperWords[4],
                wrapperWords[5], commandChannel[0xD8 / 4],
                commandChannel[0xDC / 4], res);
            IFile nativeFile = {0};
            if (R_SUCCEEDED(IFile_Open(&nativeFile, ARCHIVE_SDMC,
                fsMakePath(PATH_EMPTY, ""),
                fsMakePath(PATH_ASCII,
                           "/3ds/Cthulhu/native-rebuild.txt"),
                FS_OPEN_CREATE | FS_OPEN_WRITE)))
            {
                u64 written = 0;
                IFile_Write(&nativeFile, &written, nativeReport,
                            nativeLength, FS_WRITE_FLUSH);
                IFile_SetSize(&nativeFile, nativeLength);
                IFile_Close(&nativeFile);
            }
            WriteSortJournal(R_SUCCEEDED(res) ?
                "home-native-rebuild-acked" : "home-native-rebuild-failed",
                res);
        }
    }
    else
        res = lockResult;
    if (home) svcCloseHandle(home);
    g_cthulhuBackgroundSort = false;
    return res;
}

void CthulhuHomeMenu_ApplySdSort(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_DrawString(10, 10, COLOR_TITLE,
                    "Apply Cthulhu sort v" CTH_SORT_BUILD_VERSION);
    Draw_DrawString(10, 35, COLOR_WHITE,
        "Uses the reusable title catalog in\n"
        "/3ds/Cthulhu/sort-request.bin.\n\n"
        "SD and Launcher backups are written first.\n"
        "NAND title icons remain fixed.\n\n"
        "X (top button): A-Z\n"
        "Y (left button): Z-A\n"
        "A: Catalog order    B: Cancel");
    Draw_FlushFramebuffer();
    Draw_Unlock();
    u32 input = 0;
    do input = waitInput();
    while (!(input & (KEY_A | KEY_B | KEY_X | KEY_Y)) && !menuShouldExit);
    if ((input & KEY_B) || menuShouldExit)
        return;

    ShowProgress("Validating sort request", 0, 7);
    u16 algorithm = 0;
    u32 mutations = 0;
    u16 selectedAlgorithm = input & KEY_X ? 1 : input & KEY_Y ? 2 : 0;
    Result res = ApplySdSort(selectedAlgorithm, false, true,
                             &algorithm, &mutations);
    WriteSortJournal("apply-returned", res);
    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Cthulhu SD sort result");
        if (R_SUCCEEDED(res))
        {
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Verified %lu positions (algorithm %u).\n\nRebooting now...",
                mutations, algorithm);
            Draw_FlushFramebuffer();
            Draw_Unlock();
            svcSleepThread(1500 * 1000 * 1000LL);
            menuLeave();
            WriteSortJournal("request-hardware-reboot", (Result)-1);
            APT_HardwareResetAsync();
            return;
        }
        if (R_FAILED(res))
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "No changes committed.\nResult: 0x%08lx\n\n"
                "See /3ds/Cthulhu/sort-transaction.txt\nPress B to return.", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void CthulhuHomeMenu_ArmFolderSort(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_DrawString(10, 10, COLOR_TITLE,
                    "Arm folder sort v" CTH_SORT_BUILD_VERSION);
    Draw_DrawString(10, 35, COLOR_WHITE,
        "Stages SD titles now and folder positions\n"
        "for the next normal power-off.\n\n"
        "After success: exit Rosalina, then power off\n"
        "normally without rearranging any icons.\n\n"
        "X (top): folders first, A-Z\n"
        "Y (left): folders last, Z-A\n"
        "B: Cancel");
    Draw_FlushFramebuffer();
    Draw_Unlock();
    u32 input = 0;
    do input = waitInput();
    while (!(input & (KEY_B | KEY_X | KEY_Y)) && !menuShouldExit);
    if ((input & KEY_B) || menuShouldExit) return;
    u16 algorithm = 0;
    u32 mutations = 0;
    u16 selected = input & KEY_X ? 1 : 2;
    Result res = ApplySdSort(selected, true, selected == 1,
                             &algorithm, &mutations);
    WriteSortJournal("arm-folder-sort-returned", res);
    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Folder sort staging result");
        if (R_SUCCEEDED(res))
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Plan armed (algorithm %u).\n\n"
                "Press B, exit Rosalina, then use a normal\n"
                "power-off. Do not rearrange icons first.", algorithm);
        else
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "No plan armed. Result: 0x%08lx\n\n"
                "See sort-journal.txt and sort-transaction.txt.\n"
                "Press B to return.", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

static Result ReadSdmcLayout(const char *path, u8 *destination)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                            fsMakePath(PATH_ASCII, path), FS_OPEN_READ);
    if (R_FAILED(res))
        return res;
    u64 size = 0, read = 0;
    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res) && size != CTH_SD_LAYOUT_SIZE)
        res = (Result)-40;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, destination, CTH_SD_LAYOUT_SIZE);
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && read != CTH_SD_LAYOUT_SIZE)
        res = (Result)-41;
    return res;
}

static Result WriteSdmcLayout(const char *path, const u8 *source)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                            fsMakePath(PATH_ASCII, path),
                            FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res))
        return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, source, CTH_SD_LAYOUT_SIZE, FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != CTH_SD_LAYOUT_SIZE)
        res = (Result)-42;
    if (R_SUCCEEDED(res))
        res = IFile_SetSize(&file, CTH_SD_LAYOUT_SIZE);
    IFile_Close(&file);
    return res;
}

static Result ReadSdmcLauncher(const char *path, u8 *destination)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                            fsMakePath(PATH_ASCII, path), FS_OPEN_READ);
    if (R_FAILED(res)) return res;
    u64 size = 0, read = 0;
    res = IFile_GetSize(&file, &size);
    if (R_SUCCEEDED(res) && size != CTH_LAUNCHER_SIZE) res = (Result)-49;
    if (R_SUCCEEDED(res))
        res = IFile_Read(&file, &read, destination, CTH_LAUNCHER_SIZE);
    IFile_Close(&file);
    if (R_SUCCEEDED(res) && read != CTH_LAUNCHER_SIZE) res = (Result)-50;
    return res;
}

static Result WriteSdmcLauncher(const char *path, const u8 *source)
{
    IFile file = {0};
    Result res = IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                            fsMakePath(PATH_ASCII, path),
                            FS_OPEN_CREATE | FS_OPEN_WRITE);
    if (R_FAILED(res)) return res;
    u64 written = 0;
    res = IFile_Write(&file, &written, source, CTH_LAUNCHER_SIZE, FS_WRITE_FLUSH);
    if (R_SUCCEEDED(res) && written != CTH_LAUNCHER_SIZE) res = (Result)-51;
    if (R_SUCCEEDED(res)) res = IFile_SetSize(&file, CTH_LAUNCHER_SIZE);
    IFile_Close(&file);
    return res;
}

static Result RestoreSdLayout(u32 *rawOut, u32 *processedOut)
{
    ShowProgress("Reading current SD layout", 0, 7);
    Result res = ReadSdSaveData();
    if (R_FAILED(res))
        return res;
    u16 currentUsed = 0;
    if (!ValidateLayout(g_sortRaw, false, &currentUsed))
        return (Result)-43;
    memcpy(g_sortOriginal, g_sortRaw, sizeof(g_sortOriginal));

    Handle process = 0;
    u32 pid = 0;
    ShowProgress("Discovering live SD buffers", 1, 7);
    if (R_FAILED(res = OpenUsaHomeMenu(&process, &pid)))
        return res;
    u32 rawAddress = 0, processedAddress = 0;
    MemInfo mem = {0};
    res = DiscoverSdRuntime(process, g_sortOriginal, &rawAddress,
                            &processedAddress, &mem);
    if (R_FAILED(res))
    {
        svcCloseHandle(process);
        return res;
    }
    res = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, process,
                                mem.base_addr, mem.size, 0);
    if (R_FAILED(res))
    {
        svcCloseHandle(process);
        return res;
    }
    u16 launcherUsed = 0, launcherRestoreUsed = 0;
    if (R_SUCCEEDED(res))
        memcpy(g_launcherOriginal, (const void *)CTH_NAND_RAW_ADDRESS,
               CTH_LAUNCHER_SIZE);
    if (R_SUCCEEDED(res) && !ValidateLayout(g_launcherOriginal, true, &launcherUsed))
        res = (Result)-52;
    ShowProgress("Validating recovery layout", 2, 7);
    if (R_SUCCEEDED(res))
        res = ReadSdmcLayout("/3ds/Cthulhu/pre-sort-SaveData.dat", g_sortRaw);
    u16 restoreUsed = 0;
    if (R_SUCCEEDED(res) &&
        (!ValidateLayout(g_sortRaw, false, &restoreUsed) || restoreUsed != currentUsed ||
         !BuildSdGridFromRaw(g_sortRaw, g_sortGrid)))
        res = (Result)-44;
    if (R_SUCCEEDED(res))
        res = ReadSdmcLauncher("/3ds/Cthulhu/pre-sort-Launcher.dat", g_launcherRaw);
    if (R_SUCCEEDED(res) &&
        (!ValidateLayout(g_launcherRaw, true, &launcherRestoreUsed) ||
         launcherRestoreUsed != launcherUsed))
        res = (Result)-53;

    ShowProgress("Backing up current layout", 3, 7);
    if (R_SUCCEEDED(res))
        res = WriteSdmcLayout("/3ds/Cthulhu/pre-restore-SaveData.dat", g_sortOriginal);
    if (R_SUCCEEDED(res))
        res = WriteSdmcLauncher("/3ds/Cthulhu/pre-restore-Launcher.dat",
                                g_launcherOriginal);
    ShowProgress("Committing recovery layout", 4, 7);
    if (R_SUCCEEDED(res))
    {
        memcpy(g_sortCommitted, g_sortRaw, sizeof(g_sortCommitted));
        res = WriteSdSaveData();
    }
    if (R_SUCCEEDED(res))
    {
        res = ReadSdSaveData();
        if (R_SUCCEEDED(res) &&
            memcmp(g_sortRaw, g_sortCommitted, sizeof(g_sortRaw)) != 0)
            res = (Result)-45;
        if (R_SUCCEEDED(res))
            res = SaveCommittedSnapshot();
    }
    ShowProgress("Updating live HOME Menu", 5, 7);
    if (R_SUCCEEDED(res))
    {
        memcpy((void *)rawAddress, g_sortRaw, sizeof(g_sortRaw));
        memcpy((void *)CTH_NAND_RAW_ADDRESS, g_launcherRaw, CTH_LAUNCHER_SIZE);
        memcpy((void *)processedAddress, g_sortGrid, sizeof(g_sortGrid));
        svcFlushProcessDataCache(process, rawAddress, sizeof(g_sortRaw));
        svcFlushProcessDataCache(process, processedAddress, sizeof(g_sortGrid));
        *rawOut = rawAddress;
        *processedOut = processedAddress;
    }
    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
    svcCloseHandle(process);
    ShowProgress("Writing recovery report", 6, 7);
    WriteSortTransactionReport(res, 0, restoreUsed, 0,
                               rawAddress, processedAddress);
    ShowProgress("Recovery transaction complete", 7, 7);
    return res;
}

void CthulhuHomeMenu_RestoreSdSort(void)
{
    Draw_Lock();
    Draw_ClearFramebuffer();
    Draw_DrawString(10, 10, COLOR_TITLE,
                    "Restore Cthulhu v" CTH_SORT_BUILD_VERSION);
    Draw_DrawString(10, 35, COLOR_WHITE,
        "Restore pre-sort SD and Launcher layouts?\n\n"
        "The current layouts will also be backed up.\n\n"
        "A verified success will reboot immediately.\n\n"
        "Press A to continue or B to cancel.");
    Draw_FlushFramebuffer();
    Draw_Unlock();
    u32 input = 0;
    do input = waitInput();
    while (!(input & (KEY_A | KEY_B)) && !menuShouldExit);
    if (!(input & KEY_A) || menuShouldExit)
        return;

    u32 rawAddress = 0, processedAddress = 0;
    Result res = RestoreSdLayout(&rawAddress, &processedAddress);
    WriteSortJournal("restore-returned", res);
    do
    {
        Draw_Lock();
        Draw_ClearFramebuffer();
        Draw_DrawString(10, 10, COLOR_TITLE, "Cthulhu restore result");
        if (R_SUCCEEDED(res))
        {
            Draw_DrawFormattedString(10, 35, COLOR_WHITE,
                "Recovery verified.\nraw=%08lx processed=%08lx\n\nRebooting now...",
                (unsigned long)rawAddress, (unsigned long)processedAddress);
            Draw_FlushFramebuffer();
            Draw_Unlock();
            svcSleepThread(1500 * 1000 * 1000LL);
            menuLeave();
            WriteSortJournal("request-restore-hardware-reboot", (Result)-1);
            APT_HardwareResetAsync();
            return;
        }
        Draw_DrawFormattedString(10, 35, COLOR_WHITE,
            "No recovery committed.\nResult: 0x%08lx\n\nPress B to return.", res);
        Draw_FlushFramebuffer();
        Draw_Unlock();
    }
    while (!(waitInput() & KEY_B) && !menuShouldExit);
}

void CthulhuHomeMenu_WritePersistenceAudit(void)
{
    static u8 expected[CTH_SD_LAYOUT_SIZE];
    char report[4096];
    int length = sprintf(report,
        "Cthulhu HOME persistence audit v1\n"
        "sorter_version=" CTH_SORT_BUILD_VERSION "\n");

    Result currentResult = ReadSdSaveData();
    u16 currentUsed = 0;
    bool currentValid = R_SUCCEEDED(currentResult) &&
                        ValidateLayout(g_sortRaw, false, &currentUsed);
    Result expectedResult = ReadSdmcLayout(
        "/3ds/Cthulhu/post-sort-SaveData.dat", expected);
    u16 expectedUsed = 0;
    bool expectedValid = R_SUCCEEDED(expectedResult) &&
                         ValidateLayout(expected, false, &expectedUsed);
    u32 byteMismatches = 0, positionMismatches = 0;
    if (currentValid && expectedValid)
    {
        for (u32 i = 0; i < CTH_SD_LAYOUT_SIZE; i++)
            if (g_sortRaw[i] != expected[i]) byteMismatches++;
        for (u32 slot = 0; slot < CTH_LAYOUT_SLOTS; slot++)
            if (ReadS16(g_sortRaw, 0xCB0 + slot * 2) !=
                ReadS16(expected, 0xCB0 + slot * 2))
                positionMismatches++;
    }
    length += sprintf(report + length,
        "\n[EXTDATA]\ncurrent_result=%08lx current_valid=%u current_titles=%u "
        "current_crc=%08lx\nexpected_result=%08lx expected_valid=%u "
        "expected_titles=%u expected_crc=%08lx\n"
        "byte_mismatches=%lu position_mismatches=%lu\n",
        currentResult, currentValid, currentUsed,
        currentValid ? CthCrc32(g_sortRaw, sizeof(g_sortRaw)) : 0,
        expectedResult, expectedValid, expectedUsed,
        expectedValid ? CthCrc32(expected, sizeof(expected)) : 0,
        (unsigned long)byteMismatches, (unsigned long)positionMismatches);
    if (currentValid)
        WriteSdmcLayout("/3ds/Cthulhu/boot-current-SaveData.dat", g_sortRaw);

    Handle process = 0;
    u32 pid = 0, rawAddress = 0, processedAddress = 0;
    MemInfo mem = {0};
    Result liveResult = currentValid ? OpenUsaHomeMenu(&process, &pid) : (Result)-1;
    if (R_SUCCEEDED(liveResult))
        liveResult = DiscoverSdRuntime(process, g_sortRaw, &rawAddress,
                                       &processedAddress, &mem);
    u32 rawLiveMismatches = 0, gridLiveMismatches = 0;
    u32 launcherAddress = 0, launcherMatches = 0;
    MemInfo launcherMem = {0};
    Result launcherResult = (Result)-1;
    bool mapped = false, launcherMapped = false;
    if (R_SUCCEEDED(liveResult))
    {
        u32 preferred = rawAddress - CTH_LAUNCHER_TO_SD_DELTA;
        launcherResult = DiscoverLauncherRuntime(process, preferred,
            &launcherAddress, &launcherMem, &launcherMatches);
    }
    if (R_SUCCEEDED(liveResult))
    {
        liveResult = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr,
                                           process, mem.base_addr, mem.size, 0);
        mapped = R_SUCCEEDED(liveResult);
    }
    if (mapped)
    {
        BuildSdGridFromRaw(g_sortRaw, g_sortGrid);
        for (u32 i = 0; i < sizeof(g_sortRaw); i++)
            if (((const u8 *)rawAddress)[i] != g_sortRaw[i]) rawLiveMismatches++;
        for (u32 i = 0; i < sizeof(g_sortGrid); i++)
            if (((const u8 *)processedAddress)[i] != ((const u8 *)g_sortGrid)[i])
                gridLiveMismatches++;
        if (R_SUCCEEDED(launcherResult) && launcherMem.base_addr != mem.base_addr)
        {
            launcherResult = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE,
                launcherMem.base_addr, process, launcherMem.base_addr,
                launcherMem.size, 0);
            launcherMapped = R_SUCCEEDED(launcherResult);
        }
        if (R_SUCCEEDED(launcherResult))
        {
            const u8 *launcher = (const u8 *)launcherAddress;
            length += sprintf(report + length, "\n[FOLDERS]\n");
            for (u32 folder = 0; folder < CTH_FOLDER_COUNT; folder++)
            {
                u32 number = 0;
                memcpy(&number, launcher + CTH_FOLDER_NUMBER_OFFSET + folder * 4, 4);
                if (number != 0)
                    length += sprintf(report + length,
                        "id=%lu number=%lu position=%d\n",
                        (unsigned long)folder, (unsigned long)number,
                        ReadS16(launcher, CTH_FOLDER_POSITION_OFFSET + folder * 2));
            }
        }
    }
    length += sprintf(report + length,
        "\n[LIVE]\npid=%lu result=%08lx raw=%08lx processed=%08lx\n"
        "raw_byte_mismatches=%lu processed_byte_mismatches=%lu\n"
        "launcher_result=%08lx launcher=%08lx launcher_matches=%lu\n",
        (unsigned long)pid, liveResult, rawAddress, processedAddress,
        (unsigned long)rawLiveMismatches, (unsigned long)gridLiveMismatches,
        launcherResult, launcherAddress, (unsigned long)launcherMatches);
    if (launcherMapped)
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, launcherMem.base_addr,
                                launcherMem.size);
    if (mapped)
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, mem.base_addr, mem.size);
    if (process) svcCloseHandle(process);

    IFile file = {0};
    if (R_SUCCEEDED(IFile_Open(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
        fsMakePath(PATH_ASCII, "/3ds/Cthulhu/persistence-audit.txt"),
        FS_OPEN_CREATE | FS_OPEN_WRITE)))
    {
        u64 written = 0;
        IFile_Write(&file, &written, report, length, FS_WRITE_FLUSH);
        IFile_SetSize(&file, length);
        IFile_Close(&file);
    }
}
