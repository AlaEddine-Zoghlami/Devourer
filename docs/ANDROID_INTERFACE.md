# Complete Android-app <-> WiFi-adapter interface — gap map

## How it connects (the unrooted USB-host path, PROVEN by monitor mode)
1. Kotlin: UsbManager.requestPermission -> openDevice() -> getFileDescriptor()
2. JNI: pass fd to native -> libusb_set_option(LIBUSB_OPTION_NO_DEVICE_DISCOVERY)
   -> libusb_wrap_sys_device(fd) -> libusb_device_handle
3. C++: new RtlUsbAdapter(handle) -> devourer drives the chip.
devourer itself is headless libusb (constructor takes libusb_device_handle);
ALL Android glue lives in PixelPilot's wfbngrtl8812 module, not in devourer.

## Layer status for a COMPLETE station interface
| Layer | Exists? | Where | Notes |
|-------|---------|-------|-------|
| libusb chip driver (8812AU) | YES | devourer | reference part, all bands |
| fd-bridge + JNI (monitor) | YES | PixelPilot/wfbngrtl8812 | reuse pattern |
| JNI for STATION fns | NO | (new) | expose associate/status/LQ to Kotlin |
| libusb .so per ABI (arm64) | YES | PixelPilot | ships already |
| Station state machine | PARTIAL | our StationMode | arming+probe done |
| Station TX descriptor (BMC=0) | DONE | our StationTxDesc | audit fix |
| Mgmt/EAPOL frame builders | DONE | our Dot11Frames | |
| WPA2 4-way handshake | PARTIAL | our Wpa2Supplicant | bodies elided |
| WPA2 crypto (SHA1/PBKDF2/CCM) | WIRED | our Wpa2Crypto | needs backend vendored |
| IP / DHCP / ARP | NO | (new) | userspace; no OS netstack unrooted |
| 802.11->UDP->RTP de-encap | NO | (new) | feeds existing onNewRTPData |
| LQ feedback sender (UDP 12345) | NO | (new) | dongle radiotap RSSI -> VTX |
| Connect/disconnect/status UI | NO | (new, PixelPilot) | mode toggle + OSD |

## Critical de-risking note
The USB-on-Android question is ALREADY ANSWERED: monitor mode works unrooted
today via the fd-bridge above. Station mode reuses the exact same transport;
only higher layers (state/crypto/IP) are new. The transport is not a risk.
