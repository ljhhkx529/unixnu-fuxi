# Fuxi audio test results

Canonical test folder: [`tests/fuxi-audio/`](../tests/fuxi-audio/README.md).
Machine-readable database: [`tests/fuxi-audio/results.json`](../tests/fuxi-audio/results.json)
(`docs/fuxi-audio-test-results.json` is a symlink to it).

Other agents should read the JSON, not this table. One object is appended per
booted image. `PASS_READY` means **ADSP ready only**; it is not a speaker PASS.

| ID | Kernel SHA-256 (short) | Boot SHA-256 (short) | Stage / error | Strict SCM | SMP2P 443 | Result |
| --- | --- | --- | --- | --- | --- | --- |
| `R-GPR-08` | `76fad7ba4a15` | `853c5ead61fd` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GPR-07` | `130d821c814e` | `1383b4eb2284` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GPR-06` | `07720336a2e6` | `132948e03b6d` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GPR-05` | `219c47820951` | `ff5bbf091fe1` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GPR-04` | `df6271bb8045` | `6b9eedf32f8c` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GPR-03` | `2039fcfcb78c` | `af1e1b182c63` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GPR-02` | `84f36ecf426f` | `a957274f0f2b` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GPR-01` | `4b108b038a7b` | `10d1aebdd9b6` | `13 / -60` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `BLOCKED_GPR` |
| `R-GLINK-01` | `df1992448f04` | `4d2f1b437069` | `13 / 0` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x6` | `PASS_GLINK` |
| `R-SCM-STRICT-01` | `33e076b8b161` | `a45a4b8d1090` | `11 / 0` | mask `0x3f`, last `0/0` | present, entry idx 2, value `0x2` | `PASS_READY` |
| `R-SCM-BUG-01` | `b3e103da83bb` | `036af01d73ed` | `11 / -6` | not strict; `x2` was not `pas` | absent | `BLOCKED_PRE_SCM_FIX` |





## Incident I-RAW-GPR-01 (2026-11-09)

- Kernel: `37d69eb2433b185599e29cef3bb2030a83ddeba7417101f39621ff8e411f11dd`
- Boot: `2733228d422d334d6a0f90550a21bf796a46097f4002fd423366b0c9763567af`
- Capture: `/private/tmp/fuxi-usb/run-20261109-211133`
- Symptom: CDC USB disconnected about 3 seconds after the collector attached; the
  device then enumerated in Fastboot. No panic line was retained.
- Raw GPR: **not captured**. No `GPRQ`/`GPRR` hex lines were present before the
  disconnect.
- Context: this experimental image increased the probe to 100 queries over 60 s
  and added raw `kprintf`; the reset occurred near the extended GPR window.
- Action: do not reuse this image. Revert to the `R-GPR-02` 50-query/20-second
  timing and verify CDC stability before another raw-capture attempt.





## APM service and status definition (2026-11-09)

From the mainline/Fuxi Linux audio source:

- `GPR_APM_MODULE_IID = 1` (`include/dt-bindings/soc/qcom,gpr.h`). The APM
  service is destination port **1**, not 2/3/7. Port 2 is PRM, port 3 is AMDB,
  port 4 is VCPM in the static GPR table.
- `q6apm` allocates GET_SPF_STATE with `GPR_HDR_SIZE + APM_CMD_HDR_SIZE` and a
  zero 16-byte APM command header.
- `status = 1` is **`ADSP_EFAILED` — general failure**, per
  `sound/soc/qcom/qdsp6/q6dsp-errno.h`; it is not an APM-ready status.
- `q6apm_probe()` calls `q6apm_get_apm_state()` but ignores its return and
  continues. This is evidence that the Linux path tolerates a failed
  GET_SPF_STATE; it does not turn status 1 into ready.



## R-GPR-08: CDC recovery after graph-open incident (2026-11-09)

- Kernel: `76fad7ba4a150814a6acdcb5320aa5c158a70025195853b16486ff461cbe80ef`
- Boot: `853c5ead61fdd2a2f61fa61f05d2bb663672d9df37ecdea5e70c62f2e9ff69dc`
- CDC session: `d259acb8eb88bce7`
- Capture: `/private/tmp/fuxi-usb/run-20261109-231402`
- `kGraphOpenOnly` is **false**. The GRAPH_OPEN code is compiled but cannot
  run; `dspReceive` only intercepts graph replies when that flag is true.
- CDC PING is OK and the control channel stays alive. The stable 24-byte
  GET_SPF_STATE returns the same `ADSP_EFAILED`.
- No graph was opened, no PCM, no amplifier activity.
- QcomAudioGraph host builder passes 67 checks (296-byte packet), so the
  incident is not explained by a malformed host packet alone.

## Incident I-GRAPH-OPEN-01 (2026-11-09)

- Kernel: `934a6f29f8a05fea77c384666a9ba3bea3830d7bd447f340bdaa5aadda4e1433`
- Boot: `0f8806969561c92be2b495064400f8b24f543f32bb133d08cd67d29a6e6efe48`
- Capture: `/private/tmp/fuxi-usb/run-20261109-230825`
- The GRAPH_OPEN-only image booted and answered one DIAGSTATUS
  (page 1 value 7), then CDC disappeared. No graph-open result was read; the
  device later enumerated in Fastboot.
- Action: graph mode is disabled by default. Re-enable only after a host-side
  packet/CDC isolation test and a reviewed bounded device run.


## R-GPR-07: Linux APM source port test (2026-11-09)

- Kernel: `130d821c814ecce4ceaa0eb263f3dba2537d9725f97b7bd23bb3b78b25a21298`
- Boot: `1383b4eb2284791acdb16f89b572d43d4040ecfe6ccf8564fa81c0347af17c95`
- CDC session: `9145b17885781b3f`
- Capture: `/private/tmp/fuxi-usb/run-20261109-224905`
- Linux `q6apm` uses `GPR_APM_MODULE_IID` (1) as the source port for APM
  commands. This run sent GET_SPF_STATE with source port 1 and destination
  port 1, 24-byte header only, 50 queries / 20 s.
- Result is unchanged: `BASIC_RESULT 0x02001005`, result opcode
  `0x01001021`, status `0x1` (`ADSP_EFAILED`). Token matches; 0 SPF_STATE
  answers.
- Regression: CDC PING OK, APFS `disk1`, Metal 13/13, GOP 24/24, no reset
  during capture.
- Conclusion: source port 1 is not the fix. The board binding source port
  0x2001 is restored in the source tree.


## R-GPR-06 (2026-11-09)

- Kernel: `07720336a2e610a435953b6d27641e024fac281c4e97b1a4671e748b027a978c`
- Boot: `132948e03b6dc00df2e4058dff87d9b422b04b7a895a16c936560f747a4c0324`
- CDC session: `43851b4d550ea42d`
- Capture: `/private/tmp/fuxi-usb/run-20261109-223359`
- Recovered stable 24-byte header-only GET_SPF_STATE, 50 queries / 20 s.
- SCM `0x3f`, SMP2P ready, GLINK channel up.
- GPR: `BASIC_RESULT` `0x02001005`, result opcode `0x01001021`,
  status `0x1` (`ADSP_EFAILED`); 50 queries, 0 SPF_STATE answers.
- Regression: CDC PING OK, APFS `disk1`, Metal 16/16, GOP offers 1/1
  (late capture), no panic/SMMU fault.
- No speaker graph or amplifier was started.

## Incident I-APM-HEADER-01 (2026-11-09)

- Kernel: `fc1964211567361324beed6096c0001a0e0001fd6f48d3dd07d2f970f78a73b3`
- Boot: `e85af631aa3ad285558df16a5b1a22e881a20054e70680fe989f913ce10b37f0`
- Capture: `/private/tmp/fuxi-usb/run-20261109-221924`
- The 40-byte q6apm-style request (24-byte GPR + 16-byte APM header) reset to
  Fastboot before a DIAGSTATUS read. No panic line was retained.
- This image is not reused. The 24-byte stable form is restored in `R-GPR-06`.


## R-GPR-04 / R-GPR-05: Fuxi APM destination-port scan (2026-11-09)

- Port 3 (`spf_core` in the DTB): kernel `df6271bb...`, boot `6b9eedf3...`,
  session `0d74e2205982e2e0`. Same `BASIC_RESULT` status 1, token 50,
  result opcode `0x01001021`.
- Port 2 (`audio_gpr` reg in the DTB): kernel `219c4782...`, boot
  `ff5bbf09...`, session `c2d96d046ba7811a`. Same `BASIC_RESULT` status 1,
  token 50, result opcode `0x01001021`.
- Both used the Fuxi 20-intent GPR configuration (`qcom,intents = <0x200 0x14>`).
- Conclusion: the status-1 response is not a port-2/port-3 addressing issue;
  ports 1, 2 and 3 all answer the exact request with the same basic-result
  status. The APM/SPF refusal is deeper than the destination port.
- No speaker graph or amplifier was started.


## R-GPR-03 (2026-11-09)

- Kernel: `2039fcfcb78ced76f37a0bb61f048d066a71390ac4bfae4cb80a462109d7f77c`
- Boot: `af1e1b182c63925533318bfe28432f0004af138276c54799b7254ab465ea2c18`
- CDC session: `82f24440a5bb5bdf`
- Capture: `/private/tmp/fuxi-usb/run-20261109-212301`
- Stable timing: 50 queries / 20 s (the 100-query/60-second image is
  `I-RAW-GPR-01` and is not reused).
- Request bytes (derived from the decoded header and `gpr_encode`):
  `60 18 00 00 02 03 01 20 00 00 01 00 00 00 00 00 32 00 00 00 21 10 00 01`
- Reply bytes (derived from the decoded reply; raw `kprintf` was not retained):
  `60 20 00 00 03 02 01 00 00 00 01 20 00 00 32 00 00 00 05 10 00 02 21 10 00 01 01 00 00 00`
- Decoded: request `0x01001021`, token 50; reply `0x02001005`
  (`BASIC_RESULT`), token 50, payload `{0x01001021, 0x1}`.
- Compare Sheng: a successful reply is `APM_SPF_STATE` opcode `0x02001007`
  with payload status 1. Fuxi instead returns `BASIC_RESULT` status 1 for the
  exact request. The query encoding is not the problem.
- Regression: CDC PING OK, APFS `disk1` active, Metal 66/66, GOP offers 21/21,
  no panic/SMMU fault.
- No speaker graph or amplifier was started.
- Next: map `BASIC_RESULT status 1` to the DSP APM state machine or find the
  correct APM ready transition; do not open the speaker graph.

## R-GPR-02 (2026-11-09)

- Kernel: `84f36ecf426f267559a4d4082b1973b63fb5bbe72703b1b7d0b89a8e03892520`
- Boot: `a957274f0f2bf3c6688adb1a14246998d41b30659b6fc9db934f82ffcb73a9df`
- CDC session: `54f10744dd315618`
- Capture: `/private/tmp/fuxi-usb/run-20261109-205530`
- Request: opcode `0x01001021` (`APM_GET_SPF_STATE`), token 50, source port
  `0x2001`, destination port 1, no payload.
- Response: opcode `0x02001005` (`BASIC_RESULT`), token 50 (match), payload
  `{request_opcode=0x01001021, status=0x1}`.
- Conclusion: the BASIC_RESULT **is** the reply to this exact request; opcode,
  token and result payload all match. It is not a routing/opcode/token
  mismatch. The DSP did not send `APM_SPF_STATE` with status 1.
- Bounded probe: 50 queries, 0 `SPF_STATE` answers, gave up at `last_error=-60`.
- Regression: CDC PING OK, APFS `disk1` active, Metal 67/67, GOP offers 21/21,
  no panic/SMMU fault.
- No speaker graph or amplifier was started.
- Next: interpret the DSP's `BASIC_RESULT status=1` APM response and its state
  transition/wait policy; do not open the speaker graph.

## R-GPR-01 (2026-11-09)

- Kernel: `4b108b038a7b6aa0f4eeafe519225178d5ae36bab4f2ea07a07149d48357b606`
- Boot: `10d1aebdd9b698a805c87207576768a4631afc52e46876b09a99db58b7eaae5c`
- CDC session: `2634a444dbffe9b8`
- Capture: `/private/tmp/fuxi-usb/run-20261109-203341`
- Boot args: `fuxi-adsp=2`; compile-time limit 14, bounded GPR/SPF readiness only.
- SCM `stage_mask=0x3f`; SMP2P 443 ready (`slave-kernel` index 2, value `0x6`).
- GLINK healthy: items 478/479/480, version agreed, `adsp_apps` up,
  RX 226 / TX 108, unknown opens 6.
- GPR/SPF: 50 bounded queries, 0 `APM_SPF_STATE` answers, last opcode
  `0x02001005` (`GPR_OP_BASIC_RESULT`), last status `0x1`; probe gave up with
  `last_error=-60`. This is **not** an SPF ready reply.
- Regression: CDC PING OK, APFS `disk1` active, Metal 91/91, GOP offers 34/34,
  no panic/SMMU fault.
- No speaker graph or amplifier was started.
- Next: inspect APM `GET_SPF_STATE` handling and the DSP's `BASIC_RESULT`
  status 1; do not open the speaker graph.

## R-GLINK-01 (2026-11-09)

- Kernel: `df1992448f047b13ee93b3cf15cbd678146c0a42d962b1cc8f213356590f0488`
- Boot: `4d2f1b43706960a66b4a086c9ab4f49e25f1149aa8a06d9a836703cf08298d1b`
- CDC session: `f31e6eeccd451476`
- Capture: `/private/tmp/fuxi-usb/run-20261109-202204`
- Boot args: `fuxi-adsp=2`; compile-time limit 13, GLINK only.
- SCM: all six stages strict `0/0` (same `stage_mask=0x3f`).
- SMP2P 443: valid header, `slave-kernel` index 2, value `0x6` (ready+handover).
- GLINK: items 478/479/480 present; descriptor 32 bytes; 479/480 are 0x4000-byte
  FIFOs (page low 12 bits zero; channel handshake succeeded). Version agreed,
  `adsp_apps` channel up, RX 10 / TX 8, unknown opens 4, ladder stage 13.
- Regression: CDC PING OK, APFS `disk1` active, launchd early boot complete,
  Metal 73/73, GOP offers 24/24, no panic/SMMU fault.
- Note: item 480 was created by the DSP; 478/479 by the AP. No GPR/SPF query
  and no speaker graph were started.
- Next: limit 14, bounded GPR/SPF readiness query only.

## R-SCM-STRICT-01 (2026-11-09)

- Kernel: `33e076b8b161cb4b4b93e4298ff48ce59a9331c543733aa7f2696e625d6c91ef`
- Boot: `a45a4b8d1090bbef89b810724879526728ef11b0110952a7da520d5c57692551`
- CDC session: `1e1e1b6926a53273`
- Capture: `/private/tmp/fuxi-usb/run-20261109-194509`
- Boot args: `fuxi-adsp=2`; compile-time limit 11, no GLINK.
- Firmware: `modem_b` first, `modem_a` fallback.
- SCM: all six stages passed with strict `x0 == 0 && x1 == 0`:
  DTB `INIT_IMAGE` / `MEM_SETUP` / `AUTH_AND_RESET`, then ADSP
  `INIT_IMAGE` / `MEM_SETUP` / `AUTH_AND_RESET`. Last operation `AUTH_AND_RESET`,
  PAS 1, `x0=0`, `x1=0`.
- SMP2P item 443: header valid (`check_in=0`), `slave-kernel` entry index 2,
  value `0x2` (ready), first seen at 3594 ms, 6 SMP2P IRQs, 4 kicks.
- RSC: forced v3 layout after DRV2 ID read 0; active TCS 2; vote done; lcx
  index 7 / lmx index 4.
- QMP: pre/post `load_state` open, states 4/4, order `0xf`. Mailbox response 0 only means the AOP cleared/took the message; it is not a business success code.
- Regression: CDC PING OK, APFS `disk1` active, launchd early boot complete,
  Metal 72/72, GOP offers 24/24 (late sample; the same image was 14/14 and 0/0 in the early capture), no panic/SMMU fault.
- Next: raise the compile-time limit to 13 and validate GLINK/`adsp_apps`.
  Do not start the speaker graph until that link and GPR/SPF are ready.

## Why the previous round failed

`secureCall()` had the register arguments shifted: it wrote `a/b/c` to
`x2/x3/x4`, so the PAS id was never in `x2`. The secure world returned
positive-looking cookies (`0xcfffe5`, `0xcfffe7`, `0xcfffb9`) and the code
mistook them for success. An invalid PAS id (`0xdead`) returned the same
`0xcfffb9` cookie, proving it was not a success result. The fix is:

```c
x2 = pas;
x3 = a;
x4 = b;
x5 = c;
```

and strict success `x0 == 0 && x1 == 0`.

## How to add a run

Append a new object to `runs` in the JSON with at least: `id`, `when`,
`kernel_sha256`, `boot_sha256`, `cdc_session`, `limit`, `stage`,
`last_error`, `scm`, `smp2p`, `regression`, `result`. Keep the SHA-256
values full length. Do not edit older objects; append a new one.
