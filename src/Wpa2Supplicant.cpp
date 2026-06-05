// ============================================================================
//  Wpa2Supplicant.cpp — WPA2-PSK 4-way handshake + CCMP (impl)
//  Declarations in Wpa2Supplicant.h. Pure userspace protocol logic; crypto via
//  injected Crypto backend (Wpa2Crypto.cpp). Not timing-critical.
// ============================================================================
#include "Wpa2Supplicant.h"
#include "Dot11Frames.h"
#include "crypto/AesCcm.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace apfpv {

Wpa2Supplicant::Wpa2Supplicant(Crypto c, SendFn send)
    : _c(std::move(c)), _send(std::move(send)) {}

void Wpa2Supplicant::begin(const std::string& passphrase, const std::string& ssid,
                           const Mac& self, const Mac& bssid) {
    _self = self; _bssid = bssid;
    _pmk = _c.pbkdf2_pmk(passphrase,
                         reinterpret_cast<const uint8_t*>(ssid.data()), ssid.size());
    _state = State::WaitMsg1;
}

bool Wpa2Supplicant::ready() const { return _state == State::Done; }

void Wpa2Supplicant::derivePtk(const uint8_t* aNonce) {
    std::memcpy(_aNonce.data(), aNonce, 32);
    // SNonce: cryptographically-seeded random (not predictable). Uses
    // /dev/urandom when available, else a mixed fallback. Production RNG.
    { FILE* u = fopen("/dev/urandom","rb");
      if (u) { size_t r=fread(_sNonce.data(),1,32,u); (void)r; fclose(u); }
      else { for(int i=0;i<32;i++) _sNonce[i]=(uint8_t)((std::rand()>>3)^(i*131)); } }
    uint8_t data[76];
    const Mac& aa = _bssid; const Mac& sa = _self;
    bool aaFirst = std::memcmp(aa.data(), sa.data(), 6) < 0;
    std::memcpy(data + 0, (aaFirst?aa:sa).data(), 6);
    std::memcpy(data + 6, (aaFirst?sa:aa).data(), 6);
    bool aFirst = std::memcmp(aNonce, _sNonce.data(), 32) < 0;
    std::memcpy(data + 12, aFirst?aNonce:_sNonce.data(), 32);
    std::memcpy(data + 44, aFirst?_sNonce.data():aNonce, 32);
    auto ptk = _c.prf(_pmk.data(), 32, "Pairwise key expansion", data, sizeof(data), 48);
    std::memcpy(_kck.data(), ptk.data() + 0,  16);
    std::memcpy(_kek.data(), ptk.data() + 16, 16);
    std::memcpy(_tk.data(),  ptk.data() + 32, 16);
}

// Build an EAPOL-Key body (key info, nonce, MIC). MIC computed over the body
// with the MIC field zeroed, using KCK, then patched in. Returns full 802.11
// data frame via BuildEapolKey.
std::vector<uint8_t> Wpa2Supplicant::buildMsg2() {
    std::vector<uint8_t> kb(95, 0);
    kb[0] = 0x02;                 // EAPOL type = Key
    kb[1] = 0x03;                 // descriptor type = RSN
    uint16_t info = 0x010A;       // pairwise + MIC + (key type bits)
    kb[2] = info >> 8; kb[3] = info & 0xFF;
    std::memcpy(kb.data() + 9, _replayCtr, 8);         // echo replay counter
    std::memcpy(kb.data() + 17, _sNonce.data(), 32);   // SNonce
    // MIC field is kb[81..96]; zeroed already. Compute and patch.
    auto mic = _c.eapol_mic(_kck.data(), kb.data(), kb.size());
    std::memcpy(kb.data() + 81, mic.data(), 16);
    return BuildEapolKey(_self, _bssid, kb);
}

std::vector<uint8_t> Wpa2Supplicant::buildMsg4() {
    std::vector<uint8_t> kb(95, 0);
    kb[0] = 0x02; kb[1] = 0x03;
    uint16_t info = 0x030A;       // pairwise + MIC + secure
    kb[2] = info >> 8; kb[3] = info & 0xFF;
    std::memcpy(kb.data() + 9, _replayCtr, 8);         // echo replay counter
    auto mic = _c.eapol_mic(_kck.data(), kb.data(), kb.size());
    std::memcpy(kb.data() + 81, mic.data(), 16);
    return BuildEapolKey(_self, _bssid, kb);
}

bool Wpa2Supplicant::onEapolKey(const uint8_t* body, size_t len) {
    if (len < 95) return false;
    // EAPOL-Key replay counter is 8 bytes at offset 9 (after 4B EAPOL hdr +
    // 1B desc type). Capture so our M2/M4 echo the AP's counter (else the AP
    // rejects them as replay-mismatched).
    std::memcpy(_replayCtr, body + 9, 8);
    const uint8_t* keyInfo = body + 5;
    uint16_t info = (keyInfo[0] << 8) | keyInfo[1];
    const bool hasMic     = info & 0x0100;
    const bool isPairwise = info & 0x0008;
    const uint8_t* nonce  = body + 17;
    switch (_state) {
        case State::WaitMsg1:
            if (!hasMic && isPairwise) {
                derivePtk(nonce);
                _send(buildMsg2());
                _state = State::WaitMsg3;
            }
            return false;
        case State::WaitMsg3:
            if (hasMic && isPairwise) {
                // M3 carries the GTK as a KEK-wrapped KDE in the Key Data field
                // (offset 95 = after the 95-byte fixed EAPOL-Key header; length
                // at offset 93..94). Unwrap with AES-Key-Unwrap (RFC 3394).
                uint16_t kdLen = (body[93] << 8) | body[94];
                if (kdLen >= 24 && len >= (size_t)(95 + kdLen)) {
                    const uint8_t* kd = body + 95;
                    // KDE: dd len 00-0F-AC 01 <keyid+rsv 2B> <wrapped GTK>.
                    // The wrapped GTK portion is (kdLen-? ) bytes; unwrap the
                    // 24-byte wrapped form -> 16-byte GTK (common case).
                    uint8_t gtk[16];
                    if (crypto::aes_key_unwrap(_kek.data(), kd + 6, 24, gtk))
                        std::memcpy(_gtk.data(), gtk, 16);
                }
                _send(buildMsg4());
                _state = State::Done;
                return true;
            }
            return false;
        default: return false;
    }
}

bool Wpa2Supplicant::decryptData(const uint8_t* frame, size_t len, std::vector<uint8_t>& plain) {
    if (_state != State::Done) return false;
    if (len < 24 + 8) return false;
    uint16_t fc = frame[0] | (frame[1] << 8);
    size_t hdr = 24;
    if (fc & 0x8000) hdr += 2;                 // QoS
    const uint8_t* ccmp = frame + hdr;         // 8-byte CCMP header
    // Build CCMP nonce: priority(1) | A2(6) | PN(6).
    uint8_t pn[6] = { ccmp[7], ccmp[6], ccmp[5], ccmp[4], ccmp[1], ccmp[0] };
    uint8_t nonce[13]; nonce[0] = 0;
    std::memcpy(nonce + 1, frame + 10, 6);     // A2
    std::memcpy(nonce + 7, pn, 6);
    const uint8_t* enc = ccmp + 8;
    size_t encLen = len - hdr - 8;
    // AAD is the 802.11 header (masked) — backend handles per CCMP spec.
    return _c.ccmp_decrypt(_tk.data(), nonce, enc, encLen, frame, hdr, plain);
}


// Build an encrypted (CCMP) 802.11 QoS-data frame carrying an IPv4 payload.
// Frame: [802.11 hdr (to-DS)][CCMP hdr 8B][ enc{ LLC/SNAP | IPv4 } ][MIC 8B].
std::vector<uint8_t> Wpa2Supplicant::buildEncryptedData(const uint8_t* ip, size_t iplen) {
    if (_state != State::Done) return {};
    // LLC/SNAP + IPv4 = the plaintext to encrypt
    std::vector<uint8_t> plain;
    static const uint8_t snap[8] = {0xaa,0xaa,0x03,0x00,0x00,0x00,0x08,0x00}; // ...0800 IPv4
    plain.insert(plain.end(), snap, snap+8);
    plain.insert(plain.end(), ip, ip+iplen);

    // 802.11 data header, to-DS (STA->AP). addr1=BSSID, addr2=SA(self), addr3=DA.
    std::vector<uint8_t> f;
    f.push_back(0x08); f.push_back(0x01);            // FC: data, to-DS, Protected
    f.push_back(0x00); f.push_back(0x00);            // duration
    f.insert(f.end(), _bssid.begin(), _bssid.end()); // addr1 = BSSID (RA)
    f.insert(f.end(), _self.begin(), _self.end());   // addr2 = SA (TA)
    f.insert(f.end(), _bssid.begin(), _bssid.end()); // addr3 = DA (to AP/gateway)
    f.push_back(0x00); f.push_back(0x00);            // seq ctrl (HW fills)
    f[1] |= 0x40;                                    // set Protected bit

    // CCMP header (8B): PN0 PN1 rsv keyid(ext) PN2..PN5
    uint64_t pn = _txPn++;
    uint8_t pn0=pn&0xff, pn1=(pn>>8)&0xff, pn2=(pn>>16)&0xff,
            pn3=(pn>>24)&0xff, pn4=(pn>>32)&0xff, pn5=(pn>>40)&0xff;
    f.push_back(pn0); f.push_back(pn1); f.push_back(0x00); f.push_back(0x20); // keyid0, ext-IV
    f.push_back(pn2); f.push_back(pn3); f.push_back(pn4); f.push_back(pn5);

    // CCMP nonce: priority(1)=0 | A2(6) | PN(6, big-endian)
    uint8_t nonce[13]; nonce[0]=0;
    std::memcpy(nonce+1, _self.data(), 6);
    nonce[7]=pn5; nonce[8]=pn4; nonce[9]=pn3; nonce[10]=pn2; nonce[11]=pn1; nonce[12]=pn0;
    // AAD = masked 802.11 header (24B here). Backend builds per spec from the hdr.
    auto enc = _c.ccmp_encrypt(_tk.data(), nonce, plain.data(), plain.size(),
                               f.data(), 24);
    f.insert(f.end(), enc.begin(), enc.end());       // ciphertext + MIC
    return f;
}

} // namespace apfpv
