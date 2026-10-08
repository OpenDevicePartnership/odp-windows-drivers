#include <ntddk.h>
#include <wdf.h>

#include "Trace.h"
#include "km_wil_result_macros.h"

#include <poclass.h>

#include "device.h"

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD HidTimeEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP HidTimeEvtDriverContextCleanup;

_Use_decl_annotations_ VOID
HidTimeEvtDriverContextCleanup(WDFOBJECT Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    TraceLoggingUnregister(g_hTraceProvider);
}

_Use_decl_annotations_ extern "C" NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = HidTimeEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, &DeviceContext::create);
    TraceLoggingRegister(g_hTraceProvider);

    const NTSTATUS status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status))
    {
        TraceLoggingUnregister(g_hTraceProvider);
    }
    return status;
}
