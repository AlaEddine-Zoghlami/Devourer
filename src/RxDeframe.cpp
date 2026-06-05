// ============================================================================
//  RxDeframe.cpp — 802.11 data -> UDP/RTP -> port 5600 + RSSI tap (impl)
//  Declarations in RxDeframe.h. Wire onPacket() as devourer's RX callback.
// ============================================================================
#include "RxDeframe.h"
#include "ApfpvStation.h"
#include <cstring>

namespace apfpv {

RxDeframe::RxDeframe(const Mac& self, const Mac& bssid, Wpa2Supplicant* wpa,
                     LqFeedback* lq, OnRtpFn onRtp)
    : _self(self), _bssid(bssid), _wpa(wpa), _lq(lq), _onRtp(std::move(onRtp)) {}

int RxDeframe::toDbm(uint8_t r) {
    // RxAtrib.rssi[] is devourer's gain_trsw byte for the path: the RX
    // descriptor's {TRSW(bit7), gain[6:0]} field (FrameParser.cpp fills it from
    // driver_data.gain_trsw). It is a GAIN index, not dBm — so the placeholder
    // r/2-95 was wrong. The RTL8812 (Jaguar) phystatus path in the Realtek HAL
    // converts the OFDM RX gain to power as:  rx_pwr_dBm = gain_index - 110
    // (the phydm rx_pwr_all base; LNA/VGA gain index maps ~1:1 to dB, offset by
    // the front-end reference of -110 dBm at gain 0). We mask the TRSW bit and
    // apply that. Clamp to a sane RF window.
    int gain = r & 0x7F;                 // strip TRSW flag, keep gain[6:0]
    int dbm  = gain - 110;               // Jaguar OFDM gain-index -> dBm
    if (dbm < -100) dbm = -100;
    if (dbm > -10)  dbm = -10;
    return dbm;
}

void RxDeframe::onPacket(const Packet& pkt) {
    if (_station) _station->notifyRxAlive();   // any RX = link alive (supervisor)
    if (_lq) _lq->update(toDbm(pkt.RxAtrib.rssi[0]), toDbm(pkt.RxAtrib.rssi[1]));

    const uint8_t* f = pkt.Data.data();
    size_t len = pkt.Data.size();
    if (len < 24) return;

    uint16_t fc = f[0] | (f[1] << 8);
    // mgmt deauth/disassoc from our AP -> immediate link-loss signal
    if (((fc >> 2) & 0x3) == 0x0) {
        uint8_t sub = (fc >> 4) & 0xF;
        if ((sub == 0xC || sub == 0xA) && _station) _station->notifyDeauth();
        return;
    }
    if (((fc >> 2) & 0x3) != 0x2) return;          // data frames only
    if (!(fc & 0x0200)) return;                    // from-DS (AP->STA)
    if (std::memcmp(f + 4, _self.data(), 6) != 0) return;  // addr1 == us

    size_t hdrLen = 24;
    if ((fc & 0x0300) == 0x0300) hdrLen += 6;      // 4-addr
    if (fc & 0x8000) hdrLen += 2;                  // QoS
    if (len <= hdrLen) return;

    const uint8_t* body = f + hdrLen; size_t bodyLen = len - hdrLen;
    std::vector<uint8_t> plain; const uint8_t* llc; size_t llcLen;
    if (fc & 0x4000) {                             // Protected
        if (!_wpa || !_wpa->ready()) return;
        if (!_wpa->decryptData(f, len, plain)) return;
        llc = plain.data(); llcLen = plain.size();
    } else { llc = body; llcLen = bodyLen; }

    if (llcLen < 8) return;
    static const uint8_t snap[6] = {0xaa,0xaa,0x03,0x00,0x00,0x00};
    if (std::memcmp(llc, snap, 6) != 0) return;
    uint16_t ethertype = (llc[6] << 8) | llc[7];
    // EAPOL (0x888E): the WPA2 4-way handshake. MUST route to the supplicant or
    // the handshake never completes and the link stalls at Handshaking.
    if (ethertype == 0x888E) {
        if (_wpa) _wpa->onEapolKey(llc + 8, llcLen - 8);
        return;
    }
    if (ethertype != 0x0800) return; // IPv4 (video)
    const uint8_t* ip = llc + 8; size_t ipLen = llcLen - 8;
    if (ipLen < 20 || (ip[0] >> 4) != 4 || ip[9] != 17) return; // IPv4/UDP
    size_t ihl = (ip[0] & 0x0f) * 4;
    if (ipLen < ihl + 8) return;
    const uint8_t* udp = ip + ihl;
    uint16_t dport = (udp[2] << 8) | udp[3];
    uint16_t udlen = (udp[4] << 8) | udp[5];
    // DHCP: replies (OFFER/ACK) arrive on UDP port 68 (BOOTP client). Forward
    // the UDP payload (BOOTP message) to the DHCP state machine so DORA can
    // complete. Without this, the client never sees OFFER and DHCP stalls.
    if (dport == 68) {
        if (_onDhcp && udlen >= 8 && (size_t)udlen <= ipLen - ihl)
            _onDhcp(udp + 8, udlen - 8);
        return;
    }
    if (dport != 5600) return;   // video port
    uint16_t ulen = udlen;
    if (ulen < 8 || (size_t)ulen > ipLen - ihl) return;
    const uint8_t* rtp = udp + 8; size_t rtpLen = ulen - 8;
    if (rtpLen && _onRtp) _onRtp(rtp, rtpLen);
}

} // namespace apfpv
