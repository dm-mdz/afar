/**
 ******************************************************************************
 * @file    af_plat_bt.m
 * @author  Afar_C
 * @version 1.0.0
 * @brief   macOS 经典蓝牙 IOBluetooth 列表、连断与配对封装（DD-01/04）。
 ******************************************************************************
 */

#include <dm.h>
#include <dm/link.h>
#include "plat/af_plat.h"
#include "plat/af_objc_undef.h"

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include <stdio.h>
#include <string.h>

@interface AFBtPairDelegate : NSObject <IOBluetoothDeviceInquiryDelegate, IOBluetoothDevicePairDelegate>
@property (nonatomic, assign) uint cbLinkID;
@property (nonatomic, copy) NSString *pairAddr;
@end

@implementation AFBtPairDelegate

- (void)postMsg:(const af_bt_pair_cb_msg_st *)pstMsg
{
  if (0u != self.cbLinkID)
  {
    (void)dm_queue_post_async(self.cbLinkID, pstMsg, (uint32)sizeof(*pstMsg));
  }
}

- (void)fillAddr:(af_bt_pair_cb_msg_st *)pstMsg
{
  if (nil != self.pairAddr)
  {
    snprintf(pstMsg->acAddr, sizeof(pstMsg->acAddr), "%s", [self.pairAddr UTF8String] ?: "");
  }
}

- (void)deviceInquiryDeviceFound:(IOBluetoothDeviceInquiry *)oSender
                          device:(IOBluetoothDevice *)oDev
{
  (void)oSender;
  if (nil == oDev) { return; }
  NSString *oAddr = [oDev addressString];
  NSString *oName = [oDev nameOrAddress];
  if (nil == oAddr) { return; }
  af_bt_pair_cb_msg_st stMsg;
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.eStage = AF_BT_PAIR_SEARCHING;
  stMsg.bHasDev = DM_TRUE;
  snprintf(stMsg.stDev.acAddr, sizeof(stMsg.stDev.acAddr), "%s", [oAddr UTF8String] ?: "");
  snprintf(stMsg.stDev.acName, sizeof(stMsg.stDev.acName), "%s",
           (nil != oName) ? ([oName UTF8String] ?: "") : "");
  stMsg.stDev.bConnected = [oDev isConnected] ? DM_TRUE : DM_FALSE;
  stMsg.stDev.u32Major = (uint32)[oDev deviceClassMajor];
  stMsg.stDev.bBle = (0u == stMsg.stDev.u32Major) ? DM_TRUE : DM_FALSE;
  [self postMsg:&stMsg];
}

- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry *)oSender
                        error:(IOReturn)iError
                      aborted:(BOOL)bAborted
{
  (void)oSender;
  af_bt_pair_cb_msg_st stMsg;
  memset(&stMsg, 0, sizeof(stMsg));
  if ((kIOReturnSuccess == iError) || bAborted)
  {
    stMsg.eStage = AF_BT_PAIR_SEARCHED;
  }
  else
  {
    stMsg.eStage = AF_BT_PAIR_FAILED;
    snprintf(stMsg.acMsg, sizeof(stMsg.acMsg), "Bluetooth inquiry error 0x%x", (uint)iError);
  }
  [self postMsg:&stMsg];
}

- (void)devicePairingStarted:(id)oSender
{
  (void)oSender;
  af_bt_pair_cb_msg_st stMsg;
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.eStage = AF_BT_PAIR_PAIRING;
  [self fillAddr:&stMsg];
  [self postMsg:&stMsg];
}

- (void)devicePairingUserPasskeyNotification:(id)oSender passkey:(BluetoothPasskey)u32Key
{
  (void)oSender;
  af_bt_pair_cb_msg_st stMsg;
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.eStage = AF_BT_PAIR_PAIRING;
  [self fillAddr:&stMsg];
  snprintf(stMsg.acCode, sizeof(stMsg.acCode), "%06u", (uint)u32Key);
  [self postMsg:&stMsg];
}

- (void)devicePairingUserConfirmationRequest:(id)oSender
                                numericValue:(BluetoothNumericValue)u32Val
{
  af_bt_pair_cb_msg_st stMsg;
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.eStage = AF_BT_PAIR_PAIRING;
  [self fillAddr:&stMsg];
  snprintf(stMsg.acCode, sizeof(stMsg.acCode), "%06u", (uint)u32Val);
  [self postMsg:&stMsg];
  [(IOBluetoothDevicePair *)oSender replyUserConfirmation:YES];
}

- (void)devicePairingPINCodeRequest:(id)oSender
{
  BluetoothPINCode stPin;
  af_bt_pair_cb_msg_st stMsg;
  memset(&stPin, 0, sizeof(stPin));
  memcpy(stPin.data, "0000", 4u);
  memset(&stMsg, 0, sizeof(stMsg));
  stMsg.eStage = AF_BT_PAIR_PAIRING;
  [self fillAddr:&stMsg];
  snprintf(stMsg.acCode, sizeof(stMsg.acCode), "0000");
  [self postMsg:&stMsg];
  [(IOBluetoothDevicePair *)oSender replyPINCode:4u PINCode:&stPin];
}

- (void)devicePairingFinished:(id)oSender error:(IOReturn)iError
{
  (void)oSender;
  af_bt_pair_cb_msg_st stMsg;
  memset(&stMsg, 0, sizeof(stMsg));
  [self fillAddr:&stMsg];
  if (kIOReturnSuccess == iError)
  {
    stMsg.eStage = AF_BT_PAIR_DONE;
  }
  else
  {
    stMsg.eStage = AF_BT_PAIR_FAILED;
    snprintf(stMsg.acMsg, sizeof(stMsg.acMsg), "Bluetooth pairing failed 0x%x", (uint)iError);
  }
  [self postMsg:&stMsg];
}

@end

static IOBluetoothDeviceInquiry *g_oInquiry = nil;
static IOBluetoothDevicePair *g_oPair = nil;
static AFBtPairDelegate *g_oPairDel = nil;

af_bt_auth_et af_plat_bt_auth(void)
{
  switch ([CBManager authorization])
  {
    case CBManagerAuthorizationAllowedAlways: return AF_BT_AUTH_ALLOWED;
    case CBManagerAuthorizationDenied:        return AF_BT_AUTH_DENIED;
    case CBManagerAuthorizationRestricted:    return AF_BT_AUTH_RESTRICTED;
    default:                                  return AF_BT_AUTH_NOT_DETERMINED;
  }
}

uint32 af_plat_bt_paired(af_bt_device_st *pastOut, uint32 u32Cap)
{
  if ((NULL == pastOut) || (0u == u32Cap))
  {
    return 0u;
  }
  @autoreleasepool
  {
    NSArray *oDevs = [IOBluetoothDevice pairedDevices];
    uint32 u32Cnt = 0u;
    for (IOBluetoothDevice *oD in oDevs)
    {
      if (u32Cnt >= u32Cap) { break; }
      NSString *oAddr = [oD addressString];
      NSString *oName = [oD nameOrAddress];
      if (nil == oAddr) { continue; }
      memset(&pastOut[u32Cnt], 0, sizeof(pastOut[u32Cnt]));
      snprintf(pastOut[u32Cnt].acAddr, sizeof(pastOut[u32Cnt].acAddr), "%s", [oAddr UTF8String]);
      snprintf(pastOut[u32Cnt].acName, sizeof(pastOut[u32Cnt].acName), "%s",
               (nil != oName) ? [oName UTF8String] : "");
      pastOut[u32Cnt].bConnected = [oD isConnected] ? DM_TRUE : DM_FALSE;
      pastOut[u32Cnt].u32Major = (uint32)[oD deviceClassMajor];
      pastOut[u32Cnt].bBle = (0u == pastOut[u32Cnt].u32Major) ? DM_TRUE : DM_FALSE;
      u32Cnt++;
    }
    return u32Cnt;
  }
}

static IOBluetoothDevice *af__bt_find(const char *pcAddr)
{
  if ((NULL == pcAddr) || ('\0' == pcAddr[0]))
  {
    return nil;
  }
  NSString *oAddr = [NSString stringWithUTF8String:pcAddr];
  return (nil != oAddr) ? [IOBluetoothDevice deviceWithAddressString:oAddr] : nil;
}

bool_t af_plat_bt_is_connected(const char *pcAddr)
{
  @autoreleasepool
  {
    IOBluetoothDevice *oDev = af__bt_find(pcAddr);
    return ((nil != oDev) && [oDev isConnected]) ? DM_TRUE : DM_FALSE;
  }
}

int af_plat_bt_connect(const char *pcAddr)
{
  @autoreleasepool
  {
    IOBluetoothDevice *oDev = af__bt_find(pcAddr);
    if (nil == oDev)
    {
      return DM_NOT_FOUND;
    }
    if ([oDev isConnected])
    {
      return DM_SUCCESS;
    }
    return (kIOReturnSuccess == [oDev openConnection]) ? DM_SUCCESS : DM_IO;
  }
}

int af_plat_bt_disconnect(const char *pcAddr)
{
  @autoreleasepool
  {
    IOBluetoothDevice *oDev = af__bt_find(pcAddr);
    if (nil == oDev)
    {
      return DM_NOT_FOUND;
    }
    if (![oDev isConnected])
    {
      return DM_SUCCESS;
    }
    return (kIOReturnSuccess == [oDev closeConnection]) ? DM_SUCCESS : DM_IO;
  }
}

int af_plat_bt_pair_search(uint uiCbLinkID)
{
  @autoreleasepool
  {
    if (nil != g_oInquiry)
    {
      g_oInquiry.delegate = nil;
      [g_oInquiry stop];
      g_oInquiry = nil;
    }
    if (nil == g_oPairDel)
    {
      g_oPairDel = [[AFBtPairDelegate alloc] init];
    }
    g_oPairDel.cbLinkID = uiCbLinkID;
    g_oPairDel.pairAddr = nil;
    g_oInquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:g_oPairDel];
    if (nil == g_oInquiry)
    {
      return DM_FAILD;
    }
    return (kIOReturnSuccess == [g_oInquiry start]) ? DM_SUCCESS : DM_IO;
  }
}

int af_plat_bt_pair_start(const char *pcAddr, uint uiCbLinkID)
{
  @autoreleasepool
  {
    IOBluetoothDevice *oDev = af__bt_find(pcAddr);
    if (nil == oDev)
    {
      return DM_NOT_FOUND;
    }
    if (nil != g_oInquiry)
    {
      g_oInquiry.delegate = nil;
      [g_oInquiry stop];
      g_oInquiry = nil;
    }
    if (nil != g_oPair)
    {
      g_oPair.delegate = nil;
      [g_oPair stop];
      g_oPair = nil;
    }
    if (nil == g_oPairDel)
    {
      g_oPairDel = [[AFBtPairDelegate alloc] init];
    }
    g_oPairDel.cbLinkID = uiCbLinkID;
    g_oPairDel.pairAddr = (NULL != pcAddr) ? [NSString stringWithUTF8String:pcAddr] : nil;
    g_oPair = [IOBluetoothDevicePair pairWithDevice:oDev];
    if (nil == g_oPair)
    {
      return DM_FAILD;
    }
    g_oPair.delegate = g_oPairDel;
    return (kIOReturnSuccess == [g_oPair start]) ? DM_SUCCESS : DM_IO;
  }
}

void af_plat_bt_pair_cancel(void)
{
  @autoreleasepool
  {
    if (nil != g_oInquiry)
    {
      g_oInquiry.delegate = nil;
      [g_oInquiry stop];
      g_oInquiry = nil;
    }
    if (nil != g_oPair)
    {
      g_oPair.delegate = nil;
      [g_oPair stop];
      g_oPair = nil;
    }
    g_oPairDel = nil;
  }
}
