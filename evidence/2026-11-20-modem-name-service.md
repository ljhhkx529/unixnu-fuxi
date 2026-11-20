# The modem's name service now answers us

## What was wrong

Two defects in the port's GLINK and QRTR layers, found in order and each proved on the device.

### The first: the modem's open for a channel nobody here serves was never answered

The modem opened a channel named `SSM_RTR_MODEM_APPS`, the edge it uses to ask for a subsystem restart. It is
not a channel this port serves: the component registers only `IPCRTR` (`fuxi_modem.c`, `FUXI_IPCRTR_CHANNEL`).

Upstream answers such an open. Linux `drivers/rpmsg/qcom_glink_native.c` allocates a channel for the name in
`qcom_glink_rx_open` and sends the OPEN_ACK, and `qcom_glink_create_remote` then sends its own OPEN and waits for
the acknowledgement in turn. The port did neither: it counted the open (`g->unknown_opens`) and sent nothing.

The port now allocates the channel and answers it (`package/QcomGlink.c`, `allocate_remote` and the OPEN case in
`receive_one`). The channel carries no intents, because nobody on this side serves it yet.

**This was not the reason the modem said nothing.** See below.

### The second, and the real one: our control packets had no body

Upstream's QRTR name service does not read the header's type field. It reads the command word at the start of the
**body** (Linux `net/qrtr/ns.c`, the `qrtr_ns_worker` switch on `pkt->cmd`), and the body is a whole
`qrtr_ctrl_pkt`: the command word and a 16-byte union (Linux `include/uapi/linux/qrtr.h`).

The port sent its HELLO with **no body at all**. The modem therefore had no command word to read, and upstream's
own receive path refuses such a packet outright (`af_qrtr.c`: `if (!size || size > len ...) goto err`). Our HELLO
was invisible to the modem. That is why the modem announced no service and went quiet.

## How the convention was established

Not by reading alone: the bytes the modem sent were captured on the device. The port's QRTR layer now has an
optional trace hook (`qrtr_set_trace`), and the adapter installs one that dumps the first packets as they arrive.

What the modem sent on 2026-11-20 (`/private/tmp/fuxi-usb/run-20261120-221903/xnu-usb.log`):

```
the qrtr packet is 52 bytes: 01 00 00 00 02 00 00 00 00 00 00 00 fe ff ff ff 00 00 00 00
                             14 00 00 00 ff ff ff ff fe ff ff ff 02 00 00 00 e3 1e 00 00
                             0a 00 00 00 00 00 00 00 00 00 00 00
the qrtr header is version 1 type 2 src node 0 src port 4294967294, confirm 0, size 20, dst node 4294967295 dst port 4294967294
the qrtr body is 20 bytes, its first word is 2
```

So the modem's own HELLO is a 52-byte packet: a 32-byte header saying type 2 and size 20, and a 20-byte body
whose first word is the command. The body's union carries node 7907 and port 10. The modem follows upstream's
convention, and the port did not.

## The change

Every control packet now carries the body. It is built inside `qrtr_send_from`
(`UniXNU/drivers/QcomBaseband/platform/fuxi/qcom_qrtr.c`), so no caller can forget it: a control packet handed
over with no body gets one built with the command word, our own node id and the source port. The reply to the
peer's HELLO carries the same body.

The port's own parsing was already right: it reads the service fields from the body at the offsets upstream's
`qrtr_ctrl_pkt` union puts them (body + 4, + 8, + 12, + 16).

## What the device says now

`/private/tmp/fuxi-usb/run-20261120-223708/xnu-usb.log`:

```
the qrtr packet is 52 bytes: 01 00 00 00 04 00 00 00 00 00 00 00 fe ff ff ff 00 00 00 00
                             14 00 00 00 01 00 00 00 fe ff ff ff 04 00 00 00 42 00 00 00
                             01 b4 00 00 00 00 00 00 01 00 00 00
the qrtr header is version 1 type 4 src node 0 src port 4294967294, confirm 0, size 20, dst node 1 dst port 4294967294
the qrtr body is 20 bytes, its first word is 4
```

Type 4 is `QRTR_TYPE_NEW_SERVER`. The modem is announcing a service: **service 66 (0x42), instance 0xb401,
node 0, port 1**.

And the heartbeat:

```
FUXI_MODEM: 54 s: the link is up, QRTR tx 2 rx 0, servers 2, glink irqs 32
FUXI_MODEM: 54 s: glink rx 44 tx 20 data 3 dropped 0 unknown 1, fifo 0 bytes, channel up 1 remote 1 sent 1 acked 1, our intents 8, their intents 30
FUXI_MODEM: 54 s: qrtr rx 3 bad 0, hello got 1, reply owed 0 to node 0 port 4294967294, their node 0 known 1, hello dropped 0
```

**`servers 2`** — it was 0 for every run before. `qrtr rx 3` (the modem's HELLO, its NEW_SERVER, and one more)
against `qrtr rx 1` before. `glink rx data 3` against 1 before.

The diagnostic pages agree: `glink_up=1`, `stage=9`, `last_error=0`, `smp2p_ready=1`, `smp2p_fatal=0`.

## The host checks

`UniXNU/drivers/QcomBaseband/scripts/check.py` passes. Three checks were changed, all of them pinning the old
belief that a control packet has no body:

- `tests/fuxi_qrtr_model_test.c`, `test_hello` and `test_peer_hello`: the HELLO now goes out with a 20-byte
  body whose first word is the command, and the peer's HELLO is built the way the modem builds it.
- `tests/fuxi_modem_hello_model_test.c`: the HELLO's size field is `QRTR_CTRL_LEN`, not 0.
- `tests/fuxi_modem_integration_test.c`: the same, and its modem HELLO is now the same 52 bytes the device sent.

`unixnu-fuxi/scripts/check-fuxi-glink-open-host.py` is new and passes: it drives the real `package/QcomGlink.c`
over a pair of in-memory FIFOs and pins the whole unknown-open exchange — the acknowledgement carries the
remote's channel id, the port's own OPEN goes out, the channel comes up, nothing is advertised on it, and a
channel that is registered still behaves as before.

## What this does not prove

- **The card 1 SIM has not been read.** No QMI request has gone out to a service yet, so no UIM read has been
  attempted and no signal has been collected. The name service is up; the control plane on top of it is not.
- **The service 66 is not yet identified.** 0x42 is a QMI service id; which one it is has not been looked up
  from the port's own records, and the second of the two announced services has not been dumped at all.
- **The RF is still off.** Blocker B6 stands: the RF stays offline until the control plane is verified.
- **The trace hook prints only the first two packets.** Later packets are counted but not dumped, so the second
  announced service's bytes have not been read off the log.
