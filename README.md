# ESP32-S3 BLE USB dongle (Windows)

ESP-IDF v6.1 firmware that turns an **ESP32-S3-DevKitC-1** into a USB Bluetooth HCI radio. Windows binds its inbox `BTHUSB` driver (no driver install) and shows a **Generic Bluetooth Adapter**. All HCI traffic is logged in detail on a separate UART.

## Features

- **USB Bluetooth HCI dongle** on the native USB port (GPIO19/20): TinyUSB Bluetooth class device (E0/01/01), event IN, ACL IN/OUT and 6 ISO alternate settings. VID:PID `303A:4B42`, serial number derived from the BT MAC.
- **BLE controller in VHCI mode**; `hci_usb_bridge.c` shuttles H4 packets between the controller and the USB endpoints with queues, ACL reassembly across USB packets, and flush on USB detach.
- **Windows compatibility layer** (the S3 is LE only, Windows insists on BR/EDR):
  - answers `Read Buffer Size` (0x1005) and classic `Read_*` commands with static values,
  - turns "Unknown HCI Command" for classic OGF 1-3 commands into success,
  - patches `Read Local Supported Features` to advertise BR/EDR + LE.
- **Extensive UART logging** on UART0 (GPIO43 TX / GPIO44 RX, via the FTDI adapter):
  - boot banner (IDF version, chip revision, reset reason, BT MAC, heap),
  - USB mount / unmount / suspend / resume events,
  - every HCI command, event and ACL packet decoded by name (opcode, event, LE subevent, status, handle, flags),
  - raw hex dumps (verbose level),
  - queue, heap and drop statistics every 5 s.
- **Status LED** (onboard WS2812): blue at level 4 while at least one BLE device is connected, off when none are; it blinks briefly on every BLE data (ACL) packet. GPIO (default 48; DevKitC-1 v1.1 boards use 38) and brightness are set in menuconfig. LED updates run in their own task, so they never delay the USB/BLE path.
- **Asynchronous logging** (`alog.c`): log lines and HCI decodes are queued in a 24 KB ring buffer and printed by a low-priority task pinned to core 1, so UART output never blocks the USB/BLE path. If the ring fills, records are dropped and counted (`N log record(s) dropped`).

### Verified

- Enumerates on Windows with status OK.
- LE active scan finds nearby devices.
- LE connect, GATT service discovery, connection parameter updates and sustained ACL traffic work with 0 drops.
- Windows SMP pairing requests reach the remote device (the remote may still refuse them).

### Limitations

- **Classic Bluetooth does not work** (inquiry, BR/EDR pairing, audio). Only LE does; the classic setup commands are faked just to get Windows to accept the radio.
- `Read Local Extended Features` (0x1004) is not shimmed.
- While the dongle runs, the native USB port belongs to it.

## Hardware

| Port | Use |
|------|-----|
| Native USB (GPIO19/20) | Plug into the Windows host: this is the Bluetooth dongle |
| UART0 (GPIO43/44) via FTDI adapter | Logs and flashing |

## Quick flash (prebuilt firmware)

[firmware/esp32s3_ble_usb_dongle_merged.bin](firmware/esp32s3_ble_usb_dongle_merged.bin) contains the bootloader, partition table and app in one image (flash at offset `0x0`). No ESP-IDF install is needed, only `esptool`:

```powershell
pip install esptool
esptool --chip esp32s3 -p COM6 -b 460800 write-flash 0x0 firmware/esp32s3_ble_usb_dongle_merged.bin
```

Replace `COM6` with the UART/FTDI port. If auto-reset does not work, hold **BOOT**, tap **RESET**, release BOOT, then run the command (also works over the native USB port when the dongle firmware is not running). Press RESET afterwards if the board does not restart.

Alternatively use the browser-based Espressif web flasher with the same file at address `0x0`.

The prebuilt image is rebuilt from the current sources; see the commit history for its version. Rebuild it after code changes with:

```powershell
esptool --chip esp32s3 merge-bin -o firmware/esp32s3_ble_usb_dongle_merged.bin --flash-mode dio --flash-freq 80m --flash-size 8MB 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0x10000 build/esp32s3_ble_usb_dongle.bin
```

## Build and flash

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
idf.py build
idf.py -p COMx -b 460800 flash
```

Flash over the **UART/FTDI port**. The flash tool pulses RTS to enter the bootloader, so no BOOT button is needed. Alternatively hold BOOT, reset, and flash over the native port.

If you edit `sdkconfig.defaults`, delete `sdkconfig` so the defaults are re-applied.

## Using it

1. Flash the firmware and connect the native USB port to the Windows PC. Windows adds the Bluetooth adapter.
2. Open the UART port to watch logs:
   - **921600 baud, 8N1, no flow control.**
   - **Keep DTR and RTS low when opening the port, or the board resets.** In pyserial, create `serial.Serial()` with no port, set `dtr=False`, `rts=False`, `rtscts=False`, `dsrdtr=False`, then `open()`. In terminal programs, disable hardware flow control and DTR/RTS toggling.
3. Use Bluetooth LE from Windows as usual (Settings, or any WinRT/bleak LE app).

Example monitor script (pyserial):

```python
import serial
s = serial.Serial()
s.port, s.baudrate, s.timeout = "COM6", 921600, 0.3
s.dtr = s.rts = s.rtscts = s.dsrdtr = s.xonxoff = False
s.open()
while True:
    print(s.read(4096).decode("ascii", "replace"), end="")
```

## Configuration

`idf.py menuconfig` -> "BLE USB Dongle": VID/PID, HCI decode on/off, hex dumps on/off, max hex bytes, statistics period. Log levels default to DEBUG with the `HCI` and `BRIDGE` tags forced to VERBOSE in `main.c`.

## Log tags

| Tag | Content |
|-----|---------|
| `MAIN` | boot banner, init steps |
| `ALOG` | async logger status and drop reports |
| `USB_DESC` | USB descriptors |
| `TinyUSB` | TinyUSB stack |
| `BRIDGE` | queues, USB state, shim actions, stats every 5 s |
| `HCI` | decoded command/event/ACL traffic (`D`), hex dumps (`V`) |

Every stats line carries a running report number (`stats #N`), and every packet hex line carries a submit sequence number plus its type (`C->H EVT #265 [73]: 02 20 ...`; the H4 type byte is not included). Hex dumps show whole packets (up to 272 bytes by default). A gap in the `#` sequence means log records were dropped.

Direction markers: `H->C` host (Windows) to controller, `C->H` controller to host, `SHIM` events generated by the compatibility layer.

## Source layout

| File | Role |
|------|------|
| `main/main.c` | startup: async logger, NVS, BT controller, bridge, USB |
| `main/hci_usb_bridge.c/.h` | HCI <-> USB bridge, Windows shims, stats |
| `main/usb_bt_desc.c/.h` | USB descriptors and TinyUSB install |
| `main/hci_log.c/.h` | HCI decoding and hex dumps |
| `main/alog.c/.h` | asynchronous log ring buffer + core 1 task |
| `main/Kconfig.projbuild` | project options |
