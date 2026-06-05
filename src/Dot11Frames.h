#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <array>
namespace apfpv {
using Mac = std::array<uint8_t,6>;
std::vector<uint8_t> BuildAuthOpenSeq1(const Mac& self, const Mac& bssid);
// Open (no-privacy) beacon for the EIRP-calibration "VRX beacon" mode: lets a
// phone Wi-Fi scan see the dongle as an AP so its EIRP can be measured.
std::vector<uint8_t> BuildBeacon(const Mac& self, const std::string& ssid, uint8_t channel);
std::vector<uint8_t> BuildAssocRequest(const Mac& self, const Mac& bssid, const std::string& ssid);
// Negotiated variant: RSN IE reflects the cipher chosen from the AP's beacon.
std::vector<uint8_t> BuildAssocRequest(const Mac& self, const Mac& bssid, const std::string& ssid,
                                       uint32_t pairwiseCipher, uint32_t groupCipher);
std::vector<uint8_t> BuildEapolKey(const Mac& self, const Mac& bssid, const std::vector<uint8_t>& keyBody);
}
