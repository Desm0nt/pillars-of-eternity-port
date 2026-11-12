/* F2: implementation table of the NintendoSDK plugin wrappers (see f2_nn.h for the ABI; tools/nn/nn_abi.tsv lists
 * every wrapper).  off = address of the wrapper in the Switch main, build 68976c70...  Wrappers not listed here keep
 * the constant-return stub generated from work/f2/data/nn_pinvoke.tsv (which logs its name once). */
#include <pthread.h>
#include "f2_nn.h"

#define D(fn) extern void fn(void)
/* account */
D(nn_account_Initialize); D(nn_account_GetUserCount); D(nn_account_GetUserExistence); D(nn_account_ListUsers);
D(nn_account_GetLastOpenedUser); D(nn_account_GetNickname); D(nn_account_LoadProfileImage); D(nn_account_OpenUser);
D(nn_account_OpenPreselectedUser); D(nn_account_CloseUser); D(nn_account_GetUserId); D(nn_account_StoreSaveDataThumbnailImage);
D(nn_account_DeleteSaveDataThumbnailImage); D(nn_account_nsa_EnsureAvailable); D(nn_account_nsa_IsAvailable);
D(nn_account_nsa_GetId); D(nn_account_nsa_EnsureIdTokenCacheAsync); D(nn_account_nsa_LoadIdTokenCache);
D(nn_account_AsyncContext_ctor); D(nn_account_AsyncContext_Dispose); D(nn_account_AsyncContext_Cancel);
D(nn_account_AsyncContext_HasDone); D(nn_account_AsyncContext_GetResult); D(nn_account_ShowUserSelector); D(nn_account_ShowUserCreator);
/* aoc / err / oe / Nn */
D(nn_aoc_CountAddOnContent); D(nn_aoc_ListAddOnContent); D(nn_err_ShowError); D(nn_err_ShowErrorCode);
D(nn_err_ShowUnacceptableApplicationVersion); D(nn_Abort); D(nn_oe_GetDesiredLanguage);
/* fs */
D(nn_fs_QueryMountAddOnContentCacheSize); D(nn_fs_MountAddOnContent); D(nn_fs_ReadDirectory); D(nn_fs_GetDirectoryEntryCount);
D(nn_fs_CloseDirectory); D(nn_fs_ReadFile); D(nn_fs_ReadFileOut); D(nn_fs_WriteFile); D(nn_fs_FlushFile); D(nn_fs_SetFileSize);
D(nn_fs_GetFileSize); D(nn_fs_GetFileOpenMode); D(nn_fs_CloseFile); D(nn_fs_CreateFile); D(nn_fs_DeleteFile);
D(nn_fs_CreateDirectory); D(nn_fs_DeleteDirectory); D(nn_fs_DeleteDirectoryRecursively); D(nn_fs_CleanDirectoryRecursively);
D(nn_fs_Rename); D(nn_fs_GetEntryType); D(nn_fs_OpenFile); D(nn_fs_OpenDirectory); D(nn_fs_Unmount);
D(nn_fs_QueryMountRomCacheSize); D(nn_fs_MountRom); D(nn_fs_EnsureSaveData); D(nn_fs_MountSaveData);
D(nn_fs_MountSaveDataReadOnly); D(nn_fs_IsSaveDataExisting); D(nn_fs_Commit);
/* hid */
D(nn_hid_InitializeNpad); D(nn_hid_SetSupportedStyleSet); D(nn_hid_GetSupportedStyleSet); D(nn_hid_SetSupportedIdType);
D(nn_hid_BindStyleSetUpdateEvent); D(nn_hid_IsStyleSetUpdated); D(nn_hid_DestroyStyleSetUpdateEvent); D(nn_hid_GetStyleSet);
D(nn_hid_Disconnect); D(nn_hid_GetPlayerLedPattern); D(nn_hid_GetControllerColor1); D(nn_hid_GetControllerColor2);
D(nn_hid_GetNpadState); D(nn_hid_GetNpadStates); D(nn_hid_GetJoyAssignment); D(nn_hid_noop_id); D(nn_hid_MergeSingleAsDual);
D(nn_hid_SetHoldType); D(nn_hid_GetHoldType); D(nn_hid_SetHandheldActivationMode); D(nn_hid_GetHandheldActivationMode);
D(nn_hid_SetCommunicationMode); D(nn_hid_GetCommunicationMode); D(nn_hid_InitializeTouchScreen); D(nn_hid_GetTouchScreenState);
D(nn_hid_GetTouchScreenStates); D(nn_hid_GetVibrationDeviceHandles); D(nn_hid_GetVibrationDeviceInfo);
D(nn_hid_InitializeVibrationDevice); D(nn_hid_SendVibrationValue); D(nn_hid_GetActualVibrationValue); D(nn_hid_IsVibrationPermitted);
D(nn_hid_ShowControllerSupport); D(nn_hid_ShowControllerSupportInfo);

#define I(off, fn, name) { off, (void *)fn, name }
const f2_nn_impl_t f2_nn_impls[] = {
    I(0x2496b00, nn_account_GetUserCount, "nn.account.Account.GetUserCount"),
    I(0x2496b20, nn_account_GetUserExistence, "nn.account.Account.GetUserExistence"),
    I(0x2496b50, nn_account_ListUsers, "nn.account.Account.ListAllUsers"),
    I(0x2496b70, nn_account_ListUsers, "nn.account.Account.ListOpenUsers"),
    I(0x2496b90, nn_account_GetLastOpenedUser, "nn.account.Account.GetLastOpenedUser"),
    I(0x2496bb0, nn_account_GetNickname, "nn.account.Account.GetNickname"),
    I(0x2496be0, nn_account_LoadProfileImage, "nn.account.Account.LoadProfileImage"),
    I(0x2496c10, nn_account_Initialize, "nn.account.Account.Initialize"),
    I(0x2496c20, nn_account_OpenUser, "nn.account.Account.OpenUser"),
    I(0x2496c50, nn_account_OpenPreselectedUser, "nn.account.Account.OpenPreselectedUser"),
    I(0x2496c70, nn_account_CloseUser, "nn.account.Account.CloseUser"),
    I(0x2496c80, nn_account_GetUserId, "nn.account.Account.GetUserId"),
    I(0x2496ca0, nn_account_StoreSaveDataThumbnailImage, "nn.account.Account.StoreSaveDataThumbnailImage"),
    I(0x2496d10, nn_account_DeleteSaveDataThumbnailImage, "nn.account.Account.DeleteSaveDataThumbnailImage"),
    I(0x2496d40, nn_account_nsa_EnsureAvailable, "nn.account.NetworkServiceAccount.EnsureAvailable"),
    I(0x2496d60, nn_account_nsa_IsAvailable, "nn.account.NetworkServiceAccount.IsAvailable"),
    I(0x2496d80, nn_account_nsa_GetId, "nn.account.NetworkServiceAccount.GetId"),
    I(0x2496da0, nn_account_nsa_EnsureIdTokenCacheAsync, "nn.account.NetworkServiceAccount.EnsurIdTokenCacheAsync"),
    I(0x2496dc0, nn_account_nsa_LoadIdTokenCache, "nn.account.NetworkServiceAccount.LoadIdTokenCache"),
    I(0x2496de0, nn_account_AsyncContext_ctor, "nn.account.AsyncContext.ctor"),
    I(0x2496e10, nn_account_AsyncContext_Dispose, "nn.account.AsyncContext.Dispose"),
    I(0x2496e30, nn_account_AsyncContext_Cancel, "nn.account.AsyncContext.Cancel"),
    I(0x2496e50, nn_account_AsyncContext_HasDone, "nn.account.AsyncContext.HasDone"),
    I(0x2496e70, nn_account_AsyncContext_GetResult, "nn.account.AsyncContext.GetResult"),
    I(0x2496e90, nn_account_ShowUserSelector, "nn.account.Account.ShowUserSelector(settings)"),
    I(0x2496eb0, nn_account_ShowUserSelector, "nn.account.Account.ShowUserSelector"),
    I(0x2496ed0, nn_account_ShowUserCreator, "nn.account.Account.ShowUserCreator"),
    I(0x2496ef0, nn_aoc_CountAddOnContent, "nn.aoc.Aoc.CountAddOnContent"),
    I(0x2496f00, nn_aoc_ListAddOnContent, "nn.aoc.Aoc.ListAddOnContent"),
    I(0x2496f70, nn_err_ShowError, "nn.err.Error.Show(Result)"),
    I(0x2496f80, nn_err_ShowErrorCode, "nn.err.Error.Show(ErrorCode)"),
    I(0x2496f90, nn_err_ShowUnacceptableApplicationVersion, "nn.err.Error.ShowUnacceptableApplicationVersion"),
    I(0x2496fe0, nn_fs_QueryMountAddOnContentCacheSize, "nn.fs.AddOnContent.QueryMountCacheSize"),
    I(0x2497000, nn_fs_MountAddOnContent, "nn.fs.AddOnContent.Mount"),
    I(0x2497020, nn_fs_ReadDirectory, "nn.fs.Directory.Read"),
    I(0x2497040, nn_fs_GetDirectoryEntryCount, "nn.fs.Directory.GetEntryCount"),
    I(0x2497060, nn_fs_CloseDirectory, "nn.fs.Directory.Close"),
    I(0x2497070, nn_fs_ReadFile, "nn.fs.File.Read(option)"),
    I(0x24970a0, nn_fs_ReadFile, "nn.fs.File.Read"),
    I(0x24970c0, nn_fs_ReadFileOut, "nn.fs.File.Read(out, option)"),
    I(0x24970f0, nn_fs_ReadFileOut, "nn.fs.File.Read(out)"),
    I(0x2497110, nn_fs_WriteFile, "nn.fs.File.Write"),
    I(0x2497140, nn_fs_FlushFile, "nn.fs.File.Flush"),
    I(0x2497160, nn_fs_SetFileSize, "nn.fs.File.SetSize"),
    I(0x2497180, nn_fs_GetFileSize, "nn.fs.File.GetSize"),
    I(0x24971a0, nn_fs_GetFileOpenMode, "nn.fs.File.GetOpenMode"),
    I(0x24971b0, nn_fs_CloseFile, "nn.fs.File.Close"),
    I(0x24971c0, nn_fs_CreateFile, "nn.fs.File.Create"),
    I(0x24971e0, nn_fs_DeleteFile, "nn.fs.File.Delete"),
    I(0x2497200, nn_fs_CreateDirectory, "nn.fs.Directory.Create"),
    I(0x2497220, nn_fs_DeleteDirectory, "nn.fs.Directory.Delete"),
    I(0x2497240, nn_fs_DeleteDirectoryRecursively, "nn.fs.Directory.DeleteRecursively"),
    I(0x2497260, nn_fs_CleanDirectoryRecursively, "nn.fs.Directory.CleanRecursively"),
    I(0x2497280, nn_fs_Rename, "nn.fs.File.Rename"),
    I(0x24972a0, nn_fs_Rename, "nn.fs.Directory.Rename"),
    I(0x24972c0, nn_fs_GetEntryType, "nn.fs.FileSystem.GetEntryType"),
    I(0x24972e0, nn_fs_OpenFile, "nn.fs.File.Open"),
    I(0x2497300, nn_fs_OpenDirectory, "nn.fs.Directory.Open"),
    I(0x2497320, nn_fs_Unmount, "nn.fs.FileSystem.Unmount"),
    I(0x2497330, nn_fs_QueryMountRomCacheSize, "nn.fs.Rom.QueryMountRomCacheSize"),
    I(0x2497350, nn_fs_MountRom, "nn.fs.Rom.MountRom"),
    I(0x2497370, nn_fs_EnsureSaveData, "nn.fs.SaveData.Ensure"),
    I(0x24973a0, nn_fs_MountSaveData, "nn.fs.SaveData.Mount"),
    I(0x24973d0, nn_fs_MountSaveDataReadOnly, "nn.fs.SaveData.MountSaveDataReadOnly"),
    I(0x2497400, nn_fs_IsSaveDataExisting, "nn.fs.SaveData.IsExisting(Uid)"),
    I(0x2497430, nn_fs_IsSaveDataExisting, "nn.fs.SaveData.IsExisting(applicationId, Uid)"),
    I(0x2497460, nn_fs_Commit, "nn.fs.FileSystem.Commit"),
    I(0x24974a0, nn_hid_InitializeNpad, "nn.hid.Npad.Initialize"),
    I(0x24974b0, nn_hid_SetSupportedStyleSet, "nn.hid.Npad.SetSupportedStyleSet"),
    I(0x24974c0, nn_hid_GetSupportedStyleSet, "nn.hid.Npad.GetSupportedStyleSet"),
    I(0x24974e0, nn_hid_SetSupportedIdType, "nn.hid.Npad.SetSupportedIdType"),
    I(0x24974f0, nn_hid_BindStyleSetUpdateEvent, "nn.hid.Npad.BindStyleSetUpdateEvent"),
    I(0x2497540, nn_hid_IsStyleSetUpdated, "nn.hid.Npad.IsStyleSetUpdated"),
    I(0x2497560, nn_hid_DestroyStyleSetUpdateEvent, "nn.hid.Npad.DestroyStyleSetUpdateEvent"),
    I(0x2497580, nn_hid_GetStyleSet, "nn.hid.Npad.GetStyleSet"),
    I(0x24975b0, nn_hid_Disconnect, "nn.hid.Npad.Disconnect"),
    I(0x24975e0, nn_hid_GetPlayerLedPattern, "nn.hid.Npad.GetPlayerLedPattern"),
    I(0x2497610, nn_hid_GetControllerColor1, "nn.hid.Npad.GetControllerColor"),
    I(0x2497640, nn_hid_GetControllerColor2, "nn.hid.Npad.GetControllerColor(left, right)"),
    I(0x2497670, nn_hid_GetNpadState, "nn.hid.NpadFullKey.GetState"),
    I(0x24976a0, nn_hid_GetNpadStates, "nn.hid.NpadFullKey.GetStates"),
    I(0x24976d0, nn_hid_GetNpadState, "nn.hid.NpadHandheld.GetState"),
    I(0x2497700, nn_hid_GetNpadStates, "nn.hid.NpadHandheld.GetStates"),
    I(0x2497730, nn_hid_GetJoyAssignment, "nn.hid.NpadJoy.GetAssignment"),
    I(0x2497760, nn_hid_noop_id, "nn.hid.NpadJoy.SetAssignmentModeSingle"),
    I(0x2497790, nn_hid_noop_id, "nn.hid.NpadJoy.SetAssignmentModeSingle(type)"),
    I(0x24977f0, nn_hid_noop_id, "nn.hid.NpadJoy.SetAssignmentModeDual"),
    I(0x2497820, nn_hid_MergeSingleAsDual, "nn.hid.NpadJoy.MergeSingleAsDual"),
    I(0x2497880, nn_hid_SetHoldType, "nn.hid.NpadJoy.SetHoldType"),
    I(0x2497890, nn_hid_GetHoldType, "nn.hid.NpadJoy.GetHoldType"),
    I(0x24978c0, nn_hid_SetHandheldActivationMode, "nn.hid.NpadJoy.SetHandheldActivationMode"),
    I(0x24978d0, nn_hid_GetHandheldActivationMode, "nn.hid.NpadJoy.GetHandheldActivationMode"),
    I(0x24978e0, nn_hid_SetCommunicationMode, "nn.hid.NpadJoy.SetCommunicationMode"),
    I(0x24978f0, nn_hid_GetCommunicationMode, "nn.hid.NpadJoy.GetCommunicationMode"),
    I(0x2497900, nn_hid_GetNpadState, "nn.hid.NpadJoyDual.GetState"),
    I(0x2497930, nn_hid_GetNpadStates, "nn.hid.NpadJoyDual.GetStates"),
    I(0x2497960, nn_hid_GetNpadState, "nn.hid.NpadJoyLeft.GetState"),
    I(0x2497990, nn_hid_GetNpadStates, "nn.hid.NpadJoyLeft.GetStates"),
    I(0x24979c0, nn_hid_GetNpadState, "nn.hid.NpadJoyRight.GetState"),
    I(0x24979f0, nn_hid_GetNpadStates, "nn.hid.NpadJoyRight.GetStates"),
    I(0x2497cf0, nn_hid_InitializeTouchScreen, "nn.hid.TouchScreen.Initialize"),
#define TS(i) I(0x2497d00 + 0x10 * (i), nn_hid_GetTouchScreenState, "nn.hid.TouchScreen.GetState"), \
              I(0x2497e00 + 0x10 * (i), nn_hid_GetTouchScreenStates, "nn.hid.TouchScreen.GetStates")
    TS(0), TS(1), TS(2), TS(3), TS(4), TS(5), TS(6), TS(7), TS(8), TS(9), TS(10), TS(11), TS(12), TS(13), TS(14), TS(15),
    I(0x2497f00, nn_hid_GetVibrationDeviceHandles, "nn.hid.Vibration.GetDeviceHandles"),
    I(0x2497f30, nn_hid_GetVibrationDeviceInfo, "nn.hid.Vibration.GetDeviceInfo"),
    I(0x2497f60, nn_hid_InitializeVibrationDevice, "nn.hid.Vibration.InitializeDevice"),
    I(0x2497f90, nn_hid_SendVibrationValue, "nn.hid.Vibration.SendValue"),
    I(0x2497fc0, nn_hid_GetActualVibrationValue, "nn.hid.Vibration.GetActualValue"),
    I(0x2497ff0, nn_hid_IsVibrationPermitted, "nn.hid.Vibration.IsPermitted"),
    I(0x2498000, nn_hid_ShowControllerSupport, "nn.hid.ControllerSupport.Show"),
    I(0x2498020, nn_hid_ShowControllerSupportInfo, "nn.hid.ControllerSupport.Show(resultInfo)"),
    I(0x24983f0, nn_Abort, "nn.Nn.Abort"),
    I(0x2498410, nn_oe_GetDesiredLanguage, "nn.oe.Language._GetDesired"),
};
const unsigned f2_nn_impl_count = sizeof(f2_nn_impls) / sizeof(f2_nn_impls[0]);

static pthread_once_t s_once = PTHREAD_ONCE_INIT;
static void init_once(void)
{
    f2_nn_config_load();
    f2_nn_fs_init();
    f2_log("F2: nn layer ready: %u of the 192 plugin wrappers implemented (the rest return constants)", f2_nn_impl_count);
}
/* idempotent; called by the loader hook f2_nn_init() if the boot layer wires it, and by f2_register_switch_icalls() */
void f2_nn_ensure_init(void) { pthread_once(&s_once, init_once); }
void f2_nn_init(void) { f2_nn_ensure_init(); }
