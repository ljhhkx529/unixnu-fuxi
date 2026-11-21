# Baseband bring-up: the three paths, the order, and what cost time

This file is the map. It is not the record of a round and it is not a status report —
the record is `porting.toml`, the exclusions are `EXCLUDED.md`, and the measurements are `evidence/`.

**Every claim here names its source.** If a line has no source, treat it as an opinion.

---

## 1. Three different basebands — do not mix them up

There are three bring-up paths across the two reference projects. They share vocabulary (PAS, GLINK,
QRTR, QMI) and share almost nothing else. Reading the wrong one is the single most expensive mistake
made on this port so far.

| | hakios `hakios.mpss.c` | hakios `umi_modem*.c` | mainline fuxi |
|---|---|---|---|
| Board | POCO X5 Pro (redwood, SM7325) | Xiaomi Mi 10 (umi, SM8250) | Xiaomi 13 (fuxi, SM8550) |
| The modem is | an MPSS inside the SoC | **a separate SDX55 on PCIe** | an MPSS inside the SoC |
| Transport | PAS + GLINK + QRTR | **PCIe + MHI + Sahara** | PAS + GLINK + QRTR |
| Data path | **IPA** | IP_HW0 over MHI channels 100/101 | **IPA** |
| Services seen | 88 announced | bridged to iOS CommCenter | all of them, then LTE registered |
| Use it for fuxi? | partly | **no** | yes, this is the model |

**The trap**: "UMI announced 88 services without an IPA, so the IPA is not needed." That is the SDX55
path, whose data plane is MHI. An integrated MPSS on SM8550 has no data plane without the IPA.

Sources: `hakios.mpss.c:1-2` (its own first line names redwood), `docs/cellular.md`,
`/tmp/x13/docs/交接-新会话-20261030-2110.md:26` (the IPA is the data path on fuxi).

---

## 2. The order

What the AP side must have ready, in this order. Steps 2 and 3 are done on fuxi; step 6 is where it stops.

```
0  firmware      modem.mdt / modem_dtb.mdt / .bNN / ipa_fws.mdt, read-only from the board partition
1  memory        mpss_region, q6_mpss_dtb_region, mpss_dsm_region, rmtfs_mem (2.5 MiB)
2  PAS           INIT_IMAGE -> (MEM_SETUP if relocatable) -> segments -> AUTH_AND_RESET     [done]
3  GLINK         IPCRTR channel, HELLO in both directions                                     [done]
4  QRTR name     routing, NEW_SERVER / DEL_SERVER, lookup
5  announce      the four AP-side services: 14 RMTFS / 4096 TFTP / 64 locator / 0x31 IPA
6  EFS           RMTFS service 14 over the four partitions  -> the modem announces its stack  [STOPS HERE]
7  carrier cfg   TFTP service 4096 over modem_pr            -> the modem leaves OFFLINE
8  data plane    IPA: firmware, clock, SMMU, GSI, INIT_MODEM_DRIVER with TLV 0x23
9  userspace     SIM, registration, data, SMS
```

### The two sides, aligned

| Stage | hakios (integer MPSS) | mainline fuxi |
|---|---|---|
| before PAS | `rmtfs_load`, `rmtfs_buffer_make`, `rmtfs_share` | `rmtfs` (file mode) + `tqftpserv` running |
| PAS | `step_header` / `step_image` / `step_start` | `qcom_q6v5_pas`, DTB first then the image |
| link | `step_queues` / `step_link` / `step_qrtr` | `qcom_glink_smem` + `net/qrtr` |
| services | `our_servers[4]` announced at HELLO | `qcom_pd_mapper` + userspace `rmtfs`/`tqftpserv` |

Upstream declares the same four AP-side services (`hakios.mpss.c:3995-4018`):

| service | instance word | what |
|---|---|---|
| 14 | 1 | RMTFS |
| 4096 | 1 | TFTP for the carrier configuration |
| 64 | 257 | the servreg locator (version 1, instance 1) |
| 0x31 (49) | 0x101 | the IPA host service |

Mainline reaches the same four through three separate pieces of software:
`fuxi-firmware-devices` -> `tqftpserv` -> `rmtfs -s` -> ModemManager
(`/tmp/x13/docs/目标-M5.md:76`).

---

## 3. What cost time — measured, not guessed

### 3.1 Memory and ownership

| Lesson | Source |
|---|---|
| The PAS metadata must land **below 4 GiB**. Above it `dma_alloc_coherent` returns NULL and PAS answers `-12` | `/tmp/x13/docs/交接-新会话-20261030-2110.md:70`, fixed with `CONFIG_ZONE_DMA` + `ZONE_DMA32` |
| An existing PAS call takes the **whole carveout**, not the image size | `hakios.mpss.c:1509` |
| The RMTFS buffer goes to **three** vmids — HLOS, MSS_MSA and NAV — not one | `hakios.mpss.c:3688-3697`, `VMID_NAV` is `0x2b` |
| SMEM item 497 belongs to the modem. An AP preallocation trips the modem’s assert | `/tmp/x13/docs/交接…:71`, fixed by mapping the whole SMEM window 1:1 into the IPA domain |
| `modem-tables` must be at the vendor IMEM address **0x146a8000**, not mainline’s 0x14688000 | `/tmp/x13/docs/交接…:72` |

### 3.2 Files served to the modem

| Lesson | Source |
|---|---|
| Without the four file systems the modem offers **"no services but its own"** | `hakios.mpss.c:1251-1252`, the reference’s own log string |
| Only after it is served `modem_pr` does the modem agree to go ONLINE | `docs/ru/cellular.md`, the section dated 23 October 2026 |
| `rmtfs` must run in **file mode** (`-s`). Ubuntu’s default `rmtfs -r -P -s` writes the real partitions, so the drop-in has to be in place first | `/tmp/x13/docs/交接…:121`, `/tmp/x13/docs/蜂窝调研.md:50` |
| `tqftpserv` maps `/readonly/firmware/image/` onto the remoteproc directory and sends `/readwrite/` to `/var/lib/tqftpserv` | `/tmp/x13/docs/蜂窝调研.md:52` |
| Write requests touch the **memory copy only**. Never a partition | both sides |

### 3.3 The data plane

| Lesson | Source |
|---|---|
| `INIT_MODEM_DRIVER` needs **TLV 0x23**; `0x24`/`0x25` must not be filled on fuxi | `docs/fuxi-baseband-plan.md` D2 |
| Without the IPA the modem crashes in `enl2_dl` a few seconds after ONLINE | `docs/ru/cellular.md` |
| The GSI channel-halt timeout is **5000 ms**, and it comes after the SSR shutdown | `docs/fuxi-baseband-plan.md` D2 |
| The IPA endpoints are `AP_MODEM_RX` 24 and `MODEM_AP_RX` 22, not v4.2’s 23 and 21 | `docs/fuxi-baseband-plan.md` D2 |

### 3.4 Method — the part that transfers to any port

| Lesson | Source |
|---|---|
| Keep four states apart: source done / compiles / stable on the device / physically accepted. Never skip a level | `/tmp/x13/docs/交接…:119` |
| The modem **cannot be retried inside one boot** (the rproc power reference never returns to zero). Every experiment is a reboot | `/tmp/x13/docs/交接…:74` |
| Widen one dangerous action at a time — never PAS, GLINK, QRTR and transmission in the same round | `docs/fuxi-baseband-plan.md` §3 |
| Watch the packets the modem sends, not the counters. "The counter froze" is a symptom, not a cause | `evidence/2026-11-21-modem-service-lookup.md:227` |
| Suspect your own code before the hardware — the `contiguous` recompute at `IOLib.cpp:758` was ours | this port, 2026-11-20 |
| Write the prohibitions down, not only the goals | `EXCLUDED.md` group C |

### 3.5 Two refutations worth remembering

| Refutation | What it means |
|---|---|
| Mainline at `modem4`: **rmtfs running**, and the modem still offered only `RFS(14) / LSM(64) / IPA control(49) / TFTP(4096)` — no DMS, NAS, WDS | RMTFS is **not sufficient**. Do not conclude "RMTFS was useless" if it fails; conclude "not sufficient here" |
| hakios: with the locator and TFTP both served, the sequence was still **88 services first, then ONLINE** — two gates, not one | Do not merge the two gates. This port did, and ran the wrong step first |

Sources: `/tmp/x13/docs/目标-M5.md:169-170` and `:509-512`; `/tmp/x13/docs/交接…:26,78`;
`evidence/2026-11-21-modem-servreg-locator.md`.

---

## 4. Where to look, by question

| The question | Read |
|---|---|
| PAS, secure world, metadata | `EXCLUDED.md` group A first — all of it is closed |
| The link, credit, flow control | `EXCLUDED.md` group B |
| The RMTFS wire format | `hakios.mpss.c:3765-3925` |
| The TFTP wire format | `umi_modem_mhi.inc:684-840` |
| The locator and the domain table | `hakios.mpss.c:4492-4684` |
| The IPA handshake | `/tmp/x13/docs/交接-新会话-20261030-2110.md` §3 and §4 |
| What has been measured | `evidence/`, one file per run |
| What is excluded, and where this file has been wrong | `EXCLUDED.md`, including §F |

---

## 5. How a round is run

```
1  read EXCLUDED.md and this file
2  one card: hypothesis / one criterion / one variable / prohibitions
3  read the reference implementation — local first; go upstream only if it is genuinely absent
4  change one thing -> build -> image -> flash boot only -> capture >= 300 s
5  quote only this run’s log, with its run id and kernel sha256
6  evidence -> porting.toml finding -> record checker -> commit
7  update EXCLUDED.md: this line became an exclusion, or became the next task
```

### Three anti-patterns

| Anti-pattern | Why it costs | Do instead |
|---|---|---|
| Pasting the analysis into the chat | The context is compressed and the line numbers are lost, so it re-derives | Write it to a file, reference the filename |
| Saying "as we discussed earlier" | There is no "earlier" — only the window | Cite the file and line |
| Several goals in one round | The result cannot be attributed | One card, one criterion |

---

## 6. State as of 2026-11-22

| | |
|---|---|
| Done | PAS (stage 9, 31 segments), GLINK link, QRTR, the service-registry table read off the device, the locator published and asked for |
| Excluded | everything in `EXCLUDED.md` groups A and B, including the locator |
| Blocking | the modem publishes only `0x42 SERVREG_NOTIF` and `0x2b SSCTL`; UIM, NAS, WDS and DMS are not published |
| Next | RMTFS (service 14) — criterion: does the modem send `OPEN /boot/modem_fs1` |
| Not started | TFTP, IPA, userspace |
