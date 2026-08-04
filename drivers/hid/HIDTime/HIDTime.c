/*++

Copyright (c) OpenDevicePartnership.  All rights reserved.

Module Name:

    HIDTime.c

Abstract:

    Stub KMDF driver that publishes the ACPI Time and Alarm Device (TAD)
    interface (GUID_DEVICE_ACPI_TIME) and answers its IOCTLs with fake,
    obviously-recognizable data. It does not touch real hardware and does
    not layer on HIDClass.sys; it exists so user-mode clients that talk the
    TAD IOCTL contract (see odp-platform-common ec/test-lib/src/hid.rs) have
    something to bind to on QEMU where no ACPI time device is enumerated.

Environment:

    kernel-mode only

--*/

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>

//
// ACPI Time and Alarm Device interface + IOCTL contract.
// Mirrors <poclass.h>; redefined here with kernel-friendly types so the
// driver stays self-contained (poclass.h uses UINT8/UINT16 which are not
// available in the kernel headers).
//

// {97F99BF6-4497-4F18-BB22-4B9FB2FBEF9C}
DEFINE_GUID(GUID_DEVICE_ACPI_TIME,
            0x97f99bf6, 0x4497, 0x4f18, 0xbb, 0x22, 0x4b, 0x9f, 0xb2, 0xfb, 0xef, 0x9c);

#ifndef FILE_DEVICE_BATTERY
#define FILE_DEVICE_BATTERY 0x00000029
#endif

#define IOCTL_SET_WAKE_ALARM_VALUE \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x80, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_SET_WAKE_ALARM_POLICY \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x81, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_GET_WAKE_ALARM_VALUE \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x82, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_GET_WAKE_ALARM_POLICY \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x83, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_ACPI_GET_REAL_TIME \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x84, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_ACPI_SET_REAL_TIME \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x85, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_GET_WAKE_ALARM_SYSTEM_POWERSTATE \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x86, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_GET_ACPI_TIME_AND_ALARM_CAPABILITIES \
    CTL_CODE(FILE_DEVICE_BATTERY, 0x87, METHOD_BUFFERED, FILE_READ_ACCESS)

#include <pshpack1.h>
typedef struct _WAKE_ALARM_INFORMATION
{
    ULONG TimerIdentifier;
    ULONG Timeout;
} WAKE_ALARM_INFORMATION, *PWAKE_ALARM_INFORMATION;
#include <poppack.h>

typedef struct _ACPI_REAL_TIME
{
    USHORT Year;
    UCHAR Month;
    UCHAR Day;
    UCHAR Hour;
    UCHAR Minute;
    UCHAR Second;
    UCHAR Valid;
    USHORT Milliseconds;
    SHORT TimeZone;
    UCHAR DayLight;
    UCHAR Reserved1[3];
} ACPI_REAL_TIME, *PACPI_REAL_TIME;

typedef enum _ACPI_TIME_RESOLUTION
{
    AcpiTimeResolutionMilliseconds = 0,
    AcpiTimeResolutionSeconds,
    AcpiTimeResolutionMax
} ACPI_TIME_RESOLUTION;

typedef struct _ACPI_TIME_AND_ALARM_CAPABILITIES
{
    BOOLEAN AcWakeSupported;
    BOOLEAN DcWakeSupported;
    BOOLEAN S4AcWakeSupported;
    BOOLEAN S4DcWakeSupported;
    BOOLEAN S5AcWakeSupported;
    BOOLEAN S5DcWakeSupported;
    BOOLEAN S4S5WakeStatusSupported;
    ULONG DeepestWakeSystemState;
    BOOLEAN RealTimeFeaturesSupported;
    ACPI_TIME_RESOLUTION RealTimeResolution;
} ACPI_TIME_AND_ALARM_CAPABILITIES, *PACPI_TIME_AND_ALARM_CAPABILITIES;

//
// Fake values. Chosen to be unmistakable so a client can tell at a glance the
// data came from this driver rather than a zeroed buffer.
//
#define HIDTIME_FAKE_ALARM_VALUE_SECONDS 0x0000ABCDUL
#define HIDTIME_FAKE_ALARM_POLICY_SECONDS 0x00001234UL

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD HidTimeEvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL HidTimeEvtIoDeviceControl;

_Use_decl_annotations_
    NTSTATUS
    DriverEntry(
        PDRIVER_OBJECT DriverObject,
        PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, HidTimeEvtDeviceAdd);

    return WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
}

_Use_decl_annotations_
    NTSTATUS
    HidTimeEvtDeviceAdd(
        WDFDRIVER Driver,
        PWDFDEVICE_INIT DeviceInit)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queueConfig;

    UNREFERENCED_PARAMETER(Driver);

    status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVICE_ACPI_TIME, NULL);
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = HidTimeEvtIoDeviceControl;

    return WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

_Use_decl_annotations_
    VOID
    HidTimeEvtIoDeviceControl(
        WDFQUEUE Queue,
        WDFREQUEST Request,
        size_t OutputBufferLength,
        size_t InputBufferLength,
        ULONG IoControlCode)
{
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;
    size_t bufLen = 0;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
    case IOCTL_ACPI_GET_REAL_TIME:
    {
        PACPI_REAL_TIME rt;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*rt), (PVOID *)&rt, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }
        RtlZeroMemory(rt, sizeof(*rt));
        rt->Year = 2026;
        rt->Month = 8;
        rt->Day = 4;
        rt->Hour = 12;
        rt->Minute = 34;
        rt->Second = 56;
        rt->Milliseconds = 789;
        rt->Valid = 1;
        information = sizeof(*rt);
        break;
    }

    case IOCTL_GET_WAKE_ALARM_VALUE:
    {
        PWAKE_ALARM_INFORMATION out;
        PWAKE_ALARM_INFORMATION in;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }
        out->TimerIdentifier = 0;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &bufLen)))
        {
            out->TimerIdentifier = in->TimerIdentifier;
        }
        out->Timeout = HIDTIME_FAKE_ALARM_VALUE_SECONDS;
        information = sizeof(*out);
        break;
    }

    case IOCTL_GET_WAKE_ALARM_POLICY:
    {
        PWAKE_ALARM_INFORMATION out;
        PWAKE_ALARM_INFORMATION in;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }
        out->TimerIdentifier = 0;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &bufLen)))
        {
            out->TimerIdentifier = in->TimerIdentifier;
        }
        out->Timeout = HIDTIME_FAKE_ALARM_POLICY_SECONDS;
        information = sizeof(*out);
        break;
    }

    case IOCTL_GET_ACPI_TIME_AND_ALARM_CAPABILITIES:
    {
        PACPI_TIME_AND_ALARM_CAPABILITIES caps;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*caps), (PVOID *)&caps, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }
        RtlZeroMemory(caps, sizeof(*caps));
        caps->AcWakeSupported = TRUE;
        caps->DcWakeSupported = TRUE;
        caps->S4AcWakeSupported = TRUE;
        caps->S4DcWakeSupported = TRUE;
        caps->S5AcWakeSupported = TRUE;
        caps->S5DcWakeSupported = TRUE;
        caps->S4S5WakeStatusSupported = TRUE;
        caps->DeepestWakeSystemState = PowerSystemHibernate;
        caps->RealTimeFeaturesSupported = TRUE;
        caps->RealTimeResolution = AcpiTimeResolutionSeconds;
        information = sizeof(*caps);
        break;
    }

    case IOCTL_GET_WAKE_ALARM_SYSTEM_POWERSTATE:
    {
        PULONG powerState;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*powerState), (PVOID *)&powerState, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }
        *powerState = PowerSystemHibernate;
        information = sizeof(*powerState);
        break;
    }

    case IOCTL_SET_WAKE_ALARM_VALUE:
    case IOCTL_SET_WAKE_ALARM_POLICY:
    case IOCTL_ACPI_SET_REAL_TIME:
        // Stub: accept the write and discard it.
        status = STATUS_SUCCESS;
        information = 0;
        break;

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}
