#pragma once

#include "hidusage.h"

// Constants from the draft HUTTR doc // TODO update this comment when it ships and reference a page number
namespace hid_constants
{
namespace generic_desktop
{
inline constexpr USAGE usage_page = 0x01;
namespace usage
{
inline constexpr USAGE real_time_clock = 0x14;
inline constexpr USAGE timer_expiration_external_power = 0xF0;
inline constexpr USAGE timer_expiration_internal_power = 0xF1;
inline constexpr USAGE power_source_change_minimum_expiration = 0xF2;
inline constexpr USAGE s1 = 0xF4;
inline constexpr USAGE s5 = 0xF8;
} // namespace usage
} // namespace generic_desktop

namespace generic_device_controls
{
inline constexpr USAGE usage_page = 0x06;
namespace usage
{
inline constexpr USAGE expired = 0x55;
} // namespace usage
} // namespace generic_device_controls

namespace time_and_date
{
inline constexpr USAGE usage_page = 0x13;
namespace usage
{
inline constexpr USAGE year = 0x01;
inline constexpr USAGE month = 0x02;
inline constexpr USAGE day = 0x03;
inline constexpr USAGE hour = 0x04;
inline constexpr USAGE minute = 0x05;
inline constexpr USAGE second = 0x06;
inline constexpr USAGE millisecond = 0x07;
inline constexpr USAGE time_zone_offset_from_utc = 0x10;
inline constexpr USAGE dst_observed = 0x11;
inline constexpr USAGE dst_active = 0x12;

} // namespace usage
} // namespace time_and_date

} // namespace hid_constants