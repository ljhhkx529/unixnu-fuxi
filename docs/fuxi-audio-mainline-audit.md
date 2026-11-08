# Fuxi audio mainline audit

Status: host/source audit only. No device execution is claimed.

## Sources and license boundary

The board wiring is recorded from the fixed fuxi source named in
`facts/audio/manifest.toml`. The driver semantics below were checked against Linux mainline commit
`fa4ad798f6f681031775266044ba1b0cf3c670bd`:

- `sound/soc/codecs/cs35l41.c`
- `sound/soc/codecs/cs35l41-lib.c`
- `include/sound/cs35l41.h`
- `drivers/remoteproc/qcom_q6v5_adsp.c`
- `drivers/rpmsg/qcom_glink_smem.c`
- `drivers/soc/qcom/apr.c`
- `sound/soc/qcom/qdsp6/q6apm.c`
- `sound/soc/qcom/qdsp6/q6apm-dai.c`
- `sound/soc/qcom/qdsp6/q6prm.c`

Linux is a behavioral and register-semantics reference. Its GPL implementation, OTP maps and revision patch tables
must not be copied into this BSD-2-Clause-Patent device repository. A UniXNU implementation needs an independent
implementation from public hardware documentation or a separately licensed component.

## CS35L41 requirements

The sheng CS35L43 program is not a suitable starting table. A fuxi CS35L41 owner must implement these states and
checks before it can unmute an amplifier:

1. Keep the amplifier in reset or global mute by default.
2. Enable and validate the required VA/VP supplies before releasing reset.
3. Hold reset for at least the device-specified pulse and boot delay. Reads may NAK during boot.
4. Wait for `OTP_BOOT_DONE` and reject `OTP_BOOT_ERR`.
5. Read device and revision IDs, distinguish CS35L41/CS35L41R, and reject an unexpected ID.
6. Apply the correct revision errata and OTP trim procedure from a license-compatible source.
7. Validate the board's boost inductor, capacitance and current limit before enabling boost.
8. Configure GPIO2 as the board's active-low push-pull interrupt and keep critical faults observable.
9. Configure ASP format, word length and RX slots only from confirmed fuxi stream facts.
10. Select the clock source and wait for a real PLL-lock condition after BCLK exists.
11. Clear stale power-up/power-down completion state before changing `GLOBAL_EN`, then wait for the matching
    completion indication.
12. Treat boost over-voltage, boost short, temperature warning/error, amplifier short and PLL unlock as shutdown or
    recovery events, never as logging-only events.
13. Mute before graph/clock teardown; perform the safe-reset sequence before asserting reset or removing supplies.

The first implementation milestone is therefore an identity/boot/fault audit while the amplifier stays muted. A
chip-ID read by itself is not A3 speaker completion.

## ADSP and AudioReach requirements

The sheng SM8550 path remains the closest UniXNU architecture reference, but board and firmware inputs must remain
outside its generic packet codecs. The mainline review confirms the following separation:

- remoteproc owns firmware authentication, memory regions, ready/fatal/watchdog/stop state and restart generation;
- GLINK owns the SMEM transport and channel lifecycle;
- GPR/APR owns packet validation, destination/source domains, ports, tokens and service binding;
- APM owns graph open/prepare/start/stop/close and shared-memory map/unmap;
- PRM owns LPASS clock requests;
- the hardware endpoint configuration owns sample rate, bit width, TDM slots and clock direction.

For fuxi, `adsp_apps` is an architecture lead, not yet a selected-firmware observation. The firmware source/hash,
carveouts, SMEM/SMP2P identifiers, interrupts, intent behavior, APM version, endpoint IDs, clock IDs and buffer/SMMU
ownership remain unverified in the manifest. None may be filled from sheng merely because both devices use SM8550.

## Implementation order

1. Port and rename only the board-independent SMEM/SMP2P/GLINK/GPR/APM codecs, preserving their host tests.
2. Supply all fuxi remoteproc, firmware and memory values through a board binding with evidence keys.
3. Reach silent shared-memory period completion before touching CS35L41 global enable.
4. Implement the CS35L41 muted identity/boot/fault state machine from license-compatible sources.
5. Add ASP/TDM and speaker power transitions only after slot and clock facts are measured.
6. Add protection firmware/calibration before declaring daily-use audio.

## Evidence boundary

`scripts/check-fuxi-audio-facts.py` proves only that the checked-in source facts are internally consistent and that
runtime-only values remain unclaimed. It does not prove that the devices respond, ADSP starts, a graph completes, a
speaker produces sound, or CoreAudio works.
