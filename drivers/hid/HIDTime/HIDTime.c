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

//
// TAD data producers. These are the real handler functions; their bodies are
// currently stubbed with fake, obviously-recognizable data until the HID
// transport lands.
//
static VOID
HidTimeGetRealTime(PACPI_REAL_TIME Rt)
{
    // TODO: replace stub with real HIDClass.sys / ACPI TAD query once HID transport lands.
    RtlZeroMemory(Rt, sizeof(*Rt));
    Rt->Year = 2015;
    Rt->Month = 3;
    Rt->Day = 14;
    Rt->Hour = 9;
    Rt->Minute = 26;
    Rt->Second = 53;
    Rt->Milliseconds = 589;
    Rt->Valid = 1;
}

static VOID
HidTimeGetWakeAlarmValue(PWAKE_ALARM_INFORMATION Out, ULONG TimerIdentifier)
{
    // TODO: replace stub with real HIDClass.sys / ACPI TAD query once HID transport lands.
    // Fake timeout chosen to be an obviously-recognizable seconds value.
    Out->TimerIdentifier = TimerIdentifier;
    Out->Timeout = 12345;
}

static VOID
HidTimeGetWakeAlarmPolicy(PWAKE_ALARM_INFORMATION Out, ULONG TimerIdentifier)
{
    // TODO: replace stub with real HIDClass.sys / ACPI TAD query once HID transport lands.
    // Fake timeout chosen to be an obviously-recognizable seconds value.
    Out->TimerIdentifier = TimerIdentifier;
    Out->Timeout = 54321;
}

static VOID
HidTimeGetCapabilities(PACPI_TIME_AND_ALARM_CAPABILITIES Caps)
{
    // TODO: replace stub with real HIDClass.sys / ACPI TAD query once HID transport lands.
    RtlZeroMemory(Caps, sizeof(*Caps));
    Caps->AcWakeSupported = TRUE;
    Caps->DcWakeSupported = TRUE;
    Caps->S4AcWakeSupported = TRUE;
    Caps->S4DcWakeSupported = TRUE;
    Caps->S5AcWakeSupported = TRUE;
    Caps->S5DcWakeSupported = TRUE;
    Caps->S4S5WakeStatusSupported = TRUE;
    Caps->DeepestWakeSystemState = PowerSystemHibernate;
    Caps->RealTimeFeaturesSupported = TRUE;
    Caps->RealTimeResolution = AcpiTimeResolutionSeconds;
}

static VOID
HidTimeGetWakeAlarmPowerState(PULONG PowerState)
{
    // TODO: replace stub with real HIDClass.sys / ACPI TAD query once HID transport lands.
    *PowerState = PowerSystemHibernate;
}

static NTSTATUS
HidTimeSetWakeAlarmValue(PWAKE_ALARM_INFORMATION In)
{
    // TODO: real driver programs the wake timer/timeout from this input.
    UNREFERENCED_PARAMETER(In);
    return STATUS_SUCCESS;
}

static NTSTATUS
HidTimeSetWakeAlarmPolicy(PWAKE_ALARM_INFORMATION In)
{
    // TODO: real driver configures the wake alarm policy from this input.
    UNREFERENCED_PARAMETER(In);
    return STATUS_SUCCESS;
}

static NTSTATUS
HidTimeSetRealTime(PACPI_REAL_TIME In)
{
    // TODO: real driver writes the RTC/real time from this input.
    UNREFERENCED_PARAMETER(In);
    return STATUS_SUCCESS;
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

        HidTimeGetRealTime(rt);

        information = sizeof(*rt);
        break;
    }

    case IOCTL_GET_WAKE_ALARM_VALUE:
    {
        PWAKE_ALARM_INFORMATION out;
        PWAKE_ALARM_INFORMATION in;
        ULONG timerId = 0;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &bufLen)))
        {
            timerId = in->TimerIdentifier;
        }

        HidTimeGetWakeAlarmValue(out, timerId);

        information = sizeof(*out);
        break;
    }

    case IOCTL_GET_WAKE_ALARM_POLICY:
    {
        PWAKE_ALARM_INFORMATION out;
        PWAKE_ALARM_INFORMATION in;
        ULONG timerId = 0;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &bufLen)))
        {
            timerId = in->TimerIdentifier;
        }

        HidTimeGetWakeAlarmPolicy(out, timerId);

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

        HidTimeGetCapabilities(caps);

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

        HidTimeGetWakeAlarmPowerState(powerState);

        information = sizeof(*powerState);
        break;
    }

    case IOCTL_SET_WAKE_ALARM_VALUE:
    {
        PWAKE_ALARM_INFORMATION in;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = HidTimeSetWakeAlarmValue(in);

        information = 0;
        break;
    }

    case IOCTL_SET_WAKE_ALARM_POLICY:
    {
        PWAKE_ALARM_INFORMATION in;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = HidTimeSetWakeAlarmPolicy(in);

        information = 0;
        break;
    }

    case IOCTL_ACPI_SET_REAL_TIME:
    {
        PACPI_REAL_TIME in;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, &bufLen);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = HidTimeSetRealTime(in);

        information = 0;
        break;
    }

    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}
