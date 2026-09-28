# EC Test Bridge

The default driver provides the existing ACPI evaluation and notification
interfaces. The experimental native PCC client is enabled by default for
ARM64 builds and disabled for x64 builds. Set `EnableNativePcc=false` to
disable it in an ARM64 build.

## Experimental Native PCC Client

For a disposable ARM64 test VM, enable the client in a Windows WDK build:

```powershell
msbuild ectest_kmdf.vcxproj /p:Configuration=Release /p:Platform=ARM64
```

Use the normal driver package and test-signing workflow. The ARM64 default adds
`pcc.c` and `Aux_Klib.lib`; it does not replace Windows' PCC transport.

The implementation queries `GUID_PCC_INTERFACE_STANDARD` using
`WdfFdoQueryForInterface`, version 1, for Type 3 subspace 0. The probe returns
metadata only; the execute IOCTL acquires the channel, writes the bounded
payload, calls ExecuteCommand and reads the response. On the inspected build,
execution consumes the acquisition, so the client does not call ReleaseSubspace
afterward. It exposes no kernel pointers or physical-memory interface.

The probe accepts only the inspected ARM64 ACPI image fingerprint (PE
timestamp `0xF61FB868`, image size `0xE1000`, observed file version
`10.0.28000.2605`) and the ODP QEMU platform's 376-byte PCCT containing
Type 3 and Type 4 subspaces, published by QEMU with OEM ID `BOCHS ` and
OEM table ID `BXPC    `. The fingerprint is a compatibility check, not
cryptographic authentication or a promise of compatibility with other builds.

Both subspaces must match the mailbox-only eSPI layout: 4 KiB payloads at
`0x090F0000` and `0x09100000`, and DWORD control blocks at `0x09110000` and
`0x09110020`. The driver validates the doorbell, interrupt acknowledgement,
completion and error GAS descriptors and masks using the WDK PCCT structures.
It rejects the older controller-register layout before querying the interface.
Windows remains responsible for accessing the registers described by PCCT.

### ABI Provenance

The 128-byte native interface declaration in `pcc.h` comes from the matching
Microsoft public symbols for the inbox ARM64 `fxppm.sys` version
`10.0.26100.7920`. Its PDB GUID is
`DFF090AF-D8DD-C1DD-2E45-CA75ECA11A8A`, DBI age 1; TPI record `0x2237`
defines `_PCC_INTERFACE_STANDARD`, with field list `0x2236`.

- Binary SHA-256: `f89401e1b6025dc1e98e1e634065aa989e3f31677bf41cff28b8140952508991`
- PDB SHA-256: `a07fe65dded53b74610d1e7c1f6a804f46d8228e26e4598177c3a9274bbf87b8`
- [Microsoft symbol download](https://msdl.microsoft.com/download/symbols/fxppm.pdb/DFF090AFD8DDC1DD2E45CA75ECA11A8A1/fxppm.pdb)

This is build-specific type evidence, not a published third-party PCC DDI
contract. The provider's interface-reference callbacks do not establish a
verified unregister/requery lifecycle. Use one probe session per fresh VM
boot; device restart, hot removal and suspend/resume are not qualified.

### Probe Protocol

`IOCTL_ECTEST_PCC_PROBE` (`0x0022E000`) uses buffered I/O and requires read
and write access to `GUID_DEVINTERFACE_ECTEST`. Input is two little-endian
32-bit values: protocol version 1 and subspace ID 0. Output is the 48-byte
`ECTEST_PCC_PROBE_RESPONSE` declared in `ectest.h`. Invalid requests fail
the IOCTL; accepted requests return the actual validation/query NTSTATUS in
`QueryStatus`. A successful query is cached until hardware release.

The companion `odp-platform-common` library exposes the decoder and Windows
client behind its `native-pcc` feature. A live query on ARM64 WinVOS
`10.0.28000.2605` returned success, interface version 1, subspace 0, a
4,080-byte payload and flags 1. Success means the native interface was
acquired, not that a command reached the EC.

### Request Length

The inspected Type 3 provider returns the payload pointer at shared-memory
base + 16, excluding the extended PCC header. Its ExecuteCommand argument is
one byte, which the provider writes to the 32-bit command field. No write to
the required header Length field was found in the inspected initialization
and execution paths, and the interface has no length argument.

The platform EC therefore bounds request size using the application's
`data_len` field and ignores the request PCC Length. Responses still contain
a correct PCC Length. The client only accesses the returned payload mapping;
it does not write before that pointer or independently map the mailbox.
This request framing is a platform compatibility convention, not strict
conformance to the extended PCC header Length requirement.
