#pragma once

void CthulhuHomeMenu_DumpLayoutReport(void);
void CthulhuHomeMenu_AttachRuntime(void);
void CthulhuHomeMenu_StartFramework(void);
void CthulhuHomeMenu_CheckStaticHook(void);
void CthulhuHomeMenu_ApplySdSort(void);
void CthulhuHomeMenu_RestoreSdSort(void);
void CthulhuHomeMenu_ArmFolderSort(void);
void CthulhuHomeMenu_RefreshCatalog(void);
void CthulhuHomeMenu_Search(void);
void CthulhuHomeMenu_HandleShutdownNotification(u32 notificationId);
void CthulhuHomeMenu_WritePersistenceAudit(void);
Result CthulhuHomeMenu_RunBackgroundSort(u16 selectedAlgorithm,
                                         bool foldersFirst,
                                         volatile u32 *commandChannel,
                                         u16 *algorithmOut,
                                         u32 *mutationsOut);
