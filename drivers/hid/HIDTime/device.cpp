/*++

Copyright (c) OpenDevicePartnership and Contributors.  All rights reserved.

Module Name:

    device.cpp

Abstract:

    KMDF driver that publishes the ACPI Time and Alarm Device (TAD)
    interface (GUID_DEVICE_ACPI_TIME) and relays to a HID-based time-alarm device.

Environment:

    kernel-mode only

--*/

#include "device.h"

#include "Trace.h"
#include "tad_hid_usages.h"

namespace
{
constexpr ULONG pool_tag = 'TdiH';

// The fields a device has to carry in both of its time reports for the TAD contract to be serviceable. Milliseconds are
// deliberately absent: they are optional, and load_tad_capabilities reports whichever resolution the device offers.
constexpr USAGE required_time_value_usages[] = {
    hid_constants::time_and_date::usage::year,
    hid_constants::time_and_date::usage::month,
    hid_constants::time_and_date::usage::day,
    hid_constants::time_and_date::usage::hour,
    hid_constants::time_and_date::usage::minute,
    hid_constants::time_and_date::usage::second,
    hid_constants::time_and_date::usage::time_zone_offset_from_utc,
};

// 1-bit fields are buttons, so they are described by button caps rather than value caps.
constexpr USAGE required_time_button_usages[] = {
    hid_constants::time_and_date::usage::dst_observed,
    hid_constants::time_and_date::usage::dst_active,
};

// WAKE_ALARM_INFORMATION::TimerIdentifier and ::Timeout values, which poclass.h documents only in comments.
constexpr ULONG ac_timer_identifier = 0;
constexpr ULONG dc_timer_identifier = 1;
constexpr ULONG wake_alarm_timeout_disabled = 0xFFFFFFFF;

// The ranges poclass.h documents for ACPI_REAL_TIME
constexpr LONG acpi_time_zone_min = -1440;
constexpr LONG acpi_time_zone_max = 1440;

// Identical to implementation of WDF_REL_TIMEOUT_IN_SEC in wdfcore.h, but constexpr so we can use it for constant init
constexpr LONGLONG
WDF_REL_TIMEOUT_IN_SEC_CONSTEXPR(ULONGLONG Time)
{
    return Time * -1 * WDF_TIMEOUT_TO_SEC;
}
constexpr LONGLONG hid_operation_timeout = WDF_REL_TIMEOUT_IN_SEC_CONSTEXPR(5);

// Finds a legal "null" value for the specified field.
//
// A null-state field reports "no value" as anything outside its logical range, so which value that is depends on the
// descriptor.
//
// HidP_InitializeReportForID is supposed to default-construct a report to all nulls (or 0s for fields that aren't
// listed as nullable in the report descriptor), but it has a bug that makes it not work correctly on output reports
// on devices that also have input reports, so we can't use it to construct 'blank' output reports.  Therefore, we
// need to compute a null value ourselves.
//
// TODO when HidP_InitializeReportForIdEx is made available, switch to just using it to default things to null and
//      get rid of this function.
//
NTSTATUS
null_value_for_field(const HIDP_VALUE_CAPS &caps, ULONG *null_value)
{
    if (!caps.HasNull || (caps.BitSize == 0) || (caps.BitSize > 32))
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_CONFIGURATION_ERROR);
    }

    // A logical minimum below zero is what makes a field signed, which halves the range the value can sit in.
    const bool is_signed = (caps.LogicalMin < 0);
    const LONG64 field_min = is_signed ? -(1LL << (caps.BitSize - 1)) : 0;
    const LONG64 field_max = is_signed ? ((1LL << (caps.BitSize - 1)) - 1) : ((1LL << caps.BitSize) - 1);

    LONG64 value = 0;
    if (caps.LogicalMin > field_min)
    {
        value = static_cast<LONG64>(caps.LogicalMin) - 1;
    }
    else if (caps.LogicalMax < field_max)
    {
        value = static_cast<LONG64>(caps.LogicalMax) + 1;
    }
    else
    {
        // The logical range fills the field, so there is nothing left to spell null with.
        NT_RETURN_NTSTATUS(STATUS_DEVICE_CONFIGURATION_ERROR);
    }

    // HidP writes the low BitSize bits, so a negative null has to go down as its two's complement.
    *null_value = static_cast<ULONG>(static_cast<ULONG64>(value) & ((1ull << caps.BitSize) - 1));

    return STATUS_SUCCESS;
}

bool
is_timer_identifier_valid(ULONG timer_identifier)
{
    return (timer_identifier == ac_timer_identifier) || (timer_identifier == dc_timer_identifier);
}

// Read the value of a boolean field. Counterpart to HidPExt_SetButtonValue: a 1-bit field is a button, so the only way
// to read one is to pull the list of set usages for the page and look for ours in it.
NTSTATUS
HidPExt_GetButtonValue(_In_ HIDP_REPORT_TYPE ReportType,
                       _In_ USAGE UsagePage,
                       _In_ USHORT LinkCollection,
                       _In_ USAGE Usage,
                       _Out_ BOOLEAN *Value,
                       _In_ PHIDP_PREPARSED_DATA PreparsedData,
                       _In_ PCHAR Report,
                       _In_ ULONG ReportLength)
{
    *Value = FALSE;

    ULONG usage_count = HidP_MaxUsageListLength(ReportType, UsagePage, PreparsedData);
    if (usage_count == 0)
    {
        NT_RETURN_NTSTATUS(HIDP_STATUS_USAGE_NOT_FOUND);
    }

    wil::unique_wdf_memory usage_memory;
    USAGE *usages{};
    NT_RETURN_IF_NTSTATUS_FAILED(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                                 NonPagedPoolNx,
                                                 pool_tag,
                                                 usage_count * sizeof(USAGE),
                                                 usage_memory.put(),
                                                 reinterpret_cast<void **>(&usages)));

    NT_RETURN_IF_NTSTATUS_FAILED(
        HidP_GetUsages(ReportType, UsagePage, LinkCollection, usages, &usage_count, PreparsedData, Report, ReportLength));

    for (ULONG i = 0; i < usage_count; ++i)
    {
        if (usages[i] == Usage)
        {
            *Value = TRUE;
            break;
        }
    }

    return STATUS_SUCCESS;
}

// Set the value of a boolean field to a specific value.
NTSTATUS
HidPExt_SetButtonValue(_In_ HIDP_REPORT_TYPE ReportType,
                       _In_ USAGE UsagePage,
                       _In_ USHORT LinkCollection,
                       _In_ USAGE Usage,
                       _In_ BOOLEAN Value,
                       _In_ PHIDP_PREPARSED_DATA PreparsedData,
                       _In_ PCHAR Report,
                       _In_ ULONG ReportLength)
{
    ULONG usageLength = 1;
    NTSTATUS status =
        Value ? HidP_SetUsages(ReportType, UsagePage, LinkCollection, &Usage, &usageLength, PreparsedData, Report, ReportLength)
              : HidP_UnsetUsages(ReportType, UsagePage, LinkCollection, &Usage, &usageLength, PreparsedData, Report, ReportLength);

    // The HID API will error if we try to set a boolean value to false when it is already false because it assumes that all
    // booleans are literal buttons, but that's not really true for our use case and we want to be able to unconditionally set a
    // boolean value to false without reasoning about its prior value, so we need to swallow this error.
    if (status == HIDP_STATUS_BUTTON_NOT_PRESSED)
    {
        status = STATUS_SUCCESS;
    }

    return status;
}

// Sends a driver-built request bearing our collection file object. Anything that goes to the local target lands on the
// next lower device object, so nothing here re-enters our own dispatch path.
NTSTATUS
send_synchronously(WDFIOTARGET target, WDFREQUEST request, PFILE_OBJECT file_object, ULONG_PTR *bytes_returned)
{
    PIRP irp = WdfRequestWdmGetIrp(request);

    // IoBuildDeviceIoControlRequest would set this; HIDCLASS resolves the requestor's session from it.
    irp->Tail.Overlay.Thread = PsGetCurrentThread();
    IoGetNextIrpStackLocation(irp)->FileObject = file_object;

    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS | WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, hid_operation_timeout);

    WdfRequestSend(request, target, &options);
    NT_RETURN_IF_NTSTATUS_FAILED(WdfRequestGetStatus(request));

    if (bytes_returned != nullptr)
    {
        *bytes_returned = WdfRequestGetInformation(request);
    }

    return STATUS_SUCCESS;
}

// Logically "zeroes" an output report, preparing it for use with HidP_SetUsageValue.
//
// HidP_InitializeReportForID is supposed to be the way to do this, but it has a bug that causes it to fail to construct
// output and feature reports in some cases.  There appears to be a HidP_InitializeReportForIdEx function in hidparse.sys
// and it looks like it fixes the bug that was impacting us, but it's not exported from hidparse.sys so we can't actually
// use it here (confirmed that it worked by hacking a flag in the debugger during a call to HidP_InitializeReportForID).
//
// Until we figure out why HidP_InitializeReportForIdEx isn't exported, we're using this workaround to initialize output
// reports.  TODO When/if HidP_InitializeReportForIdEx becomes available, switch to using it instead.
//
// This function works for the embedded-services reference implementation for output reports, but the 'real' implementation
// does a bunch of extra stuff to handle null-state fields that we're not doing here - that may cause issues with devices that
// use reports with custom vendor-specific fields.
//
void
initialize_output_report(_Out_writes_bytes_(report_length) PCHAR report, ULONG report_length, UINT8 report_id)
{
    RtlZeroMemory(report, report_length);
    report[0] = static_cast<CHAR>(report_id);
}
} // namespace

NTSTATUS
DeviceContext::create(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit)
{
    UNREFERENCED_PARAMETER(driver);

    // HIDCLASS is the power policy owner
    WdfDeviceInitSetPowerPolicyOwnership(deviceInit, false);

    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtDestroyCallback = on_destroy;

    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = on_prepare_hardware;
    pnpCallbacks.EvtDeviceReleaseHardware = on_release_hardware;
    pnpCallbacks.EvtDeviceQueryRemove = on_query_remove;
    pnpCallbacks.EvtDeviceSurpriseRemoval = on_surprise_removal;
    WdfDeviceInitSetPnpPowerEventCallbacks(deviceInit, &pnpCallbacks);

    // Our own open of the collection comes back down through here, and so does the close when we drop the file
    // object; both have to reach HIDCLASS, which is what AutoForwardCleanupClose gets us with no callbacks of our own.
    // Who may open us at all is enforced by the SDDL in the INF, not here.
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK);
    fileConfig.AutoForwardCleanupClose = WdfTrue;
    WdfDeviceInitSetFileObjectConfig(deviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDFDEVICE device;
    NT_RETURN_IF_NTSTATUS_FAILED(WdfDeviceCreate(&deviceInit, &attributes, &device));

    DeviceContext *deviceContext = new (GetDeviceContext(device)) DeviceContext{};
    deviceContext->m_device = device;
    NT_RETURN_IF_NTSTATUS_FAILED(deviceContext->m_device_lock.create());

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = on_ioctl;

    // We can't request D0 as a non-power-policy-owner, so a power-managed queue would park
    // requests forever once HIDCLASS drops the collection to Dx.
    queueConfig.PowerManaged = WdfFalse;

    // Sending reports synchronously requires PASSIVE_LEVEL.
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&queueAttributes);
    queueAttributes.ExecutionLevel = WdfExecutionLevelPassive;

    NT_RETURN_IF_NTSTATUS_FAILED(WdfIoQueueCreate(device, &queueConfig, &queueAttributes, WDF_NO_HANDLE));
    NT_RETURN_IF_NTSTATUS_FAILED(WdfDeviceCreateDeviceInterface(device, &GUID_DEVICE_ACPI_TIME, NULL));

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ VOID
DeviceContext::on_destroy(WDFOBJECT object)
{
    GetDeviceContext(object)->~DeviceContext();
}

_Use_decl_annotations_ VOID
DeviceContext::on_ioctl(
    WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode)
{
    DeviceContext *context = GetDeviceContext(WdfIoQueueGetDevice(Queue));

    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR information = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode)
    {
    case IOCTL_ACPI_GET_REAL_TIME: {
        PACPI_REAL_TIME rt;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*rt), (PVOID *)&rt, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        if (!NT_SUCCESS(status = context->get_time(rt)))
        {
            break;
        }

        information = sizeof(*rt);
        break;
    }

    case IOCTL_GET_WAKE_ALARM_VALUE: {
        PWAKE_ALARM_INFORMATION out;
        PWAKE_ALARM_INFORMATION in;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        ULONG timerId = ac_timer_identifier;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, nullptr)))
        {
            timerId = in->TimerIdentifier;
        }

        status = context->get_wake_alarm_value(timerId, out);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        information = sizeof(*out);
        break;
    }

    case IOCTL_GET_WAKE_ALARM_POLICY: {
        PWAKE_ALARM_INFORMATION out;
        PWAKE_ALARM_INFORMATION in;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*out), (PVOID *)&out, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        // One policy covers both timers, so an absent identifier only affects which one the answer is labelled with.
        ULONG timerId = ac_timer_identifier;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, nullptr)))
        {
            timerId = in->TimerIdentifier;
        }

        status = context->get_wake_alarm_policy(timerId, out);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        information = sizeof(*out);
        break;
    }

    case IOCTL_GET_ACPI_TIME_AND_ALARM_CAPABILITIES: {
        PACPI_TIME_AND_ALARM_CAPABILITIES caps;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*caps), (PVOID *)&caps, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = context->get_tad_capabilities(caps);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        information = sizeof(*caps);

        break;
    }

    case IOCTL_GET_WAKE_ALARM_SYSTEM_POWERSTATE: {
        PULONG powerState;

        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*powerState), (PVOID *)&powerState, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        ACPI_TIME_AND_ALARM_CAPABILITIES capabilities{};
        status = context->get_tad_capabilities(&capabilities);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        *powerState = capabilities.DeepestWakeSystemState;

        information = sizeof(*powerState);
        break;
    }

    case IOCTL_SET_WAKE_ALARM_VALUE: {
        PWAKE_ALARM_INFORMATION in;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = context->set_wake_alarm_value(in);

        information = 0;
        break;
    }

    case IOCTL_SET_WAKE_ALARM_POLICY: {
        PWAKE_ALARM_INFORMATION in;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = context->set_wake_alarm_policy(in);

        information = 0;
        break;
    }

    case IOCTL_ACPI_SET_REAL_TIME: {
        PACPI_REAL_TIME in;

        status = WdfRequestRetrieveInputBuffer(Request, sizeof(*in), (PVOID *)&in, nullptr);
        if (!NT_SUCCESS(status))
        {
            break;
        }

        status = context->set_time(in);

        information = 0;
        break;
    }

    default: {
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    }

    WdfRequestCompleteWithInformation(Request, status, information);
}

_Use_decl_annotations_ NTSTATUS
DeviceContext::on_prepare_hardware(WDFDEVICE device, WDFCMRESLIST resources_raw, WDFCMRESLIST resources_translated)
{
    DeviceContext *context = GetDeviceContext(device);

    UNREFERENCED_PARAMETER(resources_raw);
    UNREFERENCED_PARAMETER(resources_translated);

    auto lock = context->m_device_lock.acquire();

    // The cached capabilities describe the descriptor we are about to replace. Reloading them has to wait for the
    // first request, because reading the 'deepest sleep' feature report needs a handle to ourselves.
    context->m_tad_capabilities_loaded = false;

    NT_RETURN_IF_NTSTATUS_FAILED(context->load_descriptor());
    context->m_hardware_ready = true;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS
DeviceContext::on_release_hardware(WDFDEVICE device, WDFCMRESLIST resources_translated)
{
    DeviceContext *context = GetDeviceContext(device);

    UNREFERENCED_PARAMETER(resources_translated);

    auto lock = context->m_device_lock.acquire();

    context->m_hardware_ready = false;
    context->m_preparsed_data = nullptr;
    context->m_preparsed_memory.reset();
    context->close_collection_file();

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS
DeviceContext::on_query_remove(WDFDEVICE device)
{
    DeviceContext *context = GetDeviceContext(device);

    // Our open of the collection references our own device object, which is enough to defer the removal to the next
    // boot ("device in use" on a Device Manager disable). Drop it here, before the remove IRP goes down. A cancelled
    // remove needs no counterpart because ensure_collection_file() reopens on the next request.
    auto lock = context->m_device_lock.acquire();
    context->close_collection_file();

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ VOID
DeviceContext::on_surprise_removal(WDFDEVICE device)
{
    DeviceContext *context = GetDeviceContext(device);

    auto lock = context->m_device_lock.acquire();
    context->close_collection_file();
}

NTSTATUS
DeviceContext::load_descriptor()
{
    HID_COLLECTION_INFORMATION collection_information{};
    WDF_MEMORY_DESCRIPTOR descriptor;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, &collection_information, sizeof(collection_information));

    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, hid_operation_timeout);

    ULONG_PTR bytes_returned{};
    NT_RETURN_IF_NTSTATUS_FAILED(WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(m_device),
                                                                   nullptr,
                                                                   IOCTL_HID_GET_COLLECTION_INFORMATION,
                                                                   nullptr,
                                                                   &descriptor,
                                                                   &options,
                                                                   &bytes_returned));

    if ((bytes_returned < sizeof(collection_information)) || (collection_information.DescriptorSize == 0))
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
    }

    m_preparsed_data = nullptr;
    NT_RETURN_IF_NTSTATUS_FAILED(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                                 NonPagedPoolNx,
                                                 pool_tag,
                                                 collection_information.DescriptorSize,
                                                 m_preparsed_memory.put(),
                                                 reinterpret_cast<void **>(&m_preparsed_data)));

    // A short read would otherwise leave HidP parsing whatever the pool happened to hold.
    RtlZeroMemory(m_preparsed_data, collection_information.DescriptorSize);

    WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&descriptor, m_preparsed_memory.get(), nullptr);
    NT_RETURN_IF_NTSTATUS_FAILED(WdfIoTargetSendIoctlSynchronously(WdfDeviceGetIoTarget(m_device),
                                                                   nullptr,
                                                                   IOCTL_HID_GET_COLLECTION_DESCRIPTOR,
                                                                   nullptr,
                                                                   &descriptor,
                                                                   &options,
                                                                   &bytes_returned));

    if (bytes_returned != collection_information.DescriptorSize)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
    }

    NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetCaps(m_preparsed_data, &m_hid_capabilities));

    HIDP_VALUE_CAPS caps{};
    USHORT count = 1;
    NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetSpecificValueCaps(HidP_Output,
                                                           hid_constants::time_and_date::usage_page,
                                                           HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                           hid_constants::time_and_date::usage::year,
                                                           &caps,
                                                           &count,
                                                           m_preparsed_data));

    m_report_ids.set_time_report_id = caps.ReportID;

    count = 1;
    NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetSpecificValueCaps(HidP_Input,
                                                           hid_constants::time_and_date::usage_page,
                                                           HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                           hid_constants::time_and_date::usage::year,
                                                           &caps,
                                                           &count,
                                                           m_preparsed_data));
    m_report_ids.get_time_report_id = caps.ReportID;

    // Each time report is built or parsed as a unit, so every field has to sit in the report the year field named.
    NT_RETURN_IF_NTSTATUS_FAILED(verify_time_report(HidP_Output, m_report_ids.set_time_report_id));
    NT_RETURN_IF_NTSTATUS_FAILED(verify_time_report(HidP_Input, m_report_ids.get_time_report_id));

    // ACPI reports "time zone unknown" as a sentinel value, which HID expresses as a null state.
    // Figure out what the device thinks the null state is for time zone
    count = 1;
    NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetSpecificValueCaps(HidP_Output,
                                                           hid_constants::time_and_date::usage_page,
                                                           HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                           hid_constants::time_and_date::usage::time_zone_offset_from_utc,
                                                           &caps,
                                                           &count,
                                                           m_preparsed_data));

    NT_RETURN_IF_NTSTATUS_FAILED(null_value_for_field(caps, &m_time_zone_null_value));

    NT_RETURN_IF_NTSTATUS_FAILED(load_wake_timer_fields());

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::verify_time_report(HIDP_REPORT_TYPE report_type, UINT8 report_id)
{
    for (const USAGE usage : required_time_value_usages)
    {
        HIDP_VALUE_CAPS caps{};
        USHORT count = 1;
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetSpecificValueCaps(report_type,
                                                               hid_constants::time_and_date::usage_page,
                                                               HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                               usage,
                                                               &caps,
                                                               &count,
                                                               m_preparsed_data));

        if (caps.ReportID != report_id)
        {
            NT_RETURN_NTSTATUS(STATUS_DEVICE_CONFIGURATION_ERROR);
        }
    }

    for (const USAGE usage : required_time_button_usages)
    {
        HIDP_BUTTON_CAPS caps{};
        USHORT count = 1;
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetSpecificButtonCaps(report_type,
                                                                hid_constants::time_and_date::usage_page,
                                                                HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                                usage,
                                                                &caps,
                                                                &count,
                                                                m_preparsed_data));

        if (caps.ReportID != report_id)
        {
            NT_RETURN_NTSTATUS(STATUS_DEVICE_CONFIGURATION_ERROR);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::load_wake_timer_field(USAGE usage, WakeTimerFieldInfo *field)
{
    *field = {};
    field->usage = usage;

    USHORT input_caps_count = 1;
    const NTSTATUS input_status = HidP_GetSpecificValueCaps(HidP_Input,
                                                            hid_constants::generic_desktop::usage_page,
                                                            HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                            usage,
                                                            &field->input_caps,
                                                            &input_caps_count,
                                                            m_preparsed_data);

    USHORT output_caps_count = 1;
    const NTSTATUS output_status = HidP_GetSpecificValueCaps(HidP_Output,
                                                             hid_constants::generic_desktop::usage_page,
                                                             HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                             usage,
                                                             &field->output_caps,
                                                             &output_caps_count,
                                                             m_preparsed_data);

    // A device that implements neither half simply has no wake timer, which load_tad_capabilities reports as such.
    if (!NT_SUCCESS(input_status) && !NT_SUCCESS(output_status))
    {
        return STATUS_SUCCESS;
    }

    // A timer the host can set but not read back, or the reverse, cannot serve the TAD contract, so the
    // descriptor is unusable rather than describing a device without the feature.
    NT_RETURN_IF_NTSTATUS_FAILED(input_status);
    NT_RETURN_IF_NTSTATUS_FAILED(output_status);

    if ((field->input_caps.LogicalMin < 0) || (field->output_caps.LogicalMin < 0))
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_CONFIGURATION_ERROR);
    }

    // Fails the start rather than waiting for a disable request we would have no way to encode.
    NT_RETURN_IF_NTSTATUS_FAILED(null_value_for_field(field->output_caps, &field->null_value));

    field->present = true;

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::load_wake_timer_fields()
{
    NT_RETURN_IF_NTSTATUS_FAILED(
        load_wake_timer_field(hid_constants::generic_desktop::usage::timer_expiration_external_power, &m_ac_wake_timer));
    NT_RETURN_IF_NTSTATUS_FAILED(
        load_wake_timer_field(hid_constants::generic_desktop::usage::timer_expiration_internal_power, &m_dc_wake_timer));
    NT_RETURN_IF_NTSTATUS_FAILED(
        load_wake_timer_field(hid_constants::generic_desktop::usage::power_source_change_minimum_expiration, &m_wake_timer_policy));

    return STATUS_SUCCESS;
}

const WakeTimerFieldInfo &
DeviceContext::wake_timer_for_identifier(ULONG timer_identifier) const
{
    NT_ASSERT(is_timer_identifier_valid(timer_identifier));

    return (timer_identifier == ac_timer_identifier) ? m_ac_wake_timer : m_dc_wake_timer;
}

NTSTATUS
DeviceContext::open_collection_file()
{
    wil::unique_wdf_memory name_memory;
    NT_RETURN_IF_NTSTATUS_FAILED(WdfDeviceAllocAndQueryProperty(
        m_device, DevicePropertyPhysicalDeviceObjectName, NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, name_memory.put()));

    size_t name_bytes = 0;
    auto *name_buffer = static_cast<WCHAR *>(WdfMemoryGetBuffer(name_memory.get(), &name_bytes));
    if ((name_buffer == nullptr) || (name_bytes < sizeof(WCHAR)) || (name_buffer[(name_bytes / sizeof(WCHAR)) - 1] != L'\0'))
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
    }

    UNICODE_STRING pdo_name;
    RtlInitUnicodeString(&pdo_name, name_buffer);

    OBJECT_ATTRIBUTES object_attributes;
    InitializeObjectAttributes(&object_attributes, &pdo_name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);

    // Opening our own PDO by name resolves to the top of this stack, so the create comes back down through us and on to
    // HIDCLASS, which is the only way it grants the read/write access that report requests are checked against.
    HANDLE file_handle{};
    IO_STATUS_BLOCK io_status{};
    NT_RETURN_IF_NTSTATUS_FAILED(ZwOpenFile(&file_handle,
                                            FILE_READ_DATA | FILE_WRITE_DATA,
                                            &object_attributes,
                                            &io_status,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            FILE_NON_DIRECTORY_FILE));

    auto handle_cleanup = wil::scope_exit([&] { ZwClose(file_handle); });

    // Only the file object outlives the handle; it is what we stamp onto our own requests.
    NT_RETURN_IF_NTSTATUS_FAILED(ObReferenceObjectByHandle(
        file_handle, 0, *IoFileObjectType, KernelMode, reinterpret_cast<void **>(&m_collection_file_object), nullptr));

    return STATUS_SUCCESS;
}

// Caller holds m_device_lock.
NTSTATUS
DeviceContext::ensure_collection_file()
{
    // Deferred to first use because the create has to travel back down through our own stack, which cannot happen
    // while we are still inside PnP start. WDF dispatches that create inline with no callbacks of ours, so it never
    // re-enters anything that takes this lock.
    if (m_collection_file_object == nullptr)
    {
        NT_RETURN_IF_NTSTATUS_FAILED(open_collection_file());
    }

    return STATUS_SUCCESS;
}

// Caller holds m_device_lock.
void
DeviceContext::close_collection_file()
{
    if (m_collection_file_object == nullptr)
    {
        return;
    }

    // Dropping the last reference is what makes the I/O manager send the close, which travels back down through us.
    ObDereferenceObject(m_collection_file_object);
    m_collection_file_object = nullptr;
}

// Caller holds m_device_lock.
NTSTATUS
DeviceContext::send_output_report(WDFMEMORY report_memory)
{
    if (!m_hardware_ready)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_NOT_READY);
    }

    // A write carries the report exactly as HidD_SetOutputReport's buffer does: report ID first, padded to
    // OutputReportByteLength.
    WDF_MEMORY_DESCRIPTOR report_descriptor;
    WDF_MEMORY_DESCRIPTOR_INIT_HANDLE(&report_descriptor, report_memory, nullptr);

    WDF_REQUEST_SEND_OPTIONS options;
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, hid_operation_timeout);

    NT_RETURN_IF_NTSTATUS_FAILED(
        WdfIoTargetSendWriteSynchronously(WdfDeviceGetIoTarget(m_device), nullptr, &report_descriptor, nullptr, &options, nullptr));

    return STATUS_SUCCESS;
}

// Caller holds m_device_lock.
NTSTATUS
DeviceContext::request_report(ReadReportType report_type, UINT8 report_id, WDFMEMORY report_memory)
{
    if (!m_hardware_ready)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_NOT_READY);
    }

    // HIDCLASS refuses report requests that arrive without a file object.
    NT_RETURN_IF_NTSTATUS_FAILED(ensure_collection_file());

    size_t report_length = 0;
    auto *report = static_cast<UCHAR *>(WdfMemoryGetBuffer(report_memory, &report_length));
    if (report_length == 0)
    {
        NT_RETURN_NTSTATUS(STATUS_BUFFER_TOO_SMALL);
    }

    report[0] = report_id;

    // Mirrors HidD_GetInputReport/HidD_GetFeature, which hand the same buffer to both slots: the report ID travels down
    // in the buffered input copy and HIDCLASS writes the answer back through the METHOD_OUT_DIRECT MDL.
    ULONG ioctl_code = 0;
    switch (report_type)
    {
    case ReadReportType::Input:
        ioctl_code = IOCTL_HID_GET_INPUT_REPORT;
        break;
    case ReadReportType::Feature:
        ioctl_code = IOCTL_HID_GET_FEATURE;
        break;
    }

    WDFIOTARGET target = WdfDeviceGetIoTarget(m_device);
    wil::unique_wdf_any<WDFREQUEST> request;
    NT_RETURN_IF_NTSTATUS_FAILED(WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, target, request.put()));

    NT_RETURN_IF_NTSTATUS_FAILED(
        WdfIoTargetFormatRequestForIoctl(target, request.get(), ioctl_code, report_memory, nullptr, report_memory, nullptr));

    ULONG_PTR bytes_returned = 0;
    NT_RETURN_IF_NTSTATUS_FAILED(send_synchronously(target, request.get(), m_collection_file_object, &bytes_returned));

    // An empty answer is the HID-over-I2C reset indication rather than a report.
    if (bytes_returned == 0)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
    }

    if (bytes_returned > report_length)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
    }

    // We got back a different report than we requested, likely due to a device-side bug
    if (report[0] != report_id)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
    }

    TraceLoggingWrite(g_hTraceProvider,
                      "ReportRequested",
                      TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                      TraceLoggingValue(static_cast<int>(report_type), "ReportType"),
                      TraceLoggingValue(report_id, "ReportId"));

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::get_time(ACPI_REAL_TIME *out)
{
    TraceLoggingWrite(g_hTraceProvider, "GetTimeRequested", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    if (!out)
    {
        return STATUS_INVALID_PARAMETER;
    }

    auto lock = m_device_lock.acquire();

    if (!m_hardware_ready)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_NOT_READY);
    }

    const ULONG report_length = m_hid_capabilities.InputReportByteLength;
    wil::unique_wdf_memory report_memory;
    PCHAR report{};
    NT_RETURN_IF_NTSTATUS_FAILED(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                                 NonPagedPoolNx,
                                                 pool_tag,
                                                 report_length,
                                                 report_memory.put(),
                                                 reinterpret_cast<void **>(&report)));
    RtlZeroMemory(report, report_length);

    NT_RETURN_IF_NTSTATUS_FAILED(request_report(ReadReportType::Input, m_report_ids.get_time_report_id, report_memory.get()));

    RtlZeroMemory(out, sizeof(*out));

    // ACPI_REAL_TIME's fields are all different widths, so each one needs its own narrowing store.
    const struct
    {
        USAGE usage;
        void (*assign)(ACPI_REAL_TIME *, ULONG);
    } value_fields[] = {
        {hid_constants::time_and_date::usage::year, [](ACPI_REAL_TIME *t, ULONG v) { t->Year = static_cast<USHORT>(v); }},
        {hid_constants::time_and_date::usage::month, [](ACPI_REAL_TIME *t, ULONG v) { t->Month = static_cast<UCHAR>(v); }},
        {hid_constants::time_and_date::usage::day, [](ACPI_REAL_TIME *t, ULONG v) { t->Day = static_cast<UCHAR>(v); }},
        {hid_constants::time_and_date::usage::hour, [](ACPI_REAL_TIME *t, ULONG v) { t->Hour = static_cast<UCHAR>(v); }},
        {hid_constants::time_and_date::usage::minute, [](ACPI_REAL_TIME *t, ULONG v) { t->Minute = static_cast<UCHAR>(v); }},
        {hid_constants::time_and_date::usage::second, [](ACPI_REAL_TIME *t, ULONG v) { t->Second = static_cast<UCHAR>(v); }},
    };

    for (const auto &field : value_fields)
    {
        ULONG value = 0;
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetUsageValue(HidP_Input,
                                                        hid_constants::time_and_date::usage_page,
                                                        HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                        field.usage,
                                                        &value,
                                                        m_preparsed_data,
                                                        report,
                                                        report_length));
        field.assign(out, value);
    }

    // Not all devices keep millisecond resolution, so an absent field leaves the zero we already wrote.
    ULONG milliseconds = 0;
    if (NT_SUCCESS(HidP_GetUsageValue(HidP_Input,
                                      hid_constants::time_and_date::usage_page,
                                      HIDP_LINK_COLLECTION_UNSPECIFIED,
                                      hid_constants::time_and_date::usage::millisecond,
                                      &milliseconds,
                                      m_preparsed_data,
                                      report,
                                      report_length)))
    {
        out->Milliseconds = static_cast<USHORT>(milliseconds);
    }

    // Time zone is signed so we have to do some extra stuff to preserve that. A device with no time zone to report
    // leaves the field in its null state, which ACPI spells as ACPI_TIME_ZONE_UNKNOWN.
    LONG time_zone = 0;
    const NTSTATUS time_zone_status = HidP_GetScaledUsageValue(HidP_Input,
                                                               hid_constants::time_and_date::usage_page,
                                                               HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                               hid_constants::time_and_date::usage::time_zone_offset_from_utc,
                                                               &time_zone,
                                                               m_preparsed_data,
                                                               report,
                                                               report_length);
    if (time_zone_status == HIDP_STATUS_NULL)
    {
        out->TimeZone = ACPI_TIME_ZONE_UNKNOWN;
    }
    else
    {
        NT_RETURN_IF_NTSTATUS_FAILED(time_zone_status);

        if ((time_zone < acpi_time_zone_min) || (time_zone > acpi_time_zone_max))
        {
            NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
        }

        out->TimeZone = static_cast<SHORT>(time_zone);
    }

    // See the DayLight bitmask discussion in set_time.
    BOOLEAN dst_observed = FALSE;
    NT_RETURN_IF_NTSTATUS_FAILED(HidPExt_GetButtonValue(HidP_Input,
                                                        hid_constants::time_and_date::usage_page,
                                                        HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                        hid_constants::time_and_date::usage::dst_observed,
                                                        &dst_observed,
                                                        m_preparsed_data,
                                                        report,
                                                        report_length));

    BOOLEAN dst_active = FALSE;
    NT_RETURN_IF_NTSTATUS_FAILED(HidPExt_GetButtonValue(HidP_Input,
                                                        hid_constants::time_and_date::usage_page,
                                                        HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                        hid_constants::time_and_date::usage::dst_active,
                                                        &dst_active,
                                                        m_preparsed_data,
                                                        report,
                                                        report_length));

    out->DayLight = static_cast<UCHAR>((dst_observed ? ACPI_TIME_ADJUST_DAYLIGHT : 0) | (dst_active ? ACPI_TIME_IN_DAYLIGHT : 0));
    out->Valid = 1;

    TraceLoggingWrite(g_hTraceProvider,
                      "GetTimeCompleted",
                      TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                      TraceLoggingValue(out->Year, "Year"),
                      TraceLoggingValue(out->Month, "Month"),
                      TraceLoggingValue(out->Day, "Day"),
                      TraceLoggingValue(out->Hour, "Hour"),
                      TraceLoggingValue(out->Minute, "Minute"),
                      TraceLoggingValue(out->Second, "Second"),
                      TraceLoggingValue(out->Milliseconds, "Milliseconds"),
                      TraceLoggingValue(out->DayLight, "DayLight"));

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::set_time(const ACPI_REAL_TIME *in)
{
    TraceLoggingWrite(g_hTraceProvider, "SetTimeRequested", TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE));

    if (!in)
    {
        return STATUS_INVALID_PARAMETER;
    }

    auto lock = m_device_lock.acquire();

    if (!m_hardware_ready)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_NOT_READY);
    }

    const ULONG report_length = m_hid_capabilities.OutputReportByteLength;
    wil::unique_wdf_memory report_memory;
    PCHAR report{};
    NT_RETURN_IF_NTSTATUS_FAILED(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                                 NonPagedPoolNx,
                                                 pool_tag,
                                                 report_length,
                                                 report_memory.put(),
                                                 reinterpret_cast<void **>(&report)));

    // TODO switch to HidP_InitializeReportForIdEx when/if it becomes available
    initialize_output_report(report, report_length, m_report_ids.set_time_report_id);
    // NT_RETURN_IF_NTSTATUS_FAILED(
    //     HidP_InitializeReportForIdEx(HidP_Output, m_report_ids.set_time_report_id, m_preparsed_data, report, report_length));

    const struct
    {
        USAGE usage;
        ULONG value;
    } value_fields[] = {
        {hid_constants::time_and_date::usage::year, in->Year},
        {hid_constants::time_and_date::usage::month, in->Month},
        {hid_constants::time_and_date::usage::day, in->Day},
        {hid_constants::time_and_date::usage::hour, in->Hour},
        {hid_constants::time_and_date::usage::minute, in->Minute},
        {hid_constants::time_and_date::usage::second, in->Second},
    };

    for (const auto &field : value_fields)
    {
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_SetUsageValue(HidP_Output,
                                                        hid_constants::time_and_date::usage_page,
                                                        HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                        field.usage,
                                                        field.value,
                                                        m_preparsed_data,
                                                        report,
                                                        report_length));
    }

    // Not all devices support millisecond resolution, so it may be expected that this call fails.
    //
    (void)HidP_SetUsageValue(HidP_Output,
                             hid_constants::time_and_date::usage_page,
                             HIDP_LINK_COLLECTION_UNSPECIFIED,
                             hid_constants::time_and_date::usage::millisecond,
                             in->Milliseconds,
                             m_preparsed_data,
                             report,
                             report_length);

    const struct
    {
        USAGE usage;
        BOOLEAN value;
    } button_fields[] = {
        {hid_constants::time_and_date::usage::dst_observed, static_cast<BOOLEAN>((in->DayLight & ACPI_TIME_ADJUST_DAYLIGHT) != 0)},
        {hid_constants::time_and_date::usage::dst_active, static_cast<BOOLEAN>((in->DayLight & ACPI_TIME_IN_DAYLIGHT) != 0)},
    };
    for (const auto &field : button_fields)
    {
        NT_RETURN_IF_NTSTATUS_FAILED(HidPExt_SetButtonValue(HidP_Output,
                                                            hid_constants::time_and_date::usage_page,
                                                            HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                            field.usage,
                                                            field.value,
                                                            m_preparsed_data,
                                                            report,
                                                            report_length));
    }

    // NOTE: windows has changed its default behavior around time zone reporting.  It looks like by default it always sets the
    // time-alarm device to UTC and maintains timezone/DST state in the registry.  This means that we'll proably get called here
    // with time zone set to UTC, even when the time zone is being manipulated in settings.
    //
    // Preliminary research indicates that manipulating this regkey may impact this behavior:
    //
    //   "HKEY_LOCAL_MACHINE\System\CurrentControlSet\Control\TimeZoneInformation" /v RealTimeIsUniversal /t REG_DWORD /d 1
    //
    // but haven't tested this extensively.  If you're not seeing a time zone get set on the EC side, that's likely why.
    // We do still maintain a path for setting time zone in case users do manipulate this setting, though.

    // Time zone is signed so we have to do some extra stuff to preserve that. ACPI_TIME_ZONE_UNKNOWN is a sentinel
    // rather than an offset, so it goes down as the field's null state; the scaled setter cannot be used for that
    // because it only reaches null by failing a range check, and the descriptor's range may contain the sentinel.
    if (in->TimeZone == ACPI_TIME_ZONE_UNKNOWN)
    {
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_SetUsageValue(HidP_Output,
                                                        hid_constants::time_and_date::usage_page,
                                                        HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                        hid_constants::time_and_date::usage::time_zone_offset_from_utc,
                                                        m_time_zone_null_value,
                                                        m_preparsed_data,
                                                        report,
                                                        report_length));
    }
    else
    {
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_SetScaledUsageValue(HidP_Output,
                                                              hid_constants::time_and_date::usage_page,
                                                              HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                              hid_constants::time_and_date::usage::time_zone_offset_from_utc,
                                                              in->TimeZone,
                                                              m_preparsed_data,
                                                              report,
                                                              report_length));
    }

    NT_RETURN_IF_NTSTATUS_FAILED(send_output_report(report_memory.get()));

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::read_wake_timer_field(const WakeTimerFieldInfo &field, ULONG *timeout)
{
    // field aliases descriptor state, so the lock has to precede even the presence check.
    auto lock = m_device_lock.acquire();

    if (!m_hardware_ready)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_NOT_READY);
    }

    if (!field.present)
    {
        NT_RETURN_NTSTATUS(STATUS_NOT_SUPPORTED);
    }

    const ULONG report_length = m_hid_capabilities.InputReportByteLength;
    wil::unique_wdf_memory report_memory;
    PCHAR report = nullptr;
    NT_RETURN_IF_NTSTATUS_FAILED(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                                 NonPagedPoolNx,
                                                 pool_tag,
                                                 report_length,
                                                 report_memory.put(),
                                                 reinterpret_cast<void **>(&report)));
    RtlZeroMemory(report, report_length);

    NT_RETURN_IF_NTSTATUS_FAILED(request_report(ReadReportType::Input, field.input_caps.ReportID, report_memory.get()));

    ULONG value = 0;
    NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetUsageValue(HidP_Input,
                                                    hid_constants::generic_desktop::usage_page,
                                                    HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                    field.usage,
                                                    &value,
                                                    m_preparsed_data,
                                                    report,
                                                    report_length));

    // Every field carries a null state, so a value outside the logical range means unconfigured.
    const auto raw_value = static_cast<LONG64>(value);
    const bool is_set = (raw_value >= field.input_caps.LogicalMin) && (raw_value <= field.input_caps.LogicalMax);
    *timeout = is_set ? value : wake_alarm_timeout_disabled;

    TraceLoggingWrite(g_hTraceProvider,
                      "WakeTimerFieldRead",
                      TraceLoggingLevel(WINEVENT_LEVEL_VERBOSE),
                      TraceLoggingValue(field.usage, "Usage"),
                      TraceLoggingValue(*timeout, "Timeout"));

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::write_wake_timer_field(const WakeTimerFieldInfo &field, ULONG timeout)
{
    auto lock = m_device_lock.acquire();

    if (!m_hardware_ready)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_NOT_READY);
    }

    if (!field.present)
    {
        NT_RETURN_NTSTATUS(STATUS_NOT_SUPPORTED);
    }

    ULONG value = timeout;
    if (value == wake_alarm_timeout_disabled)
    {
        value = field.null_value;
    }
    else
    {
        const auto raw_value = static_cast<LONG64>(value);
        if ((raw_value < field.output_caps.LogicalMin) || (raw_value > field.output_caps.LogicalMax))
        {
            NT_RETURN_NTSTATUS(STATUS_INVALID_PARAMETER);
        }
    }

    const ULONG report_length = m_hid_capabilities.OutputReportByteLength;
    wil::unique_wdf_memory report_memory;
    PCHAR report = nullptr;
    NT_RETURN_IF_NTSTATUS_FAILED(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                                 NonPagedPoolNx,
                                                 pool_tag,
                                                 report_length,
                                                 report_memory.put(),
                                                 reinterpret_cast<void **>(&report)));
    RtlZeroMemory(report, report_length);

    // TODO switch to HidP_InitializeReportForIdEx when/if it becomes available
    initialize_output_report(report, report_length, field.output_caps.ReportID);
    // NT_RETURN_IF_NTSTATUS_FAILED(
    //     HidP_InitializeReportForID(HidP_Output, field.output_caps.ReportID, m_preparsed_data, report, report_length));

    NT_RETURN_IF_NTSTATUS_FAILED(HidP_SetUsageValue(HidP_Output,
                                                    hid_constants::generic_desktop::usage_page,
                                                    HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                    field.usage,
                                                    value,
                                                    m_preparsed_data,
                                                    report,
                                                    report_length));

    return send_output_report(report_memory.get());
}

NTSTATUS
DeviceContext::get_wake_alarm_value(ULONG timer_identifier, WAKE_ALARM_INFORMATION *out)
{
    if (!is_timer_identifier_valid(timer_identifier))
    {
        NT_RETURN_NTSTATUS(STATUS_INVALID_PARAMETER);
    }

    NT_RETURN_IF_NTSTATUS_FAILED(read_wake_timer_field(wake_timer_for_identifier(timer_identifier), &out->Timeout));
    out->TimerIdentifier = timer_identifier;

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::set_wake_alarm_value(const WAKE_ALARM_INFORMATION *in)
{
    if (!is_timer_identifier_valid(in->TimerIdentifier))
    {
        NT_RETURN_NTSTATUS(STATUS_INVALID_PARAMETER);
    }

    return write_wake_timer_field(wake_timer_for_identifier(in->TimerIdentifier), in->Timeout);
}

NTSTATUS
DeviceContext::get_wake_alarm_policy(ULONG timer_identifier, WAKE_ALARM_INFORMATION *out)
{
    if (!is_timer_identifier_valid(timer_identifier))
    {
        NT_RETURN_NTSTATUS(STATUS_INVALID_PARAMETER);
    }

    // A single policy covers both timers, so the identifier only picks which one the answer is labelled with.
    NT_RETURN_IF_NTSTATUS_FAILED(read_wake_timer_field(m_wake_timer_policy, &out->Timeout));
    out->TimerIdentifier = timer_identifier;

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::set_wake_alarm_policy(const WAKE_ALARM_INFORMATION *in)
{
    if (!is_timer_identifier_valid(in->TimerIdentifier))
    {
        NT_RETURN_NTSTATUS(STATUS_INVALID_PARAMETER);
    }

    // One policy covers both timers, so the identifier only says which caller asked.
    return write_wake_timer_field(m_wake_timer_policy, in->Timeout);
}

// Deferred out of start because the deepest wake state comes from a feature report, and reading one needs our
// collection file object, which cannot be opened until the device is started. Caller holds m_device_lock.
NTSTATUS
DeviceContext::load_tad_capabilities()
{
    if (m_tad_capabilities_loaded)
    {
        return STATUS_SUCCESS;
    }

    if (!m_hardware_ready)
    {
        NT_RETURN_NTSTATUS(STATUS_DEVICE_NOT_READY);
    }

    m_tad_capabilities = {};
    m_tad_capabilities.RealTimeFeaturesSupported = TRUE;

    {
        const struct
        {
            HIDP_REPORT_TYPE report_type;
            UINT8 report_id;
        } time_reports[] = {
            {HidP_Input, m_report_ids.get_time_report_id},
            {HidP_Output, m_report_ids.set_time_report_id},
        };

        bool ms_supported = true;
        for (const auto &time_report : time_reports)
        {
            HIDP_VALUE_CAPS value_caps{};
            USHORT value_caps_count = 1;
            ms_supported = ms_supported &&
                           NT_SUCCESS(HidP_GetSpecificValueCaps(time_report.report_type,
                                                                hid_constants::time_and_date::usage_page,
                                                                HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                                hid_constants::time_and_date::usage::millisecond,
                                                                &value_caps,
                                                                &value_caps_count,
                                                                m_preparsed_data)) &&
                           time_report.report_id == value_caps.ReportID;
        }

        m_tad_capabilities.RealTimeResolution = ms_supported ? AcpiTimeResolutionMilliseconds : AcpiTimeResolutionSeconds;
    }

    m_tad_capabilities.AcWakeSupported = m_ac_wake_timer.present;
    m_tad_capabilities.DcWakeSupported = m_dc_wake_timer.present;

    // A fired alarm can be reported back after S4/S5 only if the timer's input report carries the expired state.
    // The state is an array item, which HIDP surfaces as button caps rather than value caps.
    HIDP_BUTTON_CAPS expired_caps{};
    USHORT expired_caps_count = 1;
    m_tad_capabilities.S4S5WakeStatusSupported =
        NT_SUCCESS(HidP_GetSpecificButtonCaps(HidP_Input,
                                              hid_constants::generic_device_controls::usage_page,
                                              HIDP_LINK_COLLECTION_UNSPECIFIED,
                                              hid_constants::generic_device_controls::usage::expired,
                                              &expired_caps,
                                              &expired_caps_count,
                                              m_preparsed_data));

    // Only a device that can wake has a deepest wake state to report, so the rest costs a feature report.
    if (m_tad_capabilities.AcWakeSupported || m_tad_capabilities.DcWakeSupported)
    {
        HIDP_BUTTON_CAPS wake_state_caps{};
        USHORT wake_state_caps_count = 1;
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetSpecificButtonCaps(HidP_Feature,
                                                                hid_constants::generic_desktop::usage_page,
                                                                HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                                0,
                                                                &wake_state_caps,
                                                                &wake_state_caps_count,
                                                                m_preparsed_data));

        const ULONG report_length = m_hid_capabilities.FeatureReportByteLength;
        wil::unique_wdf_memory report_memory;
        PCHAR report = nullptr;
        NT_RETURN_IF_NTSTATUS_FAILED(WdfMemoryCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                                     NonPagedPoolNx,
                                                     pool_tag,
                                                     report_length,
                                                     report_memory.put(),
                                                     reinterpret_cast<void **>(&report)));
        RtlZeroMemory(report, report_length);

        NT_RETURN_IF_NTSTATUS_FAILED(request_report(ReadReportType::Feature, wake_state_caps.ReportID, report_memory.get()));

        USAGE selector{};
        ULONG selector_count = 1;
        NT_RETURN_IF_NTSTATUS_FAILED(HidP_GetUsages(HidP_Feature,
                                                    hid_constants::generic_desktop::usage_page,
                                                    HIDP_LINK_COLLECTION_UNSPECIFIED,
                                                    &selector,
                                                    &selector_count,
                                                    m_preparsed_data,
                                                    report,
                                                    report_length));

        if ((selector_count != 1) || (selector < hid_constants::generic_desktop::usage::s1) ||
            (selector > hid_constants::generic_desktop::usage::s5))
        {
            NT_RETURN_NTSTATUS(STATUS_DEVICE_DATA_ERROR);
        }

        // S1 through S5 are contiguous in both the usage page and SYSTEM_POWER_STATE.
        const auto deepest_state =
            static_cast<SYSTEM_POWER_STATE>(PowerSystemSleeping1 + (selector - hid_constants::generic_desktop::usage::s1));

        m_tad_capabilities.DeepestWakeSystemState = deepest_state;

        m_tad_capabilities.S4AcWakeSupported = deepest_state >= PowerSystemHibernate && m_tad_capabilities.AcWakeSupported;
        m_tad_capabilities.S4DcWakeSupported = deepest_state >= PowerSystemHibernate && m_tad_capabilities.DcWakeSupported;

        m_tad_capabilities.S5AcWakeSupported = deepest_state >= PowerSystemShutdown && m_tad_capabilities.AcWakeSupported;
        m_tad_capabilities.S5DcWakeSupported = deepest_state >= PowerSystemShutdown && m_tad_capabilities.DcWakeSupported;
    }

    m_tad_capabilities_loaded = true;

    TraceLoggingWrite(g_hTraceProvider,
                      "CapabilitiesLoaded",
                      TraceLoggingLevel(WINEVENT_LEVEL_INFO),
                      TraceLoggingValue(m_tad_capabilities.AcWakeSupported, "AcWakeSupported"),
                      TraceLoggingValue(m_tad_capabilities.DcWakeSupported, "DcWakeSupported"),
                      TraceLoggingValue(m_tad_capabilities.S4AcWakeSupported, "S4AcWakeSupported"),
                      TraceLoggingValue(m_tad_capabilities.S4DcWakeSupported, "S4DcWakeSupported"),
                      TraceLoggingValue(m_tad_capabilities.S5AcWakeSupported, "S5AcWakeSupported"),
                      TraceLoggingValue(m_tad_capabilities.S5DcWakeSupported, "S5DcWakeSupported"),
                      TraceLoggingValue(m_tad_capabilities.S4S5WakeStatusSupported, "S4S5WakeStatusSupported"),
                      TraceLoggingValue(m_tad_capabilities.DeepestWakeSystemState, "DeepestWakeSystemState"),
                      TraceLoggingValue(m_tad_capabilities.RealTimeFeaturesSupported, "RealTimeFeaturesSupported"),
                      TraceLoggingValue(static_cast<int>(m_tad_capabilities.RealTimeResolution), "RealTimeResolution"));

    return STATUS_SUCCESS;
}

NTSTATUS
DeviceContext::get_tad_capabilities(ACPI_TIME_AND_ALARM_CAPABILITIES *out)
{
    auto lock = m_device_lock.acquire();

    NT_RETURN_IF_NTSTATUS_FAILED(load_tad_capabilities());
    *out = m_tad_capabilities;

    return STATUS_SUCCESS;
}