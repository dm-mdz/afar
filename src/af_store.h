/**
 ******************************************************************************
 * @file    af_store.h
 * @author  Afar_C
 * @version 1.0.0
 * @brief   基础层唯一 config.json / state.json 内存与原子落盘所有者（DD-01 §2.2）。
 ******************************************************************************
 */
#ifndef AF_STORE_H
#define AF_STORE_H

#include <dm/type.h>
#include "plat/af_plat.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define AF_IPAD_MAP_MAX 16u
#define AF_DISP_MODES_SAVE 16u

typedef struct af_config
{
  char acIpadName[65];
  bool_t bAutoConnect;
  char acScale[16];
  uint32 au32IpadPixels[2];
  bool_t bMirror;
  uint16 u16Port;
  uint32 u32ExtraCnt;
  uint32 aau32Extra[16][2];
} af_config_st;

typedef struct af_orig_ss
{
  bool_t bSaved;
  bool_t bKnown;
  int iSec;
} af_orig_ss_st;

typedef struct af_ipad_screen_entry
{
  char acName[65];
  uint32 au32Px[2];
} af_ipad_screen_entry_st;

typedef struct af_ipad_sizes_entry
{
  char acName[65];
  uint32 aau32Sz[3][2];
} af_ipad_sizes_entry_st;

typedef struct af_ipad_scale_entry
{
  char acName[65];
  char acScale[16];
} af_ipad_scale_entry_st;

typedef struct af_disp_mode_entry
{
  char acKey[48];
  uint32 au32Mode[5];
} af_disp_mode_entry_st;

typedef struct af_state
{
  char acName[65];
  char acScale[16];
  char acLastIpad[65];
  bool_t bHasKeyboard;
  af_bt_device_st stKeyboard;
  bool_t bKeyboardAuto;
  bool_t bKeyboardTaken;
  bool_t bUserOff;
  bool_t bNoBootConnect;
  bool_t bSetupDone;
  char acPlan[16];
  bool_t bHasDeskMode;
  uint32 au32DeskMode[4];
  uint32 u32ScreenCnt;
  af_ipad_screen_entry_st astScreens[AF_IPAD_MAP_MAX];
  uint32 u32SizesCnt;
  af_ipad_sizes_entry_st astSizes[AF_IPAD_MAP_MAX];
  uint32 u32ScaleCnt;
  af_ipad_scale_entry_st astScales[AF_IPAD_MAP_MAX];
  uint32 u32DispModeCnt;
  af_disp_mode_entry_st astDispModes[AF_DISP_MODES_SAVE];
  af_orig_ss_st stOrigSs;
  bool_t bAntiUnplug;
} af_state_st;

int af_store_load(dm_in const char *pcDataDir);
int af_store_save_sync(dm_in const char *pcDataDir);
const af_config_st *af_store_cfg(void);
af_config_st *af_store_cfg_mut(void);
af_state_st *af_store_state_mut(void);
const af_state_st *af_store_state(void);
int af_store_write_atomic(dm_in const char *pcPath, dm_in const void *pvData, dm_in size_t uLen);

typedef void (*af_store_saved_cb)(dm_in int iStatus, dm_io void *pvCtx);
int af_store_save_async(dm_in af_store_saved_cb pfOnDone, dm_io void *pvCtx);
int af_store_save_cfg_async(dm_in af_store_saved_cb pfOnDone, dm_io void *pvCtx);

#ifdef __cplusplus
}
#endif

#endif /* AF_STORE_H */
