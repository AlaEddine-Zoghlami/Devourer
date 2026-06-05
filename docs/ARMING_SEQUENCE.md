# Auto-ACK arming sequence — ported from Linux

Source of truth: aircrack-ng/rtl8812au
`hal/rtl8812a/rtl8812a_hal_init.c : hw_var_set_opmode()` (STATION path)

Verified present in devourer `hal/hal_com_reg.h`:
- REG_MACID = 0x0610
- REG_BSSID = 0x0618
- REG_RCR   = 0x0608
- RCR_CBSSID_DATA = BIT(6)   (accept + ACK BSSID-matched data)
- RCR_CBSSID_BCN  = BIT(7)
- MSR = REG_CR + 2 ; STATION/INFRA = 0x02

Sequence:
1. write own MAC -> REG_MACID
2. MSR = (MSR & 0x0C) | 0x02   (set port0 net-type = STATION, keep port1)
3. write AP BSSID -> REG_BSSID
4. REG_RCR = RCR_APM|RCR_AM|RCR_AB|RCR_CBSSID_DATA|RCR_CBSSID_BCN
   (NOT RCR_AAP — that is monitor accept-all)

Open question the probe answers:
Is this register arming sufficient for the hardware MAC to auto-ACK, or is the
firmware/MLME state (the removed `infra_client_with_mlme` opmode cmd) also
required? Hardware test decides.
