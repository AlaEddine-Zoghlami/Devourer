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

## ANSWERED (2026-06-06, on-hardware E2E, OnePlus 8T + EMAX 8812AU)

Neither is the blocker. We ported BOTH:
- register arming (MACID/MSR/BSSID/RCR) — present in `StationMode::arm`.
- the firmware H2C `MEDIA_STATUS_RPT` (0x01, connect, macid 0) via a new
  `RtlUsbAdapter::fillH2CCmd` (port of `fill_h2c_cmd_8812`). Firmware is
  downloaded+ready (`FirmwareManager`), and the H2C write reports `sent=1`.

The connect STILL fails at auth with `TXFAIL_NoAuthResp`: the AP never replies,
because the auth-request never reaches it. Discovery (RX) is flawless — the
dongle hears OpenIPC + dozens of APs — but EVERY TX-dependent step fails
(auth BMC=0 and BMC=1, assoc, and the VRX beacon-cal), i.e. the radio is
RX-good / TX-silent.

Root cause = devourer issue #50 ("on-air silence"), documented in
`src/RtlJaguarDevice.cpp` (~line 164): injected frames are submitted to the
chip but do not radiate, and the root cause is one that "vendor-control-write
replay can't reach" (the TX bring-up is incomplete in a way that copying the
kernel's register/control writes does not fix). Until #50 is solved, APFPV
station mode cannot complete — it needs TX. (WFB-ng works because it is RX-only.)

Fixing #50 realistically needs a usbmon capture of a WORKING kernel-driver TX
session (same chip) diffed against devourer's USB traffic, not blind register
iteration — best done on the Linux desktop `apfpv_probe` harness.
