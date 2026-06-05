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
#include "RadioManagementModule.h"

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
    uint8_t msr = (uint8_t)((_dev.rtw_read8(MSR) & 0x0C) | HW_STATE_STATION);
    _dev.rtw_write8(MSR, msr);
    // (3) AP BSSID -> REG_BSSID
    for (int i = 0; i < 4; ++i) _dev.rtw_write8(REG_BSSID + i, bssid.b[i]);
    _dev.rtw_write16(REG_BSSID + 4, (uint16_t)(bssid.b[4] | (bssid.b[5] << 8)));
    // (4) RCR: station address-matched + accept/ACK BSSID-matched data
    uint32_t rcr = RCR_APM | RCR_AM | RCR_AB | RCR_CBSSID_DATA | RCR_CBSSID_BCN;
    _dev.rtw_write32(REG_RCR, rcr);
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
    switch (fc_subtype(fc)) {
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
    if (!_armed) return Result::Error;
    if (!sendAuthOpenSeq1(self, bssid)) return Result::Error;

    auto t0 = steady_clock::now();
    while (!_gotAuthResp && !_gotDeauth && steady_clock::now() - t0 < seconds(3))
        std::this_thread::sleep_for(milliseconds(50));
    if (!_gotAuthResp) return Result::TXFAIL_NoAuthResp;

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
    // 5GHz UNII-1 channels the EMAX VTX uses (36,40,44,48) + the hint first.
    int channels[] = { channelHint, 36, 40, 44, 48 };
    for (int ch : channels) {
        if (ch <= 0) continue;
        _rm.set_channel_bwmode((uint8_t)ch, 0, CHANNEL_WIDTH_20);  // tune radio (devourer API)
        auto t0 = steady_clock::now();
        while (steady_clock::now() - t0 < milliseconds(perChannelMs)) {
            if (_scanResult.found) { _scanResult.channel = _scanResult.channel ? _scanResult.channel : ch; return _scanResult; }
            std::this_thread::sleep_for(milliseconds(20));
        }
    }
    return _scanResult;   // .found == false if not seen
}

} // namespace apfpv