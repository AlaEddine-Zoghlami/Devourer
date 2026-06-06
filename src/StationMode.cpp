// ============================================================================
//  StationMode.cpp  —  station-mode arming + ACK-survival probe (impl)
//  Declarations in StationMode.h. The auto-ACK arming sequence is ported from
//  the Linux driver hw_var_set_opmode STATION path (see docs/ARMING_SEQUENCE.md):
//      MACID -> MSR=STATION(0x02) -> BSSID -> RCR|=RCR_CBSSID_DATA(BIT6)
//  All register symbols confirmed in devourer hal/hal_com_reg.h.
// ============================================================================
#include "StationMode.h"
#include "Dot11Frames.h"
#include "StationTxDesc.h"
#include "hal_com_reg.h"

#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include "RadioManagementModule.h"
#ifdef __ANDROID__
#include <android/log.h>
#define SMLOG(...) __android_log_print(ANDROID_LOG_INFO, "apfpv-arm", __VA_ARGS__)
#else
#define SMLOG(...) ((void)0)
#endif

namespace apfpv {

static constexpr uint8_t HW_STATE_STATION = 0x02;   // MSR_INFRA
static constexpr int     TXDESC_8812      = 40;

StationMode::StationMode(RtlUsbAdapter& dev, RadioManagementModule& rm, SendFrameFn send)
    : _dev(dev), _rm(rm), _send(std::move(send)) {}

void StationMode::arm(const MacAddr& self, const MacAddr& bssid) {
    // (1) own MAC -> REG_MACID
    for (int i = 0; i < 4; ++i) _dev.rtw_write8(REG_MACID + i, self.b[i]);
    _dev.rtw_write16(REG_MACID + 4, (uint16_t)(self.b[4] | (self.b[5] << 8)));
    // (2) MSR -> STATION (preserve port1 high bits)
    if (!std::getenv("DEVOURER_SKIP_MSR")) {
        uint8_t msr = (uint8_t)((_dev.rtw_read8(MSR) & 0x0C) | HW_STATE_STATION);
        _dev.rtw_write8(MSR, msr);
    }
    // (3) AP BSSID -> REG_BSSID
    for (int i = 0; i < 4; ++i) _dev.rtw_write8(REG_BSSID + i, bssid.b[i]);
    _dev.rtw_write16(REG_BSSID + 4, (uint16_t)(bssid.b[4] | (bssid.b[5] << 8)));
    // (4) RCR: station address-matched + accept/ACK BSSID-matched data
    if (!std::getenv("DEVOURER_SKIP_RCR")) {
        uint32_t rcr = RCR_APM | RCR_AM | RCR_AB | RCR_CBSSID_DATA | RCR_CBSSID_BCN;
        _dev.rtw_write32(REG_RCR, rcr);
    }
    if (std::getenv("DEVOURER_SKIP_H2C")) { _armed = true; return; }
    // (5) Tell the FIRMWARE: MACID 0 is a CONNECTED STATION (H2C
    // MEDIA_STATUS_RPT = 0x01). The register arming above is NOT enough to make
    // the HW MAC auto-ACK the AP's auth/assoc replies (ARMING_SEQUENCE.md open
    // question, confirmed by hardware test). Port of the Linux driver's
    // rtw_hal_set_FwMediaStatusRpt_cmd(RT_MEDIA_CONNECT, macid=0):
    //   parm[0]: bit0 opmode=1 (connect), bit1 macid_ind=0 ; parm[1]=macid.
    uint8_t msrParm[3] = { 0x01, 0x00, 0x00 };
    bool h2c = _dev.fillH2CCmd(0x01 /*H2C_MEDIA_STATUS_RPT*/, 3, msrParm);
    SMLOG("media-status-rpt H2C (connect macid0) sent=%d", h2c ? 1 : 0);
    _armed = true;
}

bool StationMode::sendAuthOpenSeq1(const MacAddr& self, const MacAddr& bssid) {
    Mac s, b; std::copy(self.b.begin(), self.b.end(), s.begin());
    std::copy(bssid.b.begin(), bssid.b.end(), b.begin());
    auto mpdu = BuildAuthOpenSeq1(s, b);
    std::vector<uint8_t> frame(TXDESC_8812 + mpdu.size(), 0);
    std::memcpy(frame.data() + TXDESC_8812, mpdu.data(), mpdu.size());
    FillStationTxDesc(frame.data(), (uint16_t)mpdu.size(), TXDESC_8812,
                      0, StationFrameKind::Mgmt, 0, 0x04);
    return _send(frame);
}

bool StationMode::sendAssocRequest(const MacAddr& self, const MacAddr& bssid, const char* ssid) {
    Mac s, b; std::copy(self.b.begin(), self.b.end(), s.begin());
    std::copy(bssid.b.begin(), bssid.b.end(), b.begin());
    auto mpdu = BuildAssocRequest(s, b, std::string(ssid), _pairwise, _group);
    std::vector<uint8_t> frame(TXDESC_8812 + mpdu.size(), 0);
    std::memcpy(frame.data() + TXDESC_8812, mpdu.data(), mpdu.size());
    FillStationTxDesc(frame.data(), (uint16_t)mpdu.size(), TXDESC_8812,
                      0, StationFrameKind::Mgmt, 0, 0x04);
    return _send(frame);
}

static inline uint8_t fc_type(uint16_t fc)    { return (fc >> 2) & 0x3; }
static inline uint8_t fc_subtype(uint16_t fc) { return (fc >> 4) & 0xF; }

void StationMode::onMgmtFrame(uint16_t fc, const MacAddr&, const MacAddr&) {
    if (fc_type(fc) != 0x0) return;          // mgmt only
    uint8_t sub = fc_subtype(fc);
    if (sub != 0x8) SMLOG("rx mgmt subtype 0x%X", sub);   // log non-beacon mgmt (auth/assoc/deauth)
    switch (sub) {
        case 0xB: _gotAuthResp = true; break;  // Auth
        case 0x1: _gotAssocOk  = true; break;  // Assoc-Resp
        case 0xC: case 0xA: _gotDeauth = true; break;  // Deauth/Disassoc
        default: break;
    }
}

StationMode::Result
StationMode::runProbe(const MacAddr& self, const MacAddr& bssid,
                      const char* ssid, int holdSeconds) {
    using namespace std::chrono;
    arm(self, bssid);
    SMLOG("arm: self %02x:%02x:%02x:%02x:%02x:%02x -> bssid %02x:%02x:%02x:%02x:%02x:%02x",
          self.b[0],self.b[1],self.b[2],self.b[3],self.b[4],self.b[5],
          bssid.b[0],bssid.b[1],bssid.b[2],bssid.b[3],bssid.b[4],bssid.b[5]);
    if (!_armed) return Result::Error;
    if (_onPhase) _onPhase(1);                         // -> Authenticating
    bool sent = sendAuthOpenSeq1(self, bssid);
    SMLOG("auth-req tx sent=%d, waiting 3s for auth-resp...", sent ? 1 : 0);
    if (!sent) return Result::Error;

    auto t0 = steady_clock::now();
    while (!_gotAuthResp && !_gotDeauth && steady_clock::now() - t0 < seconds(3))
        std::this_thread::sleep_for(milliseconds(50));
    if (!_gotAuthResp) { SMLOG("NO auth-resp (deauth=%d)", _gotDeauth.load()); return Result::TXFAIL_NoAuthResp; }
    SMLOG("got auth-resp! sending assoc-req...");

    if (_onPhase) _onPhase(2);                          // -> Associating
    std::this_thread::sleep_for(milliseconds(100));
    if (!sendAssocRequest(self, bssid, ssid)) return Result::Error;

    auto t1 = steady_clock::now();
    while (!_gotAssocOk && !_gotDeauth && steady_clock::now() - t1 < seconds(3))
        std::this_thread::sleep_for(milliseconds(50));
    if (_gotDeauth)   return Result::NOGO_Deauthed;
    if (!_gotAssocOk) return Result::NOGO_NoAssocResp;

    auto h0 = steady_clock::now();
    while (steady_clock::now() - h0 < seconds(holdSeconds)) {
        if (_gotDeauth) return Result::NOGO_Deauthed;
        std::this_thread::sleep_for(milliseconds(100));
    }
    return Result::GO_LinkHeld;
}


// ---- beacon scan: discover BSSID + channel + RSN (security/discovery parity) -
void StationMode::onScanFrame(const uint8_t* frame, size_t len) {
    ApInfo info;
    if (ScanProbe::parseBeacon(frame, len, _scanSsid, info)) {
        if (!_scanResult.found || info.rssi > _scanResult.rssi) _scanResult = info;
    }
}

ApInfo StationMode::scanForSsid(const char* ssid, int channelHint, int perChannelMs) {
    using namespace std::chrono;
    _scanSsid = ssid; _scanResult = ApInfo{};
    // Order = likelihood for a LEGAL APFPV link first, then catch-all:
    //   1) hint (the configured/last APFPV channel)
    //   2) 5.2 GHz UNII-1 (36/40/44/48) — the legal DE 200 mW @ 20 MHz APFPV band
    //      (PSD cap 10 mW/MHz x 20 MHz = 200 mW). A compliant VTX lives here.
    //   3) the rest of 5 GHz (UNII-2A/2C DFS, UNII-3 149-165) + common 2.4 GHz,
    //      so we can still find a test hotspot/router parked off the legal band.
    int channels[] = { channelHint,
                       36, 40, 44, 48,                 // UNII-1 — legal DE 200 mW
                       52, 56, 60, 64,                 // UNII-2A (DFS)
                       149, 153, 157, 161, 165,        // UNII-3
                       1, 6, 11 };                     // 2.4 GHz (common)
    for (int ch : channels) {
        if (ch <= 0) continue;
        _scanResult = ApInfo{};   // reset per channel — don't carry a bleed match over
        _rm.set_channel_bwmode((uint8_t)ch, 0, CHANNEL_WIDTH_20);  // tune radio (devourer API)
        auto t0 = steady_clock::now();
        while (steady_clock::now() - t0 < milliseconds(perChannelMs)) {
            // Accept only a CLEAN on-channel reception: the beacon's DS-param
            // channel must equal the tuned channel. This rejects 2.4GHz adjacent-
            // channel bleed (hearing a ch6 AP while tuned to ch1) that otherwise
            // returns the WRONG channel -> we arm off-channel -> auth never ACKs.
            if (_scanResult.found && (_scanResult.channel == ch || _scanResult.channel == 0)) {
                _scanResult.channel = ch; return _scanResult;
            }
            std::this_thread::sleep_for(milliseconds(20));
        }
    }
    return _scanResult;   // .found == false if not seen
}

} // namespace apfpv