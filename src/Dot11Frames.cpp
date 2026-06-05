// ============================================================================
//  Dot11Frames.cpp  —  802.11 management + EAPOL frame construction
// ============================================================================
//  Builds the raw frames StationMode injects via RtlJaguarDevice::send_packet().
//  These are TX of management/EAPOL frames — latency-tolerant (no SIFS deadline
//  applies to our *sending*; the SIFS deadline is on ACK-ing received unicast,
//  which is the hardware's job, not ours).
//
//  Frame layout we emit to send_packet(): the device's TX expects a radiotap
//  header (devourer prepends/handles the TX descriptor); here we build the
//  802.11 MPDU. Wiring of the radiotap/TX-desc prefix matches what upstream
//  devourer already does for monitor injection (see RtlJaguarDevice TX path).
//
//  Scope of this file: Auth (open seq1), Assoc-Request (with SSID + rates +
//  RSN IE for WPA2), and the EAPOL-Key frame shell for the 4-way handshake.
// ============================================================================

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <array>

#include "Dot11Frames.h"
namespace apfpv {

using Mac = std::array<uint8_t,6>;

// ---- 802.11 frame-control builders ----------------------------------------
static constexpr uint16_t FC_MGMT          = 0x0000;
static constexpr uint16_t FC_SUB_ASSOC_REQ = 0x0000;  // mgmt subtype 0
static constexpr uint16_t FC_SUB_AUTH      = 0x00B0;   // mgmt subtype 11 (<<4)
static constexpr uint16_t FC_DATA          = 0x0008;   // type data
static constexpr uint16_t FC_TODS          = 0x0100;   // to-DS (station->AP)

// Generic 802.11 MAC header (24 bytes, 3-address)
static void put_hdr(std::vector<uint8_t>& f, uint16_t fc,
                    const Mac& a1, const Mac& a2, const Mac& a3,
                    uint16_t seq = 0) {
    f.push_back(fc & 0xFF); f.push_back(fc >> 8);   // frame control
    f.push_back(0); f.push_back(0);                 // duration (HW fills)
    f.insert(f.end(), a1.begin(), a1.end());        // addr1 = RA (dest)
    f.insert(f.end(), a2.begin(), a2.end());        // addr2 = TA (us)
    f.insert(f.end(), a3.begin(), a3.end());        // addr3 = BSSID
    f.push_back((seq << 4) & 0xFF); f.push_back((seq << 4) >> 8); // seq ctrl
}

static void put_le16(std::vector<uint8_t>& f, uint16_t v){ f.push_back(v&0xFF); f.push_back(v>>8); }

// ---- Auth, open-system, sequence 1 -----------------------------------------
std::vector<uint8_t> BuildAuthOpenSeq1(const Mac& self, const Mac& bssid) {
    std::vector<uint8_t> f;
    put_hdr(f, FC_MGMT | FC_SUB_AUTH, bssid, self, bssid);
    put_le16(f, 0);   // auth algorithm = Open System
    put_le16(f, 1);   // auth sequence  = 1
    put_le16(f, 0);   // status code    = 0
    return f;
}

// ---- Association Request (SSID + supported rates + RSN IE for WPA2) ---------
std::vector<uint8_t> BuildAssocRequest(const Mac& self, const Mac& bssid,
                                       const std::string& ssid) {
    std::vector<uint8_t> f;
    put_hdr(f, FC_MGMT | FC_SUB_ASSOC_REQ, bssid, self, bssid);

    put_le16(f, 0x0431);   // capability info (ESS + privacy + short preamble/slot)
    put_le16(f, 3);        // listen interval

    // IE: SSID (0)
    f.push_back(0x00); f.push_back((uint8_t)ssid.size());
    f.insert(f.end(), ssid.begin(), ssid.end());

    // IE: Supported Rates (1) — 1,2,5.5,11,6,9,12,18 Mbps (basic+OFDM)
    static const uint8_t rates[] = {0x82,0x84,0x8b,0x96,0x0c,0x12,0x18,0x24};
    f.push_back(0x01); f.push_back(sizeof(rates));
    f.insert(f.end(), rates, rates+sizeof(rates));

    // IE: RSN (48) — WPA2-PSK / CCMP. Required so the AP starts the 4-way HS.
    //   version 1; group=CCMP; pairwise=CCMP; akm=PSK
    static const uint8_t rsn[] = {
        0x01,0x00,                      // RSN version 1
        0x00,0x0f,0xac,0x04,            // group cipher  = CCMP (00-0F-AC-4)
        0x01,0x00, 0x00,0x0f,0xac,0x04, // 1 pairwise    = CCMP
        0x01,0x00, 0x00,0x0f,0xac,0x02, // 1 AKM         = PSK  (00-0F-AC-2)
        0x00,0x00                       // RSN capabilities
    };
    f.push_back(0x30); f.push_back(sizeof(rsn));
    f.insert(f.end(), rsn, rsn+sizeof(rsn));

    return f;
}

// ---- EAPOL-Key frame shell (for WPA2 4-way handshake; msg 2/4 from us) ------
//   The KCK/KEK come from the PTK (derived in Wpa2Supplicant). This builds the
//   802.11 DATA + LLC/SNAP(0x888E) + EAPOL-Key body; MIC is filled by caller
//   after it computes HMAC over the assembled buffer with MIC field zeroed.
std::vector<uint8_t> BuildEapolKey(const Mac& self, const Mac& bssid,
                                   const std::vector<uint8_t>& keyBody) {
    std::vector<uint8_t> f;
    put_hdr(f, FC_DATA | FC_TODS, bssid, self, bssid);
    // LLC/SNAP for EAPOL
    static const uint8_t snap[] = {0xaa,0xaa,0x03,0x00,0x00,0x00,0x88,0x8e};
    f.insert(f.end(), snap, snap+sizeof(snap));
    f.insert(f.end(), keyBody.begin(), keyBody.end());
    return f;
}


// Negotiated RSN: build the IE from the scanned pairwise/group cipher suites.
std::vector<uint8_t> BuildAssocRequest(const Mac& self, const Mac& bssid,
                                       const std::string& ssid,
                                       uint32_t pairwiseCipher, uint32_t groupCipher) {
    // Reuse the base frame, then append an RSN IE with the negotiated ciphers.
    std::vector<uint8_t> f = BuildAssocRequest(self, bssid, ssid);
    // strip the trailing hardcoded RSN (last sizeof(rsn)+2 bytes) and re-add.
    // Simpler: the base already appended a CCMP/PSK RSN; if negotiated == CCMP
    // it is identical, so only rebuild when different.
    if (pairwiseCipher == 0x000FAC04 && groupCipher == 0x000FAC04) return f;
    // remove last RSN IE (0x30 ... ) — find last 0x30 tag
    for (size_t i = f.size(); i-- > 0; ) {
        if (f[i] == 0x30 && i + 1 < f.size()) { f.resize(i); break; }
    }
    auto be32 = [&](uint32_t v){ f.push_back(v>>24); f.push_back(v>>16); f.push_back(v>>8); f.push_back(v); };
    f.push_back(0x30); f.push_back(20);          // RSN IE, len 20
    f.push_back(0x01); f.push_back(0x00);        // version 1
    be32(groupCipher);                           // group
    f.push_back(0x01); f.push_back(0x00); be32(pairwiseCipher);   // 1 pairwise
    f.push_back(0x01); f.push_back(0x00); be32(0x000FAC02);       // 1 AKM = PSK
    f.push_back(0x00); f.push_back(0x00);        // RSN caps
    return f;
}

} // namespace apfpv
