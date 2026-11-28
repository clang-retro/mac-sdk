/* The Open Transport calls that have two forms on 68k: one over native
 * Open Transport (otnative.c) and one over MacTCP (ot68k.c).
 *
 * ot68k.c includes this file before <OpenTransport.h>, so that its
 * forms get the names __otsock_X. otnative.c gives the public names:
 * each one calls the native form when InitOpenTransport started native
 * Open Transport, else the __otsock_ form.
 *
 * OT68K_CALLS lists the calls. Each entry is X(name).
 *
 * Copyright (C) clang-retro contributors.
 * SPDX-License-Identifier: AGPL-3.0-only */
#ifndef OT68K_NAMES_H
#define OT68K_NAMES_H

#define OT68K_CALLS(X)                                                                             \
  X(InitOpenTransport) X(InitOpenTransportUtilities) X(CloseOpenTransport)                         \
  X(OTRegisterAsClient) X(OTUnregisterAsClient) X(OTAllocMem) X(OTFreeMem) X(OTAlloc) X(OTFree)    \
  X(OTIdle) X(OTCreateConfiguration) X(OTCloneConfiguration) X(OTDestroyConfiguration)             \
  X(OTOpenEndpoint) X(OTAsyncOpenEndpoint) X(OTOpenInternetServices)                               \
  X(OTAsyncOpenInternetServices) X(OTCloseProvider) X(OTSetSynchronous) X(OTSetAsynchronous)      \
  X(OTIsSynchronous) X(OTSetBlocking) X(OTSetNonBlocking) X(OTIsBlocking) X(OTInstallNotifier)    \
  X(OTRemoveNotifier) X(OTUseSyncIdleEvents) X(OTEnterNotifier) X(OTLeaveNotifier) X(OTAckSends)  \
  X(OTDontAckSends) X(OTIsAckingSends) X(OTCancelSynchronousCalls) X(OTIoctl)                      \
  X(OTGetProviderPortRef) X(OTGetEndpointInfo) X(OTGetEndpointState) X(OTLook)                     \
  X(OTCountDataBytes) X(OTGetProtAddress) X(OTResolveAddress) X(OTOptionManagement) X(OTBind)     \
  X(OTUnbind) X(OTConnect) X(OTRcvConnect) X(OTListen) X(OTAccept) X(OTSndDisconnect)             \
  X(OTSndOrderlyDisconnect) X(OTRcvDisconnect) X(OTRcvOrderlyDisconnect) X(OTRcv) X(OTSnd)        \
  X(OTSndUData) X(OTRcvUData) X(OTRcvUDErr) X(OTInetGetInterfaceInfo)                              \
  X(OTInetGetSecondaryAddresses) X(OTInetStringToAddress) X(OTInetAddressToName)                  \
  X(OTInetSysInfo) X(OTInetMailExchange) X(OTInetQuery)

#ifdef OT68K_RENAME
#define InitOpenTransport __otsock_InitOpenTransport
#define InitOpenTransportUtilities __otsock_InitOpenTransportUtilities
#define CloseOpenTransport __otsock_CloseOpenTransport
#define OTRegisterAsClient __otsock_OTRegisterAsClient
#define OTUnregisterAsClient __otsock_OTUnregisterAsClient
#define OTAllocMem __otsock_OTAllocMem
#define OTFreeMem __otsock_OTFreeMem
#define OTAlloc __otsock_OTAlloc
#define OTFree __otsock_OTFree
#define OTIdle __otsock_OTIdle
#define OTCreateConfiguration __otsock_OTCreateConfiguration
#define OTCloneConfiguration __otsock_OTCloneConfiguration
#define OTDestroyConfiguration __otsock_OTDestroyConfiguration
#define OTOpenEndpoint __otsock_OTOpenEndpoint
#define OTAsyncOpenEndpoint __otsock_OTAsyncOpenEndpoint
#define OTOpenInternetServices __otsock_OTOpenInternetServices
#define OTAsyncOpenInternetServices __otsock_OTAsyncOpenInternetServices
#define OTCloseProvider __otsock_OTCloseProvider
#define OTSetSynchronous __otsock_OTSetSynchronous
#define OTSetAsynchronous __otsock_OTSetAsynchronous
#define OTIsSynchronous __otsock_OTIsSynchronous
#define OTSetBlocking __otsock_OTSetBlocking
#define OTSetNonBlocking __otsock_OTSetNonBlocking
#define OTIsBlocking __otsock_OTIsBlocking
#define OTInstallNotifier __otsock_OTInstallNotifier
#define OTRemoveNotifier __otsock_OTRemoveNotifier
#define OTUseSyncIdleEvents __otsock_OTUseSyncIdleEvents
#define OTEnterNotifier __otsock_OTEnterNotifier
#define OTLeaveNotifier __otsock_OTLeaveNotifier
#define OTAckSends __otsock_OTAckSends
#define OTDontAckSends __otsock_OTDontAckSends
#define OTIsAckingSends __otsock_OTIsAckingSends
#define OTCancelSynchronousCalls __otsock_OTCancelSynchronousCalls
#define OTIoctl __otsock_OTIoctl
#define OTGetProviderPortRef __otsock_OTGetProviderPortRef
#define OTGetEndpointInfo __otsock_OTGetEndpointInfo
#define OTGetEndpointState __otsock_OTGetEndpointState
#define OTLook __otsock_OTLook
#define OTCountDataBytes __otsock_OTCountDataBytes
#define OTGetProtAddress __otsock_OTGetProtAddress
#define OTResolveAddress __otsock_OTResolveAddress
#define OTOptionManagement __otsock_OTOptionManagement
#define OTBind __otsock_OTBind
#define OTUnbind __otsock_OTUnbind
#define OTConnect __otsock_OTConnect
#define OTRcvConnect __otsock_OTRcvConnect
#define OTListen __otsock_OTListen
#define OTAccept __otsock_OTAccept
#define OTSndDisconnect __otsock_OTSndDisconnect
#define OTSndOrderlyDisconnect __otsock_OTSndOrderlyDisconnect
#define OTRcvDisconnect __otsock_OTRcvDisconnect
#define OTRcvOrderlyDisconnect __otsock_OTRcvOrderlyDisconnect
#define OTRcv __otsock_OTRcv
#define OTSnd __otsock_OTSnd
#define OTSndUData __otsock_OTSndUData
#define OTRcvUData __otsock_OTRcvUData
#define OTRcvUDErr __otsock_OTRcvUDErr
#define OTInetGetInterfaceInfo __otsock_OTInetGetInterfaceInfo
#define OTInetGetSecondaryAddresses __otsock_OTInetGetSecondaryAddresses
#define OTInetStringToAddress __otsock_OTInetStringToAddress
#define OTInetAddressToName __otsock_OTInetAddressToName
#define OTInetSysInfo __otsock_OTInetSysInfo
#define OTInetMailExchange __otsock_OTInetMailExchange
#define OTInetQuery __otsock_OTInetQuery
#endif

#endif
