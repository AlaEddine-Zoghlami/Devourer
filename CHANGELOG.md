# Changelog

## v0.3.1 — 2026-06-10

**devourer** — the capability + JNI layer on the WiFiDriver RTL8812AU userspace driver.

### Highlights
- WPA2 station hardening + configurable static IP (+ AP-mode WIP scaffolding).
- Driver extracted into the **WiFiDriver** submodule; devourer layers the capability API and the
  `apfpv_jni` / `WfbngLink` JNI bridge on top.
- WHOLE_ARCHIVE link fix — static-init translation units (and thus TX) were dropped after the
  library split; fixed so the station path radiates.
- Persistent reconnect + USB replug re-init for the APFPV station path.

Pairs with **WiFiDriver** (the driver) and **PixelPilot** (the Android FPV app).
