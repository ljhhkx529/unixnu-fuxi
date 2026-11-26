# 2026-11-26: USIM READY and the operator read from the card

**Result: the card's USIM application reached state 7 (ready) and CommCenter read EF_ICCID and EF_IMSI. The modem
requested the CMCC carrier configuration by itself and CommCenter pushed the carrier bundle; the modem reported
LTE registration roaming in the middle of the run.**

## Root cause of the five-day dead end

The bridge's first Change Provisioning Session (0x38) was malformed in two ways:

1. TLV 0x01 "Session Change" is `{ u8 session_type, u8 activate }` (libqmi data/qmi-service-uim.json, since 1.24).
   The first attempt sent `{ u8 type, u16 session_id }` with length 3 and activate 0, which is a deactivate
   request with a trailing byte. The modem answered result 1, error 1.
2. TLV 0x10 "Application Information" is `{ u8 slot, u8 aid_len, u8 aid[] }`. The AID must be the USIM
   application's own AID from the Card Status response (TLV 0x10, application identifier value, 16 bytes:
   `a0000000871002ff86ffff89ffffffff`). The first attempt passed the 38-byte blob from TLVs 0x14/0x17, which is
   not the AID.

With `{session_type 0 (primary GW), activate 1}` and the 16-byte AID the modem accepted the request (result 0,
error 0) and the next Get Card Status reported:

    card_status UMi: slots=2 card_state=1 inserted=1 apps=1 app1_state=7
    app ready (7) seen, polls 3
    TLV 0x10 indexes: gw_primary 0x0000 (was 0xffff)

CommCenter then sent Read Transparent (0x20) for EF_ICCID (0x2fe2) and EF_IMSI (0x6f07), the modem answered with
result 0, and the bridge's activation indication carried both:

    0xb00c indication len 51: ... 4f 01 00 02 50 01 00 02 56 01 00 01 57 01 00 01
                              42 0a 00 89 86 00 69 19 26 f0 04 68 91            (ICCID)
                              43 08 00 46 00 00 46 19 34 62 9f                 (IMSI)

## Operator

- The modem asked TFTP for `image/modem_pr/mcfg/configs/mcfg_sw/generic/China/CMCC/Commercial/Volte_OpenMkt/mcfg_sw.mbn`
  — the CMCC carrier configuration, at 89,296 bytes.
- CommCenter pushed the carrier bundle over BSP: 132 0xa000 messages, carrier file types 9 (14 segments) and 8
  (43 segments), each acknowledged, with 0xa001 FileTransferStatus after the last segment.
- NAS Serving System indications reported registration state 2 (registered, roaming) with CS and PS attached
  (cs=2, ps=2), then registration state 0 later in the same boot.

The card itself is a China Mobile SIM; the operator is read from the card and matched to the CMCC bundle.

## Evidence runs

| run | image | log |
|---|---|---|
| corrected 0x38, first success | /tmp/fuxi-sess2.img | /tmp/fuxi-sess2cap/xnu-usb.log |
| same boot, later | — | /tmp/fuxi-opcap/xnu-usb.log |
| EFS content check (read-only) | /tmp/fuxi-efs.img | /tmp/fuxi-efscap/xnu-usb.log |
| card power cycle only | /tmp/fuxi-pwr2.img | /tmp/fuxi-pwr2cap/xnu-usb.log |

## EFS content (read-only diagnostic)

    modemst1 first 16: 10 00 00 00 03 00 00 00 d8 01 00 00 18 b0 9e c2, nonzero 2610842 of 8388608
    modemst2 first 16: 11 00 00 00 03 00 00 00 d8 01 00 00 00 02 88 8a, nonzero 2610807 of 8388608
    fsc      first 16: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00, nonzero 0 of 1048576
    fsg      first 16: 14 00 00 00 03 00 00 00 d8 01 00 00 00 00 00 00, nonzero 2610574 of 8388608

The factory data is present; fsc empty is its normal state on this board.

## Remaining

- Registration state was 2 (roaming) in the middle of the run and 0 later; the status bar/operator name was not
  observed directly (no UI capture). A long observation with the SIM in a live cell is the next check.
- The bridge's transmit queue dropped DMS messages under load (`COACHZ txdrop: queue full svc 0x2 msg 0x24`),
  worth bounding before the next long run.
