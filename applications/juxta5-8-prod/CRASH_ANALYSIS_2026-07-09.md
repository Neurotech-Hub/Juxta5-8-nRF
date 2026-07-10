# Field Failure Analysis — JX_A17026, 2026-07-09 (Test 11), rev 2

Firmware `5.8.2`. Device worn on left ankle; failure at ~1:37 PM EDT, exactly when the
tester began walking into grass. Batteries are soldered — mechanical contact interruption
is excluded. This revision reframes the analysis around **what coincides** in the death
window.

## 1. Reconstructed timeline

| Time (EDT) | Evidence | Event |
|---|---|---|
| 11:58:54 | JXS `day_start`/`boot` | Activation, production init |
| 12:40–13:36 | JXV motion ≈0–5 | Sitting; inactivity multiplier engaged (scan every 15 s × 5 = 75 s) |
| 13:36:55 | JXV motion=37 | Walking begins → multiplier disengages at this tick |
| 13:37:00–13:37:45 | JXB rows every ~15 s | Scan bursts back at base cadence, peer-dense (3–5 devices) |
| 13:37:55 | Last JXV (motion=171, 4.31 V, 24 °C) | Vitals tick completed: ADC + SPI temp + NOR write + thread_analyzer RTT dump |
| **13:37:55–13:38:01** | Scan burst due ~13:38:00 left no JXB rows | **Death window: ≤ 6 seconds** |
| 16:38:20 | JXS `shelf_exit`/`user_connected`/`time_set` | Magnet wake + sync + offload |

Battery healthy at every sample (4.31–4.35 V). The device died within seconds of a
completed vitals tick, right at the next adv/scan burst.

## 2. What the logs exclude

Every soft-reset path leaves a forensic trail; none is present:

| Failure mode | Would have left | Present? |
|---|---|---|
| WDT trip (5 s window, fed from sysworkq) | RESETREAS `DOG` → `boot` + `wdt_recovery_dog` rows ~13:38, logging resumes | No |
| Hard fault / assert (`RESET_ON_FATAL_ERROR` → `sys_reboot`) | `SREQ` → `wdt_recovery_sreq` rows | No |
| CPU lockup | `wdt_recovery_lockup` rows | No |
| Magnet false positive (3 s continuous hold) | `shelf_entry` row + 5 blinks + 5 s delay | No |
| Vitals brownout branch | `low_battery` row (JXV row written first in same handler) | No |

The recovery net was armed (op_mode=PROD, retained-RAM RTC checkpoints at 1 Hz). The only
paths matching "no rows, device apparently asleep 3 h later" are the **silent shelf
entries**: `fresh_boot` (RESETREAS == 0 — a true power-on reset) and the Step 3b early
battery gate (single ADC sample < 2750 mV after any reset → silent shelf). Both point at a
**VDD collapse**, which software cannot produce.

One residual software scenario stays open until checked (§4): a fault at ~13:38 whose reset
corrupted retained RAM (`wdt_recovery_no_rtc`) would park the device in the sync gate —
slow-blinking and connectable for 3 h. The offloaded JXS cannot distinguish this because
the boot-identity rows are written only after the sync connection ends.

## 3. What coincides in the death window

Five things stack up in the same seconds — four electrical, one environmental:

1. **Radio TX at +8 dBm in LDO mode.** `CONFIG_BT_CTLR_TX_PWR_ANTENNA=8` and no DC/DC
   (`CONFIG_BOARD_ENABLE_DCDC*` absent) → each adv/scan burst draws ~25 mA from the supply
   (vs ~7 mA at 0 dBm with DC/DC). Adv bursts run 1 s in every 5 s continuously.
2. **Scan cadence quintuples at the motion transition.** The 13:36:55 vitals tick cleared
   `last_vitals_period_zero_motion`, dropping effective scan interval 75 s → 15 s. Radio
   duty (RX ~6 mA + TX bursts) is at its deployment maximum right at grass entry.
3. **NOR flash program bursts.** The peer-dense environment makes every scan burst end in a
   multi-row JXB flush (~20–25 mA per page program). The vitals JXV write adds another.
   Note: `vitals_work_handler` defers its NOR write only during `BLE_STATE_SCANNING` — a
   NOR program **can** coincide with an *advertising* burst (TX + program ≈ 45–50 mA peak).
4. **Vitals tick overhead.** SAADC sample, LIS2DH12 SPI temp read, and ~15 synchronous RTT
   log lines (`CONFIG_LOG_MODE_IMMEDIATE` + thread_analyzer) — CPU held active longer than
   any other 60 s window.
5. **Triboelectric charging (grass-specific, moisture-independent).** Walking through grass
   with synthetic socks/straps builds static on the wearer; the first discharges occur
   shortly after charging starts — i.e., right at grass entry. An ESD hit to the enclosure
   /strap can cause a POR-class reset or latch-up. Conformal coating does not mitigate ESD.
   This is the only hypothesis that explains *grass specifically* rather than walking
   generally, and it matches "sealed devices still fail."

Hypotheses 1–4 combine into **supply droop → POR**: peak stack-up ~50–60 mA through an
LDO-only power path. From 4.3 V this requires substantial series impedance (thin trace,
protection component, aged cell IR) — your current probing will settle it. Hypothesis 5
requires no impedance at all.

## 4. Decisive evidence still on the device

The `boot` row for the 16:38 wake was written **after** the offload (Step 9 runs post-
disconnect). Reconnect to JX_A17026 and re-pull `JXS20260709.csv`:

- Plain `boot` after `time_set` → device was in System OFF since ~13:38 → **POR confirmed**
  (supply droop or ESD; software fully exonerated).
- `wdt_recovery_no_rtc` present → it fault-reset with corrupted retained RAM and sat
  connectable in the sync gate for 3 h → software path re-opens (see §5.5/§5.6).

## 5. Answers to the specific questions + firmware findings

### 5.1 Should motion be captured less often? — No; the capture itself is not a plausible conflict
The motion pipeline is: LIS2DH12 INT1 (non-latched, HP-filtered) → GPIOTE edge IRQ →
`motion_events++`. Worst case ~100 Hz × a few µs = <0.1 % CPU, no SPI in the ISR, no
logging (LOG_DBG is compiled out at INF), no interaction with the radio scheduler (GPIOTE
runs at priority 5, MPSL at 0). The counter is read/cleared under `irq_lock` once per
vitals tick. Reducing ODR or vitals frequency would not remove any identified crash
mechanism. Bench tests at >1000 counts/row already proved the pipeline survives sustained
motion.

**What *is* worth changing is the consequence of motion, not the capture:** the binary
zero/nonzero → 5×/1× scan-cadence step means radio duty jumps 5× in one tick. If supply
droop is real, that step is the software-controlled amplifier.

### 5.2 Experiments, cheapest first (each isolates one hypothesis)
1. **Drop TX power to 0 dBm** (`CONFIG_BT_CTLR_TX_PWR_ANTENNA=0`) for one field test.
   Halves-to-quarters the single largest current spike. Failures stop → supply droop.
2. **Probe VDD, not just current.** µs-scale dips are invisible to averaging meters; use a
   scope on VDD/VDDH with a falling-edge trigger just below nominal, or a PPK2 in
   source-meter mode. Reproduce the burst stack-up on the bench: force scan cadence to
   15 s in a peer-dense room and watch adv-burst + NOR-program coincidences. Note a shunt
   adds series resistance and may itself induce the failure — that's informative, not a
   nuisance.
3. **ESD test:** with the device sealed and worn, scuff on carpet/grass and touch the
   enclosure repeatedly; or bench ESD gun if available. Also check the failure history
   against weather — dry days favor ESD.
4. **Enable the DC/DC regulators** if the board has the inductors fitted (REG1, and REG0 if
   VDDH-supplied). This is the single biggest peak-current reduction available
   (~2× on radio) and reduces droop exposure permanently.

### 5.3 HIGH — Silent shelf paths destroy all forensics (unchanged from rev 1)
`fresh_boot → enter_shelf_mode()` and the Step 3b battery gate leave zero trace, making a
field POR indistinguishable from "never woken." Persist an NVS breadcrumb on every shelf
entry (RESETREAS, reason code, boot counter) and emit it as a JXS row at the next
valid-clock boot. This converts every future silent failure into an attributable one.

### 5.4 HIGH — Use POFCON as a droop witness
The nRF52840 power-fail comparator (VDDH variant for high-voltage mode) fires an IRQ while
the CPU still has time to set a `.noinit` flag. Combined with 5.3, the next failure
self-reports "VDD sagged at uptime X" vs "clean POR with no warning" (ESD-like).

### 5.5 MEDIUM — Step 3b battery gate can mis-shelve on a recovering supply
A single 14-bit SAADC sample taken immediately after a reset — while VDD may still be
slewing — silently shelves the device below 2750 mV. If the root cause is a transient
droop + reset, this gate plausibly converts a recoverable glitch into a 3-hour outage.
Require 2–3 consistent low samples a few ms apart before shelving, and breadcrumb it.

### 5.6 MEDIUM — Avoid NOR-program / radio-TX coincidence
`vitals_work_handler` defers the NOR write during `BLE_STATE_SCANNING` but not
`BLE_STATE_ADVERTISING`; JXB flush runs radio-idle, but the JXV write can land inside an
adv burst. Extending the deferral to the advertising state removes the largest identified
peak-current coincidence at near-zero cost.

### 5.7 LOW — Soften the inactivity-multiplier step (only if droop confirms)
Ramp the effective scan interval over 2–3 vitals ticks (75 → 45 → 15 s) instead of one
step. Listed last deliberately: it treats the amplifier, not the cause, and changes core
behavior — do not implement unless the electrical hypothesis is confirmed.

### 5.8 Data-ambiguity fixes (unchanged from rev 1)
- Write `boot`/`wdt_recovery_*` rows when the clock first becomes valid, not at Step 9, so
  a sync-connection offload includes the current boot's identity.
- Don't log `shelf_exit` on `wdt_recovery_no_rtc` boots; it masks recovery boots in data.
- Residual margins worth watching, though both would fault → SREQ → recovery rows (which
  does not match this signature): MPSL Work stack 464 B free; ISR stack 44 % at rest.

## 6. Bottom line

The failure signature is a power-on-class reset, not a firmware crash — the WDT/fault/
recovery net was armed and left no trace. Motion capture itself is exonerated; the motion
*transition* matters because it quintuples radio duty (at +8 dBm, LDO mode) exactly when
the wearer starts moving, and — grass-specifically — exactly when triboelectric charging
begins. Your current/voltage probing plus the two cheap experiments (0 dBm test, re-offload
of JXS) should separate supply droop from ESD within one bench session and one field test.
