# BUILD & TEST runbook — devourer-apfpv fork

Everything in this fork is built and (offline) verified. The remaining steps
need YOUR hardware / toolchain. This is the exact order.

## 0. Get the code onto GitHub (one-time)
    tar -xzf devourer-apfpv-fork.tar.gz -C Devourer && cd Devourer
    git remote add origin https://github.com/AlaEddine-Zoghlami/Devourer.git
    git push -u origin main

## 1. Verify crypto offline (no hardware) — already passing
    g++ -std=c++17 -I. tests/crypto_test.cpp src/crypto/Sha1Hmac.cpp src/crypto/AesCcm.cpp -o ctest && ./ctest
Expect: SHA1/HMAC/PBKDF2-WPA2/AES-CCM/AES/RFC3394 all OK.

## 2. Build the native driver on Linux (laptop)
    sudo apt install cmake libusb-1.0-0-dev g++
    cmake -B build -S . && cmake --build build --target WiFiDriver -j4
    cmake --build build --target ApfpvProbe -j4

## 3. THE GATE — ACK-survival probe (Linux laptop + AU dongle + greg10.2 VTX)
The one empirical unknown: does the RTL8812AU hardware auto-ACK in station mode
when armed purely via registers (no kernel MLME)?
    sudo ./build/ApfpvProbe     # finish demo/apfpv_probe.cpp adapter bringup first
Verdict:
- GO_LinkHeld       -> hardware auto-ACKs. Viable. Proceed to Android.
- NOGO_Deauthed     -> unACKed; station mode NOT possible unrooted. Pivot to
                       phone-associated + dongle-sniff, or WFB-ng.
- TXFAIL_NoAuthResp -> TX descriptor/channel issue (fixable; not the ACK question).
Do this BEFORE any Android work — Linux isolates the driver from JNI/USB-fd.

## 4. Verify RSSI units (one line, laptop)
In RxDeframe::toDbm log raw pkt.RxAtrib.rssi[0] vs a known signal; adjust map.

## 5. Confirm the VTX REST API (browser, while associated)
AirWebUiClient assumes majestic REST at http://192.168.0.1/api/v1/set?<k>=<v>.
If your VTX uses cli-over-SSH (as gsmenu does), adjust AirWebUiClient.

## 6. Android build (only after step 3 = GO)
- Copy android/ApfpvStaLink.java -> app/wfbngrtl8812/.../wfbngrtl8812/
- Copy android/jni/apfpv_jni.cpp -> app/wfbngrtl8812/src/main/cpp/
- Copy android/pixelpilot/*.java -> app/src/main/java/com/openipc/pixelpilot/apfpv/
- Add fork src/*.cpp + src/crypto/*.cpp to wfbngrtl8812 CMakeLists
- Apply docs/PIXELPILOT_WIRING.md edits to VideoActivity
- Build in Android Studio; reconcile signature mismatches.

## Status
- [x] native driver builds (WiFiDriver + ApfpvProbe)
- [x] WPA2 crypto verified vs canonical vectors
- [x] full gsmenu feature parity (APFPV scope) as source
- [ ] step 3 ACK probe on hardware  <-- THE gate
- [ ] RSSI unit check / VTX REST confirm / Android Studio build
