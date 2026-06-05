#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include <string>
#include <functional>
#include "Wpa2Crypto.h"
namespace apfpv {
using Mac = std::array<uint8_t,6>;
class Wpa2Supplicant {
public:
    using SendFn = std::function<bool(const std::vector<uint8_t>&)>;
    Wpa2Supplicant(Crypto c, SendFn send);
    void begin(const std::string& passphrase, const std::string& ssid, const Mac& self, const Mac& bssid);
    bool onEapolKey(const uint8_t* body, size_t len);
    bool ready() const;
    bool decryptData(const uint8_t* frame, size_t len, std::vector<uint8_t>& plain);
    // Build an ENCRYPTED 802.11 data frame (CCMP) carrying an IP/UDP payload.
    // Used to TX DHCP (and any station uplink) after the 4-way handshake.
    // 'ipPayload' is a complete IPv4 packet (IP+UDP+BOOTP). Returns the full
    // 802.11 frame ready for send_packet (caller prepends the TX descriptor).
    std::vector<uint8_t> buildEncryptedData(const uint8_t* ipPayload, size_t len);
    const std::array<uint8_t,16>& tk() const { return _tk; }
private:
    enum class State { Idle, WaitMsg1, WaitMsg3, Done };
    void derivePtk(const uint8_t* aNonce);
    std::vector<uint8_t> buildMsg2();
    std::vector<uint8_t> buildMsg4();
    Crypto _c; SendFn _send;
    Mac _self{}, _bssid{};
    std::array<uint8_t,32> _pmk{}; std::array<uint8_t,16> _kck{}, _kek{}, _tk{}, _gtk{};
    std::array<uint8_t,32> _aNonce{}, _sNonce{};
    State _state = State::Idle; uint64_t _replay = 0; uint64_t _txPn = 1; uint8_t _replayCtr[8] = {0};
    std::vector<uint8_t> _lastKeyFrame;
};
}
