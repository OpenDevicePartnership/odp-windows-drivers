#pragma once

#include <ntddk.h>
#include <wdf.h>

typedef VOID (*ECTEST_PCC_COMPLETION)(NTSTATUS Status, ULONG_PTR Context);
typedef NTSTATUS (*ECTEST_PCC_ACCESS)(PVOID Handle);
typedef NTSTATUS (*ECTEST_PCC_ACQUIRE_ASYNC)(
    PVOID Handle,
    ECTEST_PCC_COMPLETION Completion,
    ULONG_PTR Context
    );
typedef NTSTATUS (*ECTEST_PCC_EXECUTE)(PVOID Handle, UCHAR Command);
typedef NTSTATUS (*ECTEST_PCC_EXECUTE_ASYNC)(
    PVOID Handle,
    UCHAR Command,
    ECTEST_PCC_COMPLETION Completion,
    ULONG_PTR Context
    );

typedef struct _ECTEST_PCC_NATIVE_INTERFACE
{
    USHORT Size;
    USHORT Version;
    PVOID Context;
    PINTERFACE_REFERENCE InterfaceReference;
    PINTERFACE_DEREFERENCE InterfaceDereference;
    ULONG SubspaceId;
    ECTEST_PCC_ACCESS PlatformNotify;
    PVOID NotifyContext;
    PVOID Handle;
    ULONG NominalLatency;
    ULONG MaximumPeriodicRate;
    PVOID Subspace;
    ULONG SubspaceSize;
    ULONG Flags;
    ECTEST_PCC_ACCESS AcquireSubspace;
    ECTEST_PCC_ACQUIRE_ASYNC AcquireSubspaceAsync;
    ECTEST_PCC_EXECUTE ExecuteCommand;
    ECTEST_PCC_EXECUTE_ASYNC ExecuteCommandAsync;
    ECTEST_PCC_ACCESS ReleaseSubspace;
} ECTEST_PCC_NATIVE_INTERFACE, *PECTEST_PCC_NATIVE_INTERFACE;

C_ASSERT(sizeof(ECTEST_PCC_NATIVE_INTERFACE) == 128);
C_ASSERT(FIELD_OFFSET(ECTEST_PCC_NATIVE_INTERFACE, SubspaceId) == 32);
C_ASSERT(FIELD_OFFSET(ECTEST_PCC_NATIVE_INTERFACE, Handle) == 56);
C_ASSERT(FIELD_OFFSET(ECTEST_PCC_NATIVE_INTERFACE, Subspace) == 72);
C_ASSERT(FIELD_OFFSET(ECTEST_PCC_NATIVE_INTERFACE, AcquireSubspace) == 88);
C_ASSERT(FIELD_OFFSET(ECTEST_PCC_NATIVE_INTERFACE, ExecuteCommand) == 104);
C_ASSERT(FIELD_OFFSET(ECTEST_PCC_NATIVE_INTERFACE, ReleaseSubspace) == 120);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ECTestPccQuery(
    WDFDEVICE Device,
    PECTEST_PCC_NATIVE_INTERFACE Interface,
    PULONG AcpiTimeStamp,
    PULONG AcpiImageSize
    );

NTSTATUS ECTestPccQueueInitialize(WDFDEVICE Device);
EVT_WDF_DEVICE_RELEASE_HARDWARE ECTestPccReleaseHardware;