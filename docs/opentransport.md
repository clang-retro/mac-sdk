# Open Transport and MacTCP

The SDK has the Open Transport client interface and the MacTCP
interface for 68k and PowerPC. A program can use either interface, or
the BSD sockets of the C library (see [Networking and pipes](network.md)).
This page tells what each interface does, which systems it runs on and
how to test it.

## Summary

| Interface | Headers | 68k | PowerPC |
| --- | --- | --- | --- |
| Open Transport | `OpenTransport.h`, `OpenTransportProviders.h` (also `OpenTptInternet.h`) | The C library, over MacTCP | Mac OS's Open Transport libraries |
| MacTCP | `MacTCP.h` (TCP and UDP parameter blocks) | The `.IPP` driver | The `.IPP` driver |
| Name resolver | `AddressXlation.h` | The C library, over the `dnrp` resource | The C library, over the `dnrp` resource |
| BSD sockets | `sys/socket.h`, `netdb.h` | The C library, over MacTCP | The C library, over MacTCP |

## Requirements

- Open Transport on PowerPC needs Open Transport 1.1 or later (Mac OS
  7.5.2 to 9.2.2). The SDK imports `OTClientLib`, `OTClientUtilLib`,
  `OTUtilityLib` and `OTInetClientLib` weakly. Without Open Transport,
  the program starts, and `InitOpenTransport` gives `kOTNotFoundErr`.
- Open Transport on 68k needs the `.IPP` driver: MacTCP (System 7), or
  Open Transport, which also has this driver. Without the driver,
  `InitOpenTransport` gives `kOTNotFoundErr`.
- The MacTCP calls need the `.IPP` driver on both processors.
- Names need the resolver: the `dnrp` resource of MacTCP or of the
  TCP/IP control panel. Without the resolver, only dotted addresses
  resolve.

## Open Transport on PowerPC

The calls go to Mac OS's Open Transport. `runtime/libc/ot.c` gives the
calls that Apple's `OpenTransportAppPPC.o` and `OpenTptInetPPC.o`
supplied:

- `InitOpenTransport`, `InitOpenTransportUtilities` and
  `CloseOpenTransport`.
- `OTOpenEndpoint`, `OTAsyncOpenEndpoint`, `OTOpenInternetServices`,
  `OTAsyncOpenInternetServices` and `OTCloseProvider`.
- `OTAllocMem`, `OTAlloc`, `OTRegisterAsClient`,
  `OTUnregisterAsClient`, `OTCreateDeferredTask` and
  `OTCreateSystemTask`.

Open Transport exports these calls only in a private form, with one
more argument: the address of the program's client record. `ot.c` keeps
this record. Apple's `ExitToShell` patch of Open Transport closes the
program's providers when the program quits.

All other calls (for example `OTBind`, `OTSnd`, `OTRcv`, `OTLook`,
`OTInetStringToAddress`) go directly to the Open Transport libraries.

## Open Transport on 68k

Apple's 68k Open Transport libraries use the Apple Shared Library
Manager. The SDK does not have this manager. Thus on 68k the C library
gives the Open Transport calls itself, in `runtime/libc/ot68k.c`. The
calls use the BSD sockets of the C library (MacTCP's `.IPP` driver) and
the name resolver.

These parts work:

- Endpoints of the `tcp` and `udp` configurations: `OTBind` (with
  `qlen` for a listener), `OTConnect`, `OTRcvConnect`, `OTListen`,
  `OTAccept`, `OTSnd`, `OTRcv`, `OTSndUData`, `OTRcvUData`,
  `OTSndOrderlyDisconnect`, `OTRcvOrderlyDisconnect`,
  `OTSndDisconnect`, `OTRcvDisconnect`, `OTUnbind`, `OTLook`,
  `OTCountDataBytes`, `OTGetEndpointState`, `OTGetEndpointInfo` and
  `OTGetProtAddress`.
- Synchronous and asynchronous mode, blocking and nonblocking mode,
  notifiers and completion events (`T_OPENCOMPLETE`, `T_BINDCOMPLETE`,
  `T_ACCEPTCOMPLETE`, ...), and the events of `OTLook` (`T_LISTEN`,
  `T_CONNECT`, `T_DATA`, `T_ORDREL`, `T_DISCONNECT`).
- The Internet services: `OTInetStringToHost`, `OTInetHostToString`,
  `OTInetGetInterfaceInfo`, `OTInetStringToAddress`,
  `OTInetAddressToName`, `OTInetSysInfo` and `OTInetMailExchange`.
- The utilities: memory, strings, lists, time stamps, atomic
  operations, and deferred and system tasks.

Differences from Open Transport:

- A notifier runs when the program calls `OTIdle`, and while a
  synchronous blocking call waits. A notifier never runs at interrupt
  time. A program in asynchronous mode must call `OTIdle` in its event
  loop.
- A deferred task and a system task run at the next `OTIdle`.
- Only `tcp`, `udp` and `dnr` configurations open. Other names (for
  example `ddp` or `rawip`) give `kOTBadConfigurationErr`.
- `OTOptionManagement` accepts the options and gives them back without
  a change. `OTIoctl` and `OTInetQuery` give `kOTNotSupportedErr`.
- `OTInetGetInterfaceInfo` gives the address, the net mask of the
  address class and the broadcast address. It does not give the
  gateway or the name server.

## Starting state of an endpoint

An endpoint starts in synchronous, nonblocking mode, as in Open
Transport. Call `OTSetBlocking` to make the calls wait.

On Mac OS 9.2.1, a blocking `OTRcv` in a program that does not let
other programs run can wait for ever, also when data is there. Use
nonblocking mode and a loop with `OTIdle` and `EventAvail` or
`WaitNextEvent`. `examples/net/fetch.c` shows this loop.

## MacTCP

`MacTCP.h` has the TCP and UDP parameter blocks (`TCPiopb`, `UDPiopb`)
and their control codes. Send them to the `.IPP` driver with
`PBControl`. The C library's sockets use these calls.

`AddressXlation.h` has the name resolver: `OpenResolver`,
`CloseResolver`, `StrToAddr`, `AddrToStr`, `AddrToName`, `EnumCache`,
`HInfo` and `MXInfo`. The C library (`runtime/libc/dnr.c`) loads the
`dnrp` resource from the MacTCP or TCP/IP control panel, or from the
System file, and calls it. On PowerPC it calls the resource through the
Mixed Mode Manager. `OpenResolver` makes `CloseResolver` run when the
program ends.

The resolver gives its result in the low word of `D0`. Open Transport's
resolver puts other bits in the high word. The library uses only the
low word.

## Sample

`examples/net/fetch.c` gets a web page twice: once with Open Transport
and once with BSD sockets. It shows the first line of each reply in a
console window. The same source builds for 68k and PowerPC:

```sh
python3 tools/macapp.py --console window -o Fetch.bin -- examples/net/fetch.c
```

## Tests

- `tests/ot.c` checks the utilities, the Internet services, a TCP
  connection between three endpoints of one program (listener, client
  and worker), a UDP datagram and an asynchronous endpoint with a
  notifier. It prints one `ok` line for each check, then `all ok`.
- `tests/run.sh` runs `tests/ot.c` for 68k in `tools/sim68k.py --os`.
  The fake MacTCP of the simulator uses the loopback network of the
  host.
- `tools/qemu/nettest.py` runs a program on Mac OS in QEMU. It puts the
  program in the Startup Items folder of a copy of the disk, starts a
  report server (port 7009) and a web server (port 8080) on the host,
  boots QEMU and collects the report. The guest finds the host at
  10.0.2.2.

These runs passed:

| Program | System | Machine | Result |
| --- | --- | --- | --- |
| `tests/ot.c` (68k) | Mac OS 8.1, Open Transport 1.3 | QEMU `q800` | all ok |
| `tests/ot.c` (PowerPC) | Mac OS 9.2.1 install CD | QEMU `mac99` | all ok |
| `examples/net/fetch.c` (68k) | Mac OS 8.1 | QEMU `q800` | Both requests got `200 OK` |
| `examples/net/fetch.c` (PowerPC) | Mac OS 9.2.1 | QEMU `mac99` | Both requests got `200 OK` |

Example commands:

```sh
python3 tools/macapp.py --cpu ppc -o ot-ppc.bin -- tests/ot.c
python3 tools/qemu/nettest.py --machine mac99 --image "Mac OS 9.2.1.iso" ot-ppc.bin

python3 tools/macapp.py --cpu 68k -o ot-68k.bin -- tests/ot.c
python3 tools/qemu/nettest.py --machine q800 --rom q800.rom --image hd81.qcow2 ot-68k.bin
```

The `q800` machine needs a Quadra 800 ROM image and a disk with Mac OS
installed. The SDK cannot supply them.

## Not done yet

- Native Open Transport on 68k (through the Apple Shared Library
  Manager).
- AppleTalk, serial and raw IP endpoints.
- An Open Transport path for the BSD sockets on PowerPC. The sockets use
  MacTCP's `.IPP` driver, which Open Transport supplies.
