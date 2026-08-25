#pragma once

#include <ntddk.h>
#include <wdf.h>

#include <new.h>

#include <hidusage.h> // Must be included before hidpi.h

#include <hidclass.h>
#include <hidpi.h>
#include <poclass.h>

#include "km_wil_result_macros.h"

#include <wil/resource.h>

// Only the time reports need this; the wake timer report IDs come from the cached wake timer caps.
struct ReportIDs
{
    UINT8 set_time_report_id;
    UINT8 get_time_report_id;
};

enum class ReadReportType
{
    Input,
    Feature,
};

// What the report descriptor says about one wake timer field, resolved once at start.
struct WakeTimerFieldInfo
{
    USAGE usage; // names the field in both its input and its output report
    bool present;
    HIDP_VALUE_CAPS input_caps;
    HIDP_VALUE_CAPS output_caps;
    ULONG null_value; // what to write to clear the field, derived from its logical range
};

class DeviceContext final
{
  public:
    static NTSTATUS
    create(WDFDRIVER driver, PWDFDEVICE_INIT deviceInit);

  private:
    static EVT_WDF_OBJECT_CONTEXT_DESTROY on_destroy;
    static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL on_ioctl;
    static EVT_WDF_DEVICE_PREPARE_HARDWARE on_prepare_hardware;
    static EVT_WDF_DEVICE_RELEASE_HARDWARE on_release_hardware;
    static EVT_WDF_DEVICE_QUERY_REMOVE on_query_remove;
    static EVT_WDF_DEVICE_SURPRISE_REMOVAL on_surprise_removal;

    NTSTATUS
    load_descriptor();

    NTSTATUS
    find_value_caps(HIDP_REPORT_TYPE report_type, USAGE usage_page, USAGE usage, HIDP_VALUE_CAPS *caps);

    // Checks that a time report carries every field the TAD contract needs, all under the given report ID.
    NTSTATUS
    verify_time_report(HIDP_REPORT_TYPE report_type, UINT8 report_id);

    NTSTATUS
    load_wake_timer_field(USAGE usage, WakeTimerFieldInfo *field);

    NTSTATUS
    load_wake_timer_fields();

    NTSTATUS
    open_collection_file();

    NTSTATUS
    ensure_collection_file();

    void
    close_collection_file();

    NTSTATUS
    send_output_report(WDFMEMORY report_memory);

    NTSTATUS
    request_report(ReadReportType report_type, UINT8 report_id, WDFMEMORY report_memory);

    NTSTATUS
    get_time(ACPI_REAL_TIME *out);

    NTSTATUS
    set_time(const ACPI_REAL_TIME *in);

    NTSTATUS
    read_wake_timer_field(const WakeTimerFieldInfo &field, ULONG *timeout);

    NTSTATUS
    write_wake_timer_field(const WakeTimerFieldInfo &field, ULONG timeout);

    const WakeTimerFieldInfo &
    wake_timer_for_identifier(ULONG timer_identifier) const;

    NTSTATUS
    get_wake_alarm_value(ULONG timer_identifier, WAKE_ALARM_INFORMATION *out);

    NTSTATUS
    set_wake_alarm_value(const WAKE_ALARM_INFORMATION *in);

    NTSTATUS
    get_wake_alarm_policy(ULONG timer_identifier, WAKE_ALARM_INFORMATION *out);

    NTSTATUS
    set_wake_alarm_policy(const WAKE_ALARM_INFORMATION *in);

    NTSTATUS
    find_sleep_state_caps(HIDP_BUTTON_CAPS *caps);

    NTSTATUS
    load_tad_capabilities();

    NTSTATUS
    get_tad_capabilities(ACPI_TIME_AND_ALARM_CAPABILITIES *out);

  private:
    WDFDEVICE m_device{};

    // Guards everything cached below.
    wil::unique_wdf_wait_lock m_device_lock{};

    wil::unique_wdf_memory m_preparsed_memory{};
    PHIDP_PREPARSED_DATA m_preparsed_data{};

    HIDP_CAPS m_hid_capabilities{};
    ReportIDs m_report_ids{};

    // How this descriptor spells ACPI_TIME_ZONE_UNKNOWN, derived from the time zone field's logical range.
    ULONG m_time_zone_null_value{};

    // All three share one input report; each has its own output report.
    WakeTimerFieldInfo m_ac_wake_timer{};
    WakeTimerFieldInfo m_dc_wake_timer{};
    WakeTimerFieldInfo m_wake_timer_policy{}; // one policy covers both timers

    // HIDCLASS keys its per-file read/write access off the create, so driver-generated requests have to carry the file
    // object from our own open of the collection.  This is similar to what hidbatt does.
    PFILE_OBJECT m_collection_file_object{};

    // The queue is not power-managed, so requests keep arriving after release-hardware.
    bool m_hardware_ready{};

    ACPI_TIME_AND_ALARM_CAPABILITIES m_tad_capabilities{};
    bool m_tad_capabilities_loaded{};
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext)
