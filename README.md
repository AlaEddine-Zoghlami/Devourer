# devourer

The **capability + JNI layer** of the 3-repo APFPV stack. It takes **[WiFiDriver](https://github.com/AlaEddine-Zoghlami/WiFiDriver)**
— the first userspace _station-mode_ driver for the RTL8812AU — and adds everything needed to turn a
station connection into a usable FPV / IP link, plus the Android JNI.

```
PixelPilot (app)  ──▶  devourer (this repo)  ──▶  WiFiDriver (driver, nested submodule)
```

## What it adds on top of WiFiDriver

- **IP layer** — DHCP client + a **configurable static-IP** option (skip DHCP, bind a fixed IP),
  ARP responder, and a decrypted-IP demux (RTP / DHCP / general TCP+UDP).
- **FPV** — RTP relay to `127.0.0.1:5600` (drop-in for PixelPilot's UDP-5600 player), LQ/RSSI feedback.
- **Session** — `ApfpvStation` orchestration + an auto-reconnect supervisor.
- **WPA2 hardening** — group + PTK rekey, EAPOL + CCMP-PN anti-replay, GTK slots, 802.11w PMF, CSPRNG.
- **JNI** — `jni/apfpv_jni.cpp` (APFPV station) + `jni/WfbngLink.cpp` (the wfb bridge) for the app.
- AP / SoftAP authenticator — **WIP, gated off**.

## Build (host demo)

WiFiDriver is a **nested** submodule, so clone recursively (or init the submodule), then build with
CMake (libusb-1.0, C++20, **CMake ≥ 3.24** for the whole-archive link):

```sh
git submodule update --init --recursive
cmake -B build -S .
cmake --build build --target ApfpvCompliance
```

`ApfpvCompliance` is an end-to-end host test (scan / connect / WPA2 / DHCP / RTP). Example:

```sh
DEVOURER_PID=0x881a APFPV_TESTS=connect APFPV_SSID=<ssid> APFPV_PASS=<pass> ./build/ApfpvCompliance
```

> ⚠️ **Build note:** the two static libs (`libWiFiDriver.a` + `libdevourer.a`) are linked with
> `--whole-archive` (`$<LINK_LIBRARY:WHOLE_ARCHIVE,…>`). A plain link drops static-init `.o` and
> **silently breaks TX**. The Android `.so` (PixelPilot) avoids this by compiling all sources into
> one library — keep one of those two approaches.

## Android

Consumed by **PixelPilot** as a submodule; PixelPilot's `cpp/CMakeLists` compiles the driver, this
layer, the wfb logic, and the JNI directly into one `libWfbngRtl8812.so`.

---

Devourer began as a userspace re-implementation of Realtek's RTL88xxAU Wi-Fi driver (monitor/inject
for wifibroadcast); this fork extends it with the station + IP + FPV + JNI layers above.
