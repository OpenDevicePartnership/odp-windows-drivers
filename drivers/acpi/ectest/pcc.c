#include "driver.h"
#include "ectest.h"
#include <acpitabl.h>
#include <aux_klib.h>
#include <ntimage.h>
#include <initguid.h>
#include <wdmguid.h>

#define ECTEST_PCC_POOL_TAG 0x63507445

static NTSTATUS ECTestPccCheckImage(PULONG TimeStamp, PULONG ImageSize)
{
    ULONG length = 0;
    PAUX_MODULE_EXTENDED_INFO modules;
    NTSTATUS status = AuxKlibQueryModuleInformation(&length, sizeof(*modules), NULL);

    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (length == 0 || length > 1024 * 1024 || length % sizeof(*modules) != 0) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    modules = ExAllocatePool2(POOL_FLAG_PAGED, length, ECTEST_PCC_POOL_TAG);
    if (modules == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = AuxKlibQueryModuleInformation(&length, sizeof(*modules), modules);
    if (NT_SUCCESS(status)) {
        status = STATUS_NOT_SUPPORTED;
        for (ULONG index = 0; index < length / sizeof(*modules); ++index) {
            const AUX_MODULE_EXTENDED_INFO *module = &modules[index];
            const UCHAR expected[] = "acpi.sys";
            const IMAGE_DOS_HEADER *dos;
            const IMAGE_NT_HEADERS64 *headers;
            BOOLEAN match = TRUE;

            if (module->FileNameOffset > sizeof(module->FullPathName) - sizeof(expected)) {
                continue;
            }
            for (ULONG character = 0; character < sizeof(expected); ++character) {
                UCHAR actual = module->FullPathName[module->FileNameOffset + character];
                if (actual >= 'A' && actual <= 'Z') {
                    actual += 'a' - 'A';
                }
                if (actual != expected[character]) {
                    match = FALSE;
                    break;
                }
            }
            if (!match || module->BasicInfo.ImageBase == NULL ||
                module->ImageSize < sizeof(*dos) + sizeof(*headers)) {
                continue;
            }
            dos = module->BasicInfo.ImageBase;
            if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
                (ULONG)dos->e_lfanew > module->ImageSize - sizeof(*headers)) {
                break;
            }
            headers = (const IMAGE_NT_HEADERS64 *)((const UCHAR *)dos + dos->e_lfanew);
            if (headers->Signature != IMAGE_NT_SIGNATURE ||
                headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
                headers->FileHeader.Machine != IMAGE_FILE_MACHINE_ARM64) {
                break;
            }
            *TimeStamp = headers->FileHeader.TimeDateStamp;
            *ImageSize = headers->OptionalHeader.SizeOfImage;
            if (*TimeStamp == 0xF61FB868 && *ImageSize == 0xE1000) {
                status = STATUS_SUCCESS;
            }
            break;
        }
    }
    ExFreePoolWithTag(modules, ECTEST_PCC_POOL_TAG);
    return status;
}

static EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL ECTestPccDeviceControl;

static VOID ECTestPccEnsureQueried(WDFDEVICE Device, PDEVICE_CONTEXT Context)
{
    if (!Context->PccQueried) {
        Context->PccQueryStatus = ECTestPccQuery(Device, &Context->PccInterface,
            &Context->PccAcpiTimeStamp, &Context->PccAcpiImageSize);
        Context->PccQueried = TRUE;
    }
}

static NTSTATUS ECTestPccExecute(
    PDEVICE_CONTEXT Context,
    const ECTEST_PCC_EXECUTE_REQUEST *Input,
    ECTEST_PCC_EXECUTE_RESPONSE *Output
    )
{
    PECTEST_PCC_NATIVE_INTERFACE pcc = &Context->PccInterface;
    ULONG words[ECTEST_PCC_MESSAGE_SIZE / sizeof(ULONG)] = { 0 };
    NTSTATUS status;

    RtlCopyMemory(words, Input->Message, Input->MessageLength);
    status = pcc->AcquireSubspace(pcc->Handle);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WRITE_REGISTER_BUFFER_ULONG((volatile ULONG *)pcc->Subspace, words,
        (Input->MessageLength + sizeof(ULONG) - 1) / sizeof(ULONG));
    // The doorbell is in a different MMIO window, so the payload must complete first.
    KeMemoryBarrier();
    // ExecuteCommand consumes the acquisition, as in fxppm; ReleaseSubspace would fail afterwards.
    status = pcc->ExecuteCommand(pcc->Handle, (UCHAR)Input->Command);
    if (NT_SUCCESS(status)) {
        READ_REGISTER_BUFFER_ULONG((volatile ULONG *)pcc->Subspace, words, ARRAYSIZE(words));
        RtlCopyMemory(Output->Message, words, sizeof(words));
    }
    return status;
}

static VOID ECTestPccDispatchExecute(
    WDFDEVICE Device,
    PDEVICE_CONTEXT Context,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength
    )
{
    ECTEST_PCC_EXECUTE_REQUEST input;
    ECTEST_PCC_EXECUTE_REQUEST *inputBuffer;
    ECTEST_PCC_EXECUTE_RESPONSE *output;
    NTSTATUS status;

    if (InputBufferLength != sizeof(input) || OutputBufferLength < sizeof(*output)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), (PVOID *)&inputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    input = *inputBuffer;
    if (input.Version != ECTEST_PCC_PROTOCOL_VERSION || input.SubspaceId != 0 ||
        input.Command > MAXUCHAR || input.MessageLength > ECTEST_PCC_MESSAGE_SIZE) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    ECTestPccEnsureQueried(Device, Context);
    RtlZeroMemory(output, sizeof(*output));
    output->Version = ECTEST_PCC_PROTOCOL_VERSION;
    output->Status = NT_SUCCESS(Context->PccQueryStatus) ?
        ECTestPccExecute(Context, &input, output) : Context->PccQueryStatus;
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*output));
}

static VOID ECTestPccDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode
    )
{
    ECTEST_PCC_PROBE_REQUEST input;
    ECTEST_PCC_PROBE_REQUEST *inputBuffer;
    ECTEST_PCC_PROBE_RESPONSE *output;
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT context = DeviceContextGet(device);
    NTSTATUS status;

    PAGED_CODE();
    if (IoControlCode == IOCTL_ECTEST_PCC_EXECUTE) {
        ECTestPccDispatchExecute(device, context, Request, OutputBufferLength, InputBufferLength);
        return;
    }
    if (IoControlCode != IOCTL_ECTEST_PCC_PROBE ||
        InputBufferLength != sizeof(input) || OutputBufferLength < sizeof(*output)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    status = WdfRequestRetrieveInputBuffer(Request, sizeof(input), (PVOID *)&inputBuffer, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    input = *inputBuffer;
    if (input.Version != ECTEST_PCC_PROTOCOL_VERSION || input.SubspaceId != 0) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*output), (PVOID *)&output, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    ECTestPccEnsureQueried(device, context);
    RtlZeroMemory(output, sizeof(*output));
    output->Version = ECTEST_PCC_PROTOCOL_VERSION;
    output->QueryStatus = context->PccQueryStatus;
    output->AcpiTimeStamp = context->PccAcpiTimeStamp;
    output->AcpiImageSize = context->PccAcpiImageSize;
    if (NT_SUCCESS(context->PccQueryStatus)) {
        output->InterfaceVersion = context->PccInterface.Version;
        output->SubspaceId = context->PccInterface.SubspaceId;
        output->SubspaceType = 3;
        output->SubspaceSize = context->PccInterface.SubspaceSize;
        output->Flags = context->PccInterface.Flags;
        output->NominalLatency = context->PccInterface.NominalLatency;
        output->MaximumPeriodicRate = context->PccInterface.MaximumPeriodicRate;
    }
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*output));
}

NTSTATUS ECTestPccQueueInitialize(WDFDEVICE Device)
{
    WDF_IO_QUEUE_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    PAGED_CODE();
    WDF_IO_QUEUE_CONFIG_INIT(&config, WdfIoQueueDispatchSequential);
    config.EvtIoDeviceControl = ECTestPccDeviceControl;
    config.PowerManaged = WdfTrue;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    return WdfIoQueueCreate(Device, &config, &attributes, &DeviceContextGet(Device)->PccQueue);
}

NTSTATUS ECTestPccReleaseHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context = DeviceContextGet(Device);

    PAGED_CODE();
    UNREFERENCED_PARAMETER(ResourcesTranslated);
    if (context->PccQueried && NT_SUCCESS(context->PccQueryStatus) &&
        context->PccInterface.InterfaceDereference != NULL) {
        context->PccInterface.InterfaceDereference(context->PccInterface.Context);
    }
    RtlZeroMemory(&context->PccInterface, sizeof(context->PccInterface));
    context->PccQueried = FALSE;
    context->PccQueryStatus = STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

static BOOLEAN ECTestPccRegisterMatches(const GEN_ADDR *Register, LONGLONG Address)
{
    return Register->AddressSpaceID == AcpiGenericSpaceMemory &&
        Register->BitWidth == 32 && Register->BitOffset == 0 &&
        Register->AccessSize == AcpiGenericAccessSizeDWord &&
        Register->Address.QuadPart == Address;
}

static NTSTATUS ECTestPccCheckTable(VOID)
{
    ULONG length = 0;
    PUCHAR table;
    UCHAR checksum = 0;
    ULONG declaredLength;
    NTSTATUS status = AuxKlibGetSystemFirmwareTable(0x41435049, 0x54434350, NULL, 0, &length);

    if (!NT_SUCCESS(status) && status != STATUS_BUFFER_TOO_SMALL) {
        return status;
    }
    if (length != 376) {
        return STATUS_NOT_SUPPORTED;
    }
    table = ExAllocatePool2(POOL_FLAG_PAGED, length, ECTEST_PCC_POOL_TAG);
    if (table == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    status = AuxKlibGetSystemFirmwareTable(0x41435049, 0x54434350, table, length, &length);
    if (NT_SUCCESS(status) && length == 376) {
        for (ULONG index = 0; index < length; ++index) {
            checksum = (UCHAR)(checksum + table[index]);
        }
        RtlCopyMemory(&declaredLength, table + 4, sizeof(declaredLength));
        if (RtlCompareMemory(table, "PCCT", 4) != 4 || declaredLength != 376 ||
            table[8] != 2 || checksum != 0 ||
            RtlCompareMemory(table + 10, "BOCHS ", 6) != 6 ||
            RtlCompareMemory(table + 16, "BXPC    ", 8) != 8 ||
            ((const PCC_TABLE *)table)->Flags.AsULong != 1) {
            status = STATUS_NOT_SUPPORTED;
        }
        for (ULONG index = 0; NT_SUCCESS(status) && index < 2; ++index) {
            PCC_EXTENDED_3_4_SUBSPACE subspace;
            const LONGLONG controlAddress = 0x09110000 + index * 0x20;
            const GEN_ADDR absentRegister = { 0 };

            RtlCopyMemory(&subspace,
                table + FIELD_OFFSET(PCC_TABLE, Subspaces) + index * sizeof(subspace),
                sizeof(subspace));
            if (subspace.Header.Type != PCC_SUBSPACE_TYPE_EXTENDED_3 + index ||
                subspace.Header.Length != sizeof(subspace) ||
                subspace.PlatformInterruptGsiv != 43 + index ||
                subspace.PlatformInterruptFlags != 0 ||
                subspace.BaseAddress.QuadPart != 0x090F0000 + index * 0x10000 ||
                subspace.Length != 4096 ||
                !ECTestPccRegisterMatches(&subspace.DoorbellRegister, controlAddress + 4) ||
                subspace.DoorbellPreserve != 0 || subspace.DoorbellWrite != 1 ||
                !ECTestPccRegisterMatches(&subspace.PlatformInterruptAckRegister, controlAddress + 12) ||
                subspace.PlatformInterruptAckPreserve != 0 || subspace.PlatformInterruptAckWrite != 1 ||
                !ECTestPccRegisterMatches(&subspace.CommandCompleteCheckRegister, controlAddress) ||
                subspace.CommandCompleteCheckMask != 1 ||
                !ECTestPccRegisterMatches(&subspace.CommandCompleteUpdateRegister, controlAddress) ||
                subspace.CommandCompleteUpdatePreserve != 0xFFFFFFFE ||
                subspace.CommandCompleteUpdateWrite != index) {
                status = STATUS_NOT_SUPPORTED;
            } else if (index == 0) {
                if (!ECTestPccRegisterMatches(&subspace.ErrorStatusRegister, controlAddress) ||
                    subspace.ErrorStatusMask != 2) {
                    status = STATUS_NOT_SUPPORTED;
                }
            } else if (RtlCompareMemory(&subspace.ErrorStatusRegister, &absentRegister,
                           sizeof(absentRegister)) != sizeof(absentRegister) ||
                       subspace.ErrorStatusMask != 0) {
                status = STATUS_NOT_SUPPORTED;
            }
        }
    } else if (NT_SUCCESS(status)) {
        status = STATUS_INVALID_BUFFER_SIZE;
    }
    ExFreePoolWithTag(table, ECTEST_PCC_POOL_TAG);
    return status;
}

NTSTATUS ECTestPccQuery(
    WDFDEVICE Device,
    PECTEST_PCC_NATIVE_INTERFACE Interface,
    PULONG AcpiTimeStamp,
    PULONG AcpiImageSize
    )
{
    NTSTATUS status;

    PAGED_CODE();
    RtlZeroMemory(Interface, sizeof(*Interface));
    *AcpiTimeStamp = 0;
    *AcpiImageSize = 0;
    status = AuxKlibInitialize();
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ECTestPccCheckImage(AcpiTimeStamp, AcpiImageSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = ECTestPccCheckTable();
    if (!NT_SUCCESS(status)) {
        return status;
    }

    Interface->SubspaceId = 0;
    status = WdfFdoQueryForInterface(Device, &GUID_PCC_INTERFACE_STANDARD,
        (PINTERFACE)Interface, sizeof(*Interface), 1, NULL);
    if (NT_SUCCESS(status) &&
        (Interface->Size != sizeof(*Interface) || Interface->Version != 1 ||
         Interface->SubspaceId != 0 || Interface->SubspaceSize != 4080 ||
         Interface->Handle == NULL || Interface->Subspace == NULL ||
         Interface->AcquireSubspace == NULL || Interface->ExecuteCommand == NULL ||
         Interface->ReleaseSubspace == NULL)) {
        if (Interface->InterfaceDereference != NULL) {
            Interface->InterfaceDereference(Interface->Context);
        }
        RtlZeroMemory(Interface, sizeof(*Interface));
        status = STATUS_REVISION_MISMATCH;
    }
    return status;
}