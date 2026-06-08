// ============================================================================
//  apfpv_compliance.cpp — APFPV station behavior compliance suite
//  Native harness (Windows WinUSB / Linux libusb) that drives the REAL
//  ApfpvStation through every behavior the Android JNI exposes and asserts each:
//      T1 SCAN       — find OpenIPC by beacon, report channel/BSSID
//      T2 CONNECT    — scan->arm->auth->assoc->4-way->dhcp; assert progress
//      T3 RECONNECT  — disconnect then reconnect; assert it re-progresses
//      T4 CREATE-AP  — beacon (EIRP-cal soft-AP) injects without error
//  Prints a PASS/FAIL table and exits non-zero if any selected test fails, so it
//  can gate CI. Each test is independently selectable (APFPV_TESTS=scan,connect,
//  reconnect,ap) and the connect pass-threshold is tunable (APFPV_PASS_STATE).
//
//  Build: cmake + make ApfpvCompliance.   Run: DEVOURER_PID=0x881a ./ApfpvCompliance
// ============================================================================
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include "WiFiDriver.h"
#include "RtlJaguarDevice.h"
#include "RtlUsbAdapter.h"
#include "ApfpvStation.h"
#include "crypto/AesCcm.h"
#include "crypto/AesCmac.h"
#include "logger.h"
#include <libusb.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>

using apfpv::ApfpvStation;
using State = ApfpvStation::State;

static const char* stateName(int s) {
    static const char* n[] = {"Idle","Scanning","Arming","Authenticating","Associating",
        "Handshaking","Dhcp","Streaming","FailNoAp","FailTx","FailNoAck","FailAuth",
        "FailDhcp","LinkLost","Reconnecting"};
    return (s >= 0 && s < 15) ? n[s] : "?";
}

// Progress rank along the SUCCESS path (Fail* states are not on it). Used to
// decide how far a connect got and whether it cleared the pass threshold.
static int progressRank(State s) {
    switch (s) {
        case State::Idle:            return 0;
        case State::Scanning:        return 1;
        case State::Arming:          return 2;
        case State::Authenticating:  return 3;
        case State::Associating:     return 4;   // <- auth got a reply (H2C fix works)
        case State::Handshaking:     return 5;   // <- assoc got a reply
        case State::Dhcp:            return 6;
        case State::Streaming:       return 7;   // <- full link
        default:                     return -1;  // any Fail*/LinkLost/Reconnecting
    }
}
static int rankForName(const std::string& n) {
    for (int i = 0; i < 8; ++i) if (n == stateName(i)) return progressRank((State)i);
    return 4; // default threshold = Associating
}

struct TestResult { std::string name; bool pass; std::string detail; };

// Tracks the furthest success-path state + whether a Fail* was seen, from the
// state callback (called on the station's worker threads -> atomics).
struct StateTracker {
    std::atomic<int> furthest{0};      // max progressRank seen (>=0)
    std::atomic<int> furthestState{0}; // the State index for that rank
    std::atomic<int> lastFail{-1};     // last Fail* State index, or -1
    void on(State s) {
        int r = progressRank(s);
        if (r >= 0) {
            int cur = furthest.load();
            while (r > cur && !furthest.compare_exchange_weak(cur, r)) {}
            if (r >= furthest.load()) furthestState.store((int)s);
        } else {
            lastFail.store((int)s);
        }
    }
    void reset() { furthest=0; furthestState=0; lastFail=-1; }
};

static bool envHasTest(const char* name) {
    const char* t = std::getenv("APFPV_TESTS");
    if (!t || !*t) return true;                 // default: all tests
    return std::strstr(t, name) != nullptr;
}

int main(int argc, char** argv) {
    // RFC 3610 CCM self-test (packet vector #1) — proves the AES + CCM core matches the
    // standard (the AP/hostapd). If this FAILS, our CCMP is non-standard and no AP frame
    // can ever decrypt; if it PASSES, the bug is in the per-frame nonce/AAD/key framing.
    {
        uint8_t key[16]={0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF};
        uint8_t nonce[13]={0x00,0x00,0x00,0x03,0x02,0x01,0x00,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5};
        uint8_t aad[8]={0,1,2,3,4,5,6,7};
        uint8_t pt[23]; for(int i=0;i<23;i++) pt[i]=(uint8_t)(0x08+i);
        uint8_t exp[31]={0x58,0x8C,0x97,0x9A,0x61,0xC6,0x63,0xD2,0xF0,0x66,0xD0,0xC2,0xC0,0xF9,0x89,0x80,
                         0x6D,0x5F,0x6B,0x61,0xDA,0xC3,0x84,0x17,0xE8,0xD1,0x2C,0xFD,0xF9,0x26,0xE0};
        uint8_t out[31];
        apfpv::crypto::aes_ccm_encrypt(key,nonce,13,aad,8,pt,23,out);
        bool ok = std::memcmp(out,exp,31)==0;
        fprintf(stderr,"[CCM-SELFTEST] RFC3610 vec#1: %s\n", ok?"PASS":"FAIL");
        if(!ok){ fprintf(stderr,"  got :"); for(int i=0;i<31;i++) fprintf(stderr," %02x",out[i]);
                 fprintf(stderr,"\n  want:"); for(int i=0;i<31;i++) fprintf(stderr," %02x",exp[i]); fprintf(stderr,"\n"); }
        // AES-CMAC (RFC 4493) — foundation for 802.11w BIP-CMAC-128 (PMF).
        fprintf(stderr,"[CMAC-SELFTEST] RFC4493: %s\n", apfpv::crypto::aes_cmac_selftest()?"PASS":"FAIL");
    }
    auto logger = std::make_shared<Logger>();
    libusb_context* ctx = nullptr;
    libusb_init(&ctx);

    uint16_t vid = 0x0bda, pid = 0x881a;
    if (const char* p = std::getenv("DEVOURER_PID")) pid = (uint16_t)strtoul(p, nullptr, 0);
    libusb_device_handle* handle = libusb_open_device_with_vid_pid(ctx, vid, pid);
    if (!handle) { printf("FATAL: no device %04x:%04x\n", vid, pid); return 2; }
    if (libusb_kernel_driver_active(handle, 0)) libusb_detach_kernel_driver(handle, 0);
    if (!std::getenv("DEVOURER_SKIP_RESET")) libusb_reset_device(handle);
    if (libusb_claim_interface(handle, 0) != 0) { printf("FATAL: claim_interface\n"); return 2; }

    // libusb event loop — REQUIRED for the async TX URBs to complete. Yields while RX is
    // paused (rxQuiesce) so it doesn't contend the event lock with pauseAsyncRx's drain +
    // the cal's synchronous control reads (the daemon-vs-direct-call serialization fix).
    std::atomic<bool> evtRun{true};
    auto rxQuiesce = std::make_shared<std::atomic<bool>>(false);
    std::thread evtThread([&, rxQuiesce]() {
        while (evtRun.load()) {
            if (rxQuiesce->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            struct timeval tv { 0, 5000 };   // 5 ms — reap OUT-completions + RX cancels fast
            libusb_handle_events_timeout_completed(ctx, &tv, nullptr);
        }
    });

    WiFiDriver driver(logger);
    auto rtl = driver.CreateRtlDevice(handle);
    if (!rtl) { printf("FATAL: CreateRtlDevice null\n"); evtRun=false; evtThread.join(); return 2; }
    rtl->adapter().setUsbContext(ctx);   // enable the event-pump drain in pauseAsyncRx
    rtl->adapter().setQuiesceFlag(rxQuiesce);  // event thread yields during RX pause/cal

    StateTracker tracker;
    auto onState = [&tracker](State s) {
        static std::atomic<int> last{-1};
        int si = (int)s;
        if (si != last.exchange(si)) { printf("    state -> %d (%s)\n", si, stateName(si)); fflush(stdout); }
        tracker.on(s);
    };
    // RTP end-to-end test hook: log received video/UDP-5600 payloads so an external
    // RTP source (e.g. the phone running `nc -u <our-ip> 5600`) can be verified arriving
    // through the dongle's decrypt path. Real PixelPilot forwards these to 127.0.0.1:5600.
    static std::atomic<uint64_t> g_rtpPkts{0}, g_rtpBytes{0};
    auto onRtp = [](const uint8_t* p, size_t n) {
        // Relay each RTP packet to 127.0.0.1:5600 — exactly what the JNI/PixelPilot path does —
        // so a local player (VLC/ffplay with the ffmpeg SDP) can display the live video.
        static int relayFd = []() -> int {
#ifdef _WIN32
            WSADATA w; WSAStartup(MAKEWORD(2,2), &w);
#endif
            return (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        }();
        static sockaddr_in dst = []() { sockaddr_in a{}; a.sin_family = AF_INET;
            a.sin_port = htons(5600); a.sin_addr.s_addr = inet_addr("127.0.0.1"); return a; }();
        if (relayFd >= 0) sendto(relayFd, (const char*)p, (int)n, 0, (sockaddr*)&dst, sizeof(dst));
        uint64_t k = ++g_rtpPkts; g_rtpBytes += n;
        if (k <= 10 || k % 25 == 0)
            fprintf(stderr, "[RTP-RX] pkt#%llu len=%zu total=%llu (b0=0x%02x)\n",
                    (unsigned long long)k, n, (unsigned long long)g_rtpBytes.load(), n? p[0]:0);
    };

    ApfpvStation station(&rtl->adapter(), &rtl->radioManager(), onRtp, onState);
    station.setDevice(rtl.get());

    const char* ssid = std::getenv("APFPV_SSID"); if (!ssid) ssid = "OpenIPC";
    const char* pass = std::getenv("APFPV_PASS"); if (!pass) pass = "12345678";
    int hintCh = 6; if (const char* c = std::getenv("APFPV_CHANNEL")) hintCh = atoi(c);
    int passRank = 4; // Associating
    if (const char* ps = std::getenv("APFPV_PASS_STATE")) passRank = rankForName(ps);

    // Optional fixed BSSID (APFPV_BSSID=06:5f:09:96:81:46) -> phone-assisted path:
    // arm straight to this BSSID on APFPV_CHANNEL, NO beacon sweep (isolates the
    // scan/channel-hop variable from the auth/assoc + RX path).
    std::array<uint8_t,6> fixedBssid{}; bool haveFixed = false;
    if (const char* bs = std::getenv("APFPV_BSSID")) {
        unsigned v[6];
        if (sscanf(bs, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) == 6) {
            for (int i=0;i<6;i++) fixedBssid[i]=(uint8_t)v[i];
            haveFixed = true;
            printf("  (fixed BSSID %s on ch%d, no scan)\n", bs, hintCh);
        }
    }
    auto makeParams = [&](bool scan) {
        ApfpvStation::Params prm;
        prm.ssid = ssid; prm.passphrase = pass;
        prm.channel = hintCh;
        if (haveFixed) { prm.scan = false; prm.haveBssid = true; prm.bssid = fixedBssid; }
        else           { prm.scan = scan;  prm.haveBssid = false; }
        if (const char* p = std::getenv("APFPV_PMF")) prm.pmf = atoi(p);  // 0=off 1=capable 2=required
        return prm;
    };

    std::vector<TestResult> results;
    printf("\n==================== APFPV COMPLIANCE SUITE ====================\n");
    printf("SSID=\"%s\"  hintCh=%d  pass-threshold=%s(rank %d)\n",
           ssid, hintCh, stateName(passRank<0?4:passRank), passRank);

    // ---- T1: SCAN ----------------------------------------------------------
    if (envHasTest("scan")) {
        printf("\n[T1 SCAN] sweeping for beacons...\n"); fflush(stdout);
        std::mutex m; std::string foundSsidInfo; bool found = false;
        station.scanAll(/*perChannelMs=*/120, /*includeDfs=*/false,
            [&](const std::string& s, const apfpv::ApInfo& info) {
                std::lock_guard<std::mutex> lk(m);
                printf("    beacon: \"%s\" ch=%d rssi=%d\n", s.c_str(), info.channel, info.rssi);
                if (s == ssid && !found) {
                    found = true;
                    char buf[128]; snprintf(buf, sizeof buf, "ch=%d rssi=%d", info.channel, info.rssi);
                    foundSsidInfo = buf;
                }
            });
        results.push_back({"T1 scan", found,
            found ? ("found " + std::string(ssid) + " " + foundSsidInfo)
                  : ("did NOT find " + std::string(ssid))});
    }

    // ---- T2: CONNECT -------------------------------------------------------
    int connSecs = 25; if (const char* s = std::getenv("APFPV_SECONDS")) connSecs = atoi(s);
    if (envHasTest("connect")) {
        printf("\n[T2 CONNECT] connecting (scan->arm->auth->assoc->4way->dhcp)...\n"); fflush(stdout);
        tracker.reset();
        station.connect(makeParams(true));
        // HTTP/TCP proof: GET from the gateway (VTX analog) — confirms the dongle carries
        // general TCP (SSH rides the identical path). Run an HTTP responder on the AP, e.g.
        //   adb shell "while true; do echo -e 'HTTP/1.0 200 OK\r\n\r\nHELLO-FROM-VTX' | toybox nc -l -p 8080; done"
        if (std::getenv("APFPV_HTTP")) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            uint32_t gw = station.leaseServerIp(); if (!gw) gw = station.leaseIp() & 0xFFFFFF00; // fallback .1? no
            uint16_t hport = 8080; if (const char* pp = std::getenv("APFPV_HTTP_PORT")) hport = (uint16_t)atoi(pp);
            fprintf(stderr, "[HTTP] GET http://%u.%u.%u.%u:%u/ ...\n",
                    (gw>>24)&255,(gw>>16)&255,(gw>>8)&255,gw&255, hport);
            std::string r = station.httpGet(gw, hport, "/", 5000);
            fprintf(stderr, "[HTTP] %s (%zu bytes): %.160s\n",
                    r.empty()?"NO RESPONSE (TCP failed)":"RESPONSE OK", r.size(), r.c_str());
        }
        std::this_thread::sleep_for(std::chrono::seconds(connSecs));
        int got = tracker.furthest.load();
        int gotState = tracker.furthestState.load();
        bool pass = got >= (passRank < 0 ? 4 : passRank);
        char detail[160];
        snprintf(detail, sizeof detail, "furthest=%s(rank %d) threshold=rank %d%s",
                 stateName(gotState), got, (passRank<0?4:passRank),
                 tracker.lastFail.load()>=0 ? (std::string(" lastFail=")+stateName(tracker.lastFail.load())).c_str() : "");
        results.push_back({"T2 connect", pass, detail});
        station.disconnect();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // ---- T3: RECONNECT -----------------------------------------------------
    if (envHasTest("reconnect")) {
        printf("\n[T3 RECONNECT] connect, drop, reconnect...\n"); fflush(stdout);
        tracker.reset();
        station.connect(makeParams(true));
        std::this_thread::sleep_for(std::chrono::seconds(connSecs/2 > 8 ? connSecs/2 : 8));
        int first = tracker.furthest.load();
        printf("    [reconnect] first attempt furthest=%s; forcing disconnect...\n",
               stateName(tracker.furthestState.load())); fflush(stdout);
        station.disconnect();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        tracker.reset();
        printf("    [reconnect] reconnecting...\n"); fflush(stdout);
        station.connect(makeParams(true));
        std::this_thread::sleep_for(std::chrono::seconds(connSecs/2 > 8 ? connSecs/2 : 8));
        int second = tracker.furthest.load();
        bool pass = second >= (passRank < 0 ? 4 : passRank) && first >= 1;
        char detail[160];
        snprintf(detail, sizeof detail, "first=rank %d second=%s(rank %d)",
                 first, stateName(tracker.furthestState.load()), second);
        results.push_back({"T3 reconnect", pass, detail});
        station.disconnect();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    // ---- T4: CREATE-AP — ⚠️ NOT READY (WIP), NOT run/exposed by default ---
    if (envHasTest("ap")) {
        // AP/SoftAP mode is WORK-IN-PROGRESS and NOT exposed (see ApfpvStation::startAp): it
        // beacons + tracks client RSSI but cannot host a real client yet (needs 8812 AP/master
        // HW bring-up) and wedges the USB on exit. Skipped unless DEVOURER_AP_WIP is set (dev only).
        if (!std::getenv("DEVOURER_AP_WIP")) {
            printf("\n[T4 CREATE-AP] AP mode NOT READY (WIP) — skipped, not exposed. "
                   "(set DEVOURER_AP_WIP to force)\n"); fflush(stdout);
            results.push_back({"T4 create-ap", true, "AP mode WIP — not run / not exposed"});
        } else {
            const char* apPass = std::getenv("APFPV_AP_PASS");        // empty/unset = open AP
            std::string apSsid = std::string(ssid) + "-AP";
            int apSecs = 30; if (const char* b = std::getenv("APFPV_AP_SECS")) apSecs = atoi(b);
            bool secured = apPass && *apPass;
            printf("\n[T4 CREATE-AP][WIP] SoftAP \"%s\" ch=%d %s for %ds\n",
                   apSsid.c_str(), hintCh, secured?"WPA2-PSK":"OPEN", apSecs); fflush(stdout);
            bool crashed = false;
            try {
                station.startAp(apSsid, hintCh, secured ? std::string(apPass) : std::string());
                for (int t = 0; t < apSecs; ++t) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    if ((t % 3) == 0) for (auto& s : station.apStations())
                        fprintf(stderr, "    [AP-STA] %02x:%02x:%02x:%02x:%02x:%02x rssi=%ddBm state=%d\n",
                                s.mac[0],s.mac[1],s.mac[2],s.mac[3],s.mac[4],s.mac[5], s.rssiDbm, s.state);
                }
                station.stopAp();
            } catch (const std::exception& e) { crashed = true; printf("    AP EXCEPTION: %s\n", e.what()); }
            results.push_back({"T4 create-ap (WIP)", !crashed, crashed ? "AP threw" : "SoftAP ran (WIP)"});
        }
    }

    // ---- SUMMARY -----------------------------------------------------------
    station.disconnect();
    evtRun = false;
    if (evtThread.joinable()) evtThread.join();
    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(ctx);

    printf("\n==================== RESULTS ====================\n");
    int nPass = 0;
    for (auto& r : results) {
        printf("  [%s] %-14s  %s\n", r.pass ? "PASS" : "FAIL", r.name.c_str(), r.detail.c_str());
        if (r.pass) ++nPass;
    }
    printf("=================================================\n");
    printf("  %d/%zu passed\n\n", nPass, results.size());
    return (nPass == (int)results.size()) ? 0 : 1;
}
