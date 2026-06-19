## Firmware Updates (DFU) over BLE for juxta-ble on Juxta5-4_nRF52840

This document outlines a simple, robust path to enable over-the-air firmware updates using Zephyr MCUboot and MCUmgr (SMP over BLE), aligned with the device’s magnet-based activation flow and without changing the existing `ble_service.c` logic.

### Overview
- **Transport/Protocol**: MCUmgr SMP service over BLE (standard GATT service) with MCUboot two-slot image swap.
- **Security**: No authentication (in-house/dev use). SMP is only exposed when the device explicitly enters DFU mode.
- **User Flow**: DFU mode is entered via a long magnet hold at boot; normal program starts otherwise.

### Explicit Mode Selection at Boot
- **Magnet <1s**: Normal program (current flow). Connectable datetime sync, then full system init; Hublink service runs as usual.
- **Magnet >5s**: DFU mode. Minimal initialization to expose the MCUmgr BLE SMP service for image upload; no app services are started. After upload, reset to boot the new image.

### What stays unchanged
- `applications/juxta-ble/src/ble_service.c` remains intact and is used only in normal mode.
- No image staging in FRAM is required; MCUmgr writes directly to the secondary flash slot.
- Board partitions are already suitable for MCUboot two-slot swaps.

### Files to adjust
- `applications/juxta-ble/prj.conf`
  - Enable MCUboot (bootloader, two-slot swap) and MCUmgr core with BLE transport.
  - Enable image and OS management groups.
  - Keep `CONFIG_BT_SMP=n` for unauthenticated DFU (development/in-house).
  - Optional later: tune BLE/MCUmgr buffers if throughput is insufficient; current ATT_MTU 247 is acceptable to start.

- `applications/juxta-ble/child_image/mcuboot.conf` (new)
  - Configure MCUboot signing (RSA-2048) and swap policy (test-then-confirm by default).
  - Point to the local private key file used for signing. Do not commit private keys.

- Local signing key (not in repo)
  - Generate an RSA-2048 keypair for signing. The build uses this to produce signed images (`app_update.bin` / `zephyr.signed.bin`).

- `applications/juxta-ble/src/main.c`
  - Add magnet-hold timing check at the very start of `main()` (before BLE/vitals/FRAM init):
    - If **<1s**: follow current program as-is (datetime sync, Hublink, state machine).
    - If **>5s**: enter **DFU mode**:
      - Initialize BLE only; register MCUmgr SMP over BLE.
      - Start connectable advertising; optionally include the SMP service UUID in advertising data.
      - Do not initialize FRAMFS, LIS2DH, ADC, or the state machine.
      - Ensure the watchdog continues to be fed (and consider a longer timeout during DFU).
      - Log clear DFU-mode status and keep the system idle except for BLE.
  - On next boot after update: simplest path is to auto-confirm success early in normal mode (optional), otherwise just re-enter the normal magnet gate and proceed.

- Optional docs
  - Add a short note in `applications/juxta-ble/HUBLINK.md` that DFU mode is entered by holding the magnet for >5s at boot and is handled via the standard MCUmgr SMP BLE service.

### User workflow (no auth)
1. Power the device while holding the magnet for >5s → device enters DFU mode and advertises as usual (now with SMP service available).
2. Use your preferred tool (e.g., MCUmgr CLI or a Nordic mobile app that supports MCUmgr) to:
   - List images (slot-0/slot-1),
   - Upload the signed image,
   - Mark as test if needed (some tools do this automatically),
   - Reset the device.
3. Device boots the new image. If auto-confirm is enabled in code, the image is confirmed early; otherwise, the normal flow resumes and can confirm after health checks.

### Operational details
- **Advertising**: In DFU mode, advertise connectable; including SMP UUID is optional. Keep the existing device name convention to make discovery easy.
- **Watchdog**: Ensure the periodic feed timer runs during DFU; increase `WDT` timeout if uploads are long.
- **Throughput**: Start with current 247 MTU; only tune BLE buffer counts and MCUmgr buffers if DFU feels slow.

### Validation checklist
- Enter DFU via magnet >5s; verify SMP service is present and upload succeeds.
- Verify swap behavior: upload smaller test build, reset, ensure new app boots.
- Verify rollback safety by simulating a failure before confirm (if not using auto-confirm), ensuring MCUboot returns to the previous image.
- Verify watchdog does not reset the device during a long upload.

### Risks and mitigations
- **Unauthorized updates**: Mitigated by only exposing SMP in DFU mode via explicit magnet action; unauthenticated by design per requirements.
- **Upload disruptions**: Two-slot MCUboot swap protects against bricking; consider auto-confirm only after minimal sanity checks if desired.
- **Radio starvation**: Keep logs modest and avoid heavy peripherals in DFU mode.

### Build expectations
- Enable sysbuild so MCUboot is built alongside the app.
- Output artifacts include a signed image for DFU. Use the signed image when uploading over MCUmgr.

This plan keeps DFU explicit, simple for users, and isolated from your application’s normal services and state machine.

