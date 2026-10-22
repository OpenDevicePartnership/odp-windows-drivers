
// Define IOCTL's and structures shared between KMDF and Application
#define IOCTL_GET_NOTIFICATION 0x1
#define IOCTL_READ_RX_BUFFER 0x2

#define SBSAQEMU_SHARED_MEM_BASE 0x10060000000

typedef struct {
    UINT64 count;
    UINT64 timestamp;
    UINT32  lastevent;
} NotificationRsp_t;

typedef struct {
    UINT8 type;
} NotificationReq_t;

typedef struct {
    UINT64 data;
} RxBufferRsp_t;

#ifdef EC_TEST_NATIVE_PCC
#define IOCTL_ECTEST_PCC_PROBE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_ECTEST_PCC_EXECUTE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define ECTEST_PCC_PROTOCOL_VERSION 1
#define ECTEST_PCC_MESSAGE_SIZE 256

typedef struct {
    ULONG Version;
    ULONG SubspaceId;
    ULONG Command;
    ULONG MessageLength;
    UCHAR Message[ECTEST_PCC_MESSAGE_SIZE];
} ECTEST_PCC_EXECUTE_REQUEST;

typedef struct {
    ULONG Version;
    LONG Status;
    UCHAR Message[ECTEST_PCC_MESSAGE_SIZE];
} ECTEST_PCC_EXECUTE_RESPONSE;

typedef struct {
    ULONG Version;
    ULONG SubspaceId;
} ECTEST_PCC_PROBE_REQUEST;

typedef struct {
    ULONG Version;
    LONG QueryStatus;
    ULONG InterfaceVersion;
    ULONG SubspaceId;
    ULONG SubspaceType;
    ULONG SubspaceSize;
    ULONG Flags;
    ULONG NominalLatency;
    ULONG MaximumPeriodicRate;
    ULONG AcpiTimeStamp;
    ULONG AcpiImageSize;
    ULONG Reserved;
} ECTEST_PCC_PROBE_RESPONSE;
#endif
