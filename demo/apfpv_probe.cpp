// APFPV station probe — Linux/WSL harness.
// Opens the AU dongle over libusb, runs the FULL ApfpvStation connect chain
// (scan -> arm -> auth -> assoc -> WPA2 -> DHCP) exactly like the Android JNI
// (apfpv_jni.cpp ensureStation + nativeStaConnect), and prints the state funnel.
// Used to capture the STATION-path TX in usbmon and find why the auth gets no reply.
//   Build: cmake + make ApfpvProbe.  Run: DEVOURER_PID=0x881a ./ApfpvProbe
#include "WiFiDriver.h"
#include "RtlJaguarDevice.h"
#include "RtlUsbAdapter.h"
#include "ApfpvStation.h"
#include "logger.h"
#include <libusb.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>

static const char* stateName(int s) {
    static const char* n[] = {"Idle","Scanning","Arming","Authenticating","Associating",
        "Handshaking","Dhcp","Streaming","FailNoAp","FailTx","FailNoAck","FailAuth",
        "FailDhcp","LinkLost","Reconnecting"};
    return (s >= 0 && s < 15) ? n[s] : "?";
}

int main(int argc, char** argv) {
    auto logger = std::make_shared<Logger>();
    libusb_context* ctx = nullptr;
    libusb_init(&ctx);

    uint16_t vid = 0x0bda, pid = 0x881a;
    if (const char* p = std::getenv("DEVOURER_PID")) pid = (uint16_t)strtoul(p, nullptr, 0);
    libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, vid, pid);
    if (!handle) { printf("ERROR: no device %04x:%04x\n", vid, pid); return 1; }
    if (libusb_kernel_driver_active(handle, 0)) libusb_detach_kernel_driver(handle, 0);
    if (!std::getenv("DEVOURER_SKIP_RESET")) libusb_reset_device(handle);
    if (libusb_claim_interface(handle, 0) != 0) { printf("ERROR: claim_interface\n"); return 1; }

    // libusb EVENT LOOP — REQUIRED: RtlUsbAdapter::send_packet uses async
    // libusb_submit_transfer; without a thread pumping libusb_handle_events the
    // TX URB is submitted but never completed -> the auth frame never reaches the
    // wire (0 bulk-OUT). The APFPV path (apfpv_jni) currently lacks this.
    std::atomic<bool> evtRun{true};
    std::thread evtThread([&]() {
        while (evtRun.load()) {
            struct timeval tv { 0, 100000 };
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
    });

    WiFiDriver driver(logger);
    auto rtl = driver.CreateRtlDevice(handle);
    if (!rtl) { printf("ERROR: CreateRtlDevice null\n"); evtRun=false; evtThread.join(); return 1; }

    std::atomic<int> last{-1};
    auto onState = [&last](apfpv::ApfpvStation::State s) {
        int si = (int)s;
        if (si != last.exchange(si)) { printf("STATE -> %d (%s)\n", si, stateName(si)); fflush(stdout); }
    };
    auto onRtp = [](const uint8_t*, size_t) {};

    apfpv::ApfpvStation station(&rtl->adapter(), &rtl->radioManager(), onRtp, onState);
    station.setDevice(rtl.get());

    apfpv::ApfpvStation::Params prm;
    prm.ssid = "OpenIPC";
    prm.passphrase = "12345678";
    prm.scan = true;                 // sweep to find OpenIPC's real channel + BSSID
    prm.channel = 6;                 // hint
    if (const char* c = std::getenv("APFPV_CHANNEL")) prm.channel = atoi(c);
    prm.haveBssid = false;

    printf("=== APFPV station probe: connecting to \"OpenIPC\" ===\n"); fflush(stdout);
    station.connect(prm);

    int secs = 25;
    if (const char* s = std::getenv("APFPV_SECONDS")) secs = atoi(s);
    std::this_thread::sleep_for(std::chrono::seconds(secs));

    station.disconnect();
    evtRun = false;
    if (evtThread.joinable()) evtThread.join();
    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(ctx);
    printf("=== probe done ===\n");
    return 0;
}
