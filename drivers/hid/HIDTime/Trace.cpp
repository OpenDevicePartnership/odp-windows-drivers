#include "Trace.h"
#include "km_wil_result_macros.h"

// {D164408B-462A-49F9-95FC-5EFE7FA9498F}
TRACELOGGING_DEFINE_PROVIDER(g_hTraceProvider,
                             "HIDTime",
                             (0xd164408b, 0x462a, 0x49f9, 0x95, 0xfc, 0x5e, 0xfe, 0x7f, 0xa9, 0x49, 0x8f));

namespace wilkm
{
namespace detail
{
void
LogFailure(const char *expression, const char *file, int line, NTSTATUS ntstatus)
{
    TraceLoggingWrite(g_hTraceProvider,
                      "OperationFailed",
                      TraceLoggingLevel(WINEVENT_LEVEL_ERROR),
                      TraceLoggingValue(ntstatus),
                      TraceLoggingValue(expression, "Expression"),
                      TraceLoggingValue(file, "File"),
                      TraceLoggingValue(line, "Line"));
}
} // namespace detail
} // namespace wilkm