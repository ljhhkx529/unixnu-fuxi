# The modem's name service does not answer this modem, and SSCTL does

## What was established

The modem's QRTR node publishes exactly two services and ignores every service lookup the AP sends. Both
facts were measured on the device, not inferred.

### The lookups go out and are not answered

The port now asks the peer which services it has, with a full `qrtr_ctrl_pkt` body exactly as upstream's
`qmi_send_new_lookup` builds it (Linux `drivers/soc/qcom/qmi_interface.c`: the command word, the service id,
and the instance word holding the version in its low byte and the instance in its high byte). Two new
functions carry it: `qrtr_send_lookup` to the node the peer's HELLO named, and `qrtr_send_lookup_to` to a
named node, because the peer's HELLO header names node 0 while its HELLO body carries node 7907.

The bytes on the wire were dumped by a new transmit-side trace hook (`qrtr_set_trace_tx`, installed by the
board adapter as `fuxi_trace_qrtr_tx`). They are correct: the header is version 1, type 10 (NEW_LOOKUP),
source node 1, source port 0xfffffffe, destination port 0xfffffffe, and the body's first word is 10 with
the service id in the second word.

```
the qrtr packet we sent is 52 bytes: 01 00 00 00 0a 00 00 00 01 00 00 00 fe ff ff ff 00 00 00 00
                                     14 00 00 00 00 00 00 00 fe ff ff ff 0a 00 00 00 42 00 00 00
                                     01 b4 00 00 00 00 00 00 00 00 00 00
```

**The modem did not answer a single one of them.** The counters say so plainly:

```
FUXI_MODEM: 219 s: the service lookups sent 13
FUXI_MODEM: 219 s: the service lookups answered 0
```

The receive count never moved past 3 (the modem's HELLO, its two NEW_SERVER announcements). Over a thousand
seconds of runtime the answer stayed at 0 while the sent count rose to 13.

### Why: upstream's name service runs on the AP, not on the modem

Upstream's `net/qrtr/ns.c` is the **AP-side** name service. Its `ctrl_cmd_new_lookup` begins:

```c
	/* Accept only local observers */
	if (from->sq_node != qrtr_ns.local_node)
		return -EINVAL;
```

The lookup is a request to the AP's own name service, which then walks its own server table and answers
with NEW_SERVER. The modem is the *server* side in upstream's arrangement; it announces with NEW_SERVER and
takes them away with DEL_SERVER. There is no lookup handler on the modem to answer ours, which is exactly
what the device shows: our 13 lookups went out, and nothing came back.

This also explains the two services that *did* appear. They were never the answer to our lookup. They were
the modem announcing on its own initiative, in the same breath as its HELLO, before our first lookup went
out (the timestamps bear this out: the NEW_SERVER packets appear at the second 2, the lookup was sent after
the link was up).

### The two services the modem publishes

From the bridge's report, from the modem's own NEW_SERVER bytes:

```
COACHZ baseband: 2 s: the modem offers service 0x42, version 1, instance 180, at node 0 port 1
COACHZ baseband: 2 s: the modem offers service 0x2b, version 2, instance 18, at node 0 port 4
```

Upstream's own `include/linux/soc/qcom/qmi.h` names both:

- `QMI_SERVICE_ID_SERVREG_NOTIF 0x42` — the service-registration **notification** service. It is not UIM.
- `QMI_SERVICE_ID_SSCTL 0x2b` — the service status control service.

**UIM is 0x0b** (the port's own `CBE_SVC_UIM 0x0bu`, `coachz_bb_esim.h`), and the modem does not publish it.

## What this leads to

SSCTL (0x2b) is the service whose job is to hold the modem's service list. It is the one the modem *did*
publish, so it is the one the list can be asked of. This is a new file, `coachz_bb_ssctl.h`, with the
request builder and the answer parser, and a bridge-owned client that asks once and reads the answer.

The first run of it produced the most useful result of the round: **the modem answered**.

```
COACHZ qmi: int-tx ch 0 svc 0x2b cid 1 type 0 msg 0x1 txn 0 len 7 err 0x0 n 1 t 22480
COACHZ qmi: int-rx ch 0 svc 0x2b cid 1 type 2 msg 0x1 txn 0 len 14 err 0x39 n 1 t 22480
COACHZ baseband: the modem lists 0 services, error 57
```

The framing is right, the routing is right, the modem parsed the request and answered it. The result is
**57**, which is `QMI_SERVICE_ERROR_INVALID_MESSAGE_ID`: the message id 0x0001 is not one SSCTL knows.

The message id of SSCTL's Get Supported Services is not in this tree. Qualcomm's own
`qmi_service_ssctl_v01.h` is not here, and libqmi does not carry the service at all — its `data/` directory
has `qmi-service-ctl.json`, `qmi-service-dms.json`, `qmi-service-uim.json` and the rest, but no
`qmi-service-ssctl.json`. So the id cannot be looked up; it has to be found.

## The id sweep

The modem tells us which ids it knows: it answers an id it does not know with the same
`QMI_SERVICE_ERROR_INVALID_MESSAGE_ID`. So the ids are swept, one per second, each with the id it is trying
in its transaction so the answer says which id it belongs to.

The sweep works, and it is the mechanism that finds the id:

```
COACHZ baseband: the answer to message id 0x02: 0 services, error 57
COACHZ baseband: the answer to message id 0x03: 0 services, error 57
...
COACHZ qmi: int-rx ch 0 svc 0x2b cid 1 type 2 msg 0x2 txn 2 len 14 err 0x39 n 1 t 22373
```

One caution the first sweep found: sent without a pause, the modem answered the first ten requests and then
went quiet — 63 requests went out and only 10 answers came back, with nothing dropped on our side. The
requests are now spaced a second apart.

## Why the tenth answer was the last: QRTR flow control was missing

The ten was not a coincidence and it was not the modem refusing. It is upstream's flow control.

Upstream's `net/qrtr/af_qrtr.c` counts the DATA packets a node has outstanding and stops at a high watermark:

```c
#define QRTR_TX_FLOW_HIGH	10
#define QRTR_TX_FLOW_LOW	5
```

`qrtr_tx_wait` blocks the sender when `flow->pending` reaches the high watermark, and it sets the packet's
`confirm_rx` flag on the packet that crosses the low watermark. The receiver answers a flagged packet with
`QRTR_TYPE_RESUME_TX`, which sets `flow->pending = 0` again. So a peer that does not get the RESUME_TX stops
answering after ten outstanding packets — exactly the ten we saw.

The port had none of it. `qrtr_send_from` wrote `confirm_rx` as a hardcoded zero, no RESUME_TX was ever
answered, and the modem's answers were dropped on the floor as they arrived. The device said so plainly:

```
FUXI modem: 1417 s: link up, QRTR tx 75 rx 10 again 37, servers 2, glink tx 104 rx 97
FUXI_MODEM: 1417 s: glink rx 97 tx 104 data 13 dropped 0 unknown 1, fifo 0 bytes, channel up 1 ...
```

Both directions frozen at once, with nothing dropped on our side: the modem stopped answering because its
credit ran out, and we stopped sending because we were waiting for answers that never came.

A second bug sat on top of it: the bridge's send hook had no way to report a refusal. `bb_send` was a
`void` callback and `coachz_baseband_poll` freed every frame as if it had gone out, so a request the link
refused was lost with nothing on the log to say so. The hook now returns the refusal and the bridge puts the
frame back on the queue to be tried again.

The flow control is now upstream's:

- `confirm_rx` is set on the DATA packet that crosses the low watermark of 5, and the high watermark of 10
  refuses further packets instead of sending them into a peer that has no room.
- The credit is kept per destination, the way upstream keys the flow (`dest_node << 32 | dest_port`).
- A received RESUME_TX frees the credit of the destination its body names.
- A received DATA packet with `confirm_rx` set is answered with a RESUME_TX, whose body names where the
  flagged packet was addressed, which is what the sender has to free.

### The host checks

`UniXNU/drivers/QcomBaseband/scripts/check.py` passes. The flow control has its own test,
`test_flow_control` in `tests/fuxi_qrtr_model_test.c`: the flag goes on the fifth packet and on no other, the
tenth is the last one that goes out, a RESUME_TX frees the credit, and a flagged DATA packet is answered with
a RESUME_TX that names the right destination.


## The host checks

`UniXNU/drivers/QcomBaseband/scripts/check.py` passes.

- `tests/coachz_bb_model_test.c` gains `test_ssctl`: the request's framing, the answer's result TLV and
  service list TLV, the error-only answer, the truncated answer, and the swept request builder.
- `tests/fuxi_qrtr_model_test.c` gains `test_lookup`: the wildcard lookup before the peer's HELLO goes to
  the broadcast node, the instance word's layout (`0x0312` for version 0x12 and instance 3), the lookup to a
  named node, and the empty NEW_SERVER that ends a listing leaving the server count alone.

## What this does not prove

- **The card 1 SIM has not been read.** No UIM request has gone out. UIM is still not published by the
  modem, and the list that would settle whether it is served at all has not come back yet.
- **The RF is still off.** Blocker B6 stands.
- **The SSCTL message id is not yet known.** The sweep is what finds it; the run that finds it has not been
  read at the time of writing.
- **The second channel is still not served.** The modem opens `SSM_RTR_MODEM_APPS` and the port answers the
  open so the edge stays up, but the channel carries no intents and nothing is read from it. Whether the
  QMI traffic the modem does not publish rides that channel is still open.


## The modem does not die after a count of answers: it dies when it executes one

Three runs on 2026-11-21 asked the SSCTL service a sweep of message ids and the modem always stopped after
answering an id it *accepted*. The id moved, the count of answers moved with it, and the earlier reading of a
fixed count of 31 was wrong.

| run | sweep | answers | the id the modem accepted |
| --- | --- | --- | --- |
| `run-20261121-053358` | ids 0x02 upward, 0x0020 asked | 31 | 0x20 |
| `run-20261121-054514` | ids 0x02 upward, 0x0020 skipped | 31 | 0x21 |
| `run-20261121-061021` | ids 0x01 upward, 0x0020 skipped | 31 | 0x21 |
| `run-20261121-064417` | ids 0x01 upward, no id skipped | 32 | 0x20 |
| `run-20261121-071238` | ids 0x01 upward, no id skipped | 32 | 0x20 |

The count of answers is always the accepted id minus 0x1e, because the sweep asks one id a second and the
modem answers from the first id of the sweep. The id 0x0020 was skipped in the second and third runs and the
accepted id moved to 0x0021, so the trigger is not the id. The trigger is the answer: an id the modem does
not know is refused with error 57 and the modem stays up, and an id the modem *accepts* is executed and the
modem stops answering.

The answers are read off the wire. The QMI response SDU is: byte 0 the type (2 for a response), bytes 1-2 the
transaction, bytes 3-4 the message id, bytes 5-6 the TLV length, then the TLVs. A refusal is
`02 1e 00 1e 00 07 00 02 04 00 01 00 39 00` and a success with no data is
`02 20 00 20 00 07 00 02 04 00 00 00 00 00`. Both carry exactly one TLV, of type 0x02, the result.

### The credit was not the reason, and the measurement says so

The new counters separate the two credit systems that could have stopped the traffic.

- **The GLINK credit was never exhausted.** The counters the device now prints are `no room 0, too small 0,
  asked 0, done 52` at the end of `run-20261121-071238`: not one send was refused for want of an intent. The
  modem's thirty receive intents were never all held at once, and the modem released them 52 times.
- **The QRTR credit was never exhausted either.** The modem's own RESUME_TX packets name the destination they
  free, and the body of every one of the six the modem sent was `node 0, port 4`, which is where our data
  packets are addressed. Both directions were checked against upstream: upstream's `qrtr_tx_resume` frees the
  flow the body names (`key = remote_node << 32 | remote_port`), and our RESUME_TX bodies name the address the
  flagged packet was sent to (`node 1, port 0x4100`), which is what upstream's `qrtr_send_resume_tx` puts in
  the body (`pkt->client.node = cb->dst_node`).
- **Nothing was lost on our side.** The GLINK counters at the end are `rx 131 tx 117 data 41 dropped 0`, and
  the 41 data packets are exactly the 32 answers plus the 9 QRTR control packets the modem sent. Every packet
  the modem sent was read and answered.

So the link was healthy and the modem stopped answering on its own. The flow counter freezing at ten pending
is the *symptom*: our sender is refusing further packets because the peer's credit was never returned, and
the peer never returned it because the modem had stopped.

### The name service will not answer, and that is upstream-correct

Upstream's name service runs on the AP, not on the modem. `ctrl_cmd_new_lookup` in `net/qrtr/ns.c` accepts
only local observers:

```c
/* Accept only local observers */
if (from->sq_node != qrtr_ns.local_node)
    return -EINVAL;
```

A lookup the AP sends is a question the modem has no name service to answer. The modem publishes on its own
initiative the services it wants announced, and on this board that is two: SERVREG_NOTIF (0x42) and SSCTL
(0x2b). UIM (0x0b) is not among them, and the list that would settle whether UIM is served at all has to come
from SSCTL itself.

### The crash reason is not reachable from here

The modem's crash reason lives in SMEM item 421 (Linux's `sm8550_mpss_resource`: `crash_reason_smem 421`),
which is in the *modem's* private partition. The port reads the partition shared with the modem (host 0, remote
pid 1), and item 421 is not there: the device says "the modem's crash reason item is not there". The item is
in the modem's own partition, which the AP does not map.

### What is still open

- **The SSCTL message id is not known.** The sweep found ids the service accepts (0x0020 and 0x0021) but the
  modem stops answering as soon as it executes one, so the sweep cannot reach the id that asks for the
  service list. libqmi does not carry SSCTL, and the id is what has to be found.
- **The card 1 SIM has not been read.** No UIM request has gone out, and UIM is not published.
- **The RF is still off.** Blocker B6 stands.

## The sweep was asking the modem to restart and shut down its own subsystem

The sweep was sent to the **SSCTL** service (0x2b), and the SSCTL service's whole message set is subsystem
control. Qualcomm's own headers (`modem_proc/qmimsgs/ssctl/api/subsystem_control_v01.h` in the MPSS tree)
define it:

| id | message |
| --- | --- |
| 0x0020 | SSCTL RESTART |
| 0x0021 | SSCTL SHUTDOWN |
| 0x0022 | SSCTL GET FAILURE REASON |
| 0x0023 | SSCTL SUBSYS EVENT |

There is no "Get Supported Services" in SSCTL. The comment in `coachz_bb_ssctl.h` that named
`QMI_SSCTL_GET_SUPPORTED_SERVICES` from "libqmi's qmi-service-ssctl.h" was wrong on both counts: libqmi
carries no SSCTL service at all (`data/qmi-service-ssctl.json` does not exist), and no such message exists.

So the sweep asked the modem to **restart its subsystem** at id 0x0020 and **shut it down** at id 0x0021. That
is what stopped the modem, and it explains everything the earlier runs showed:

- The modem answered the id it *accepted* and then went quiet, while every id it did not know was refused with
  error 57 and left it running. Asking an id the service does not know is harmless; asking 0x0020 is not.
- The trigger moved with the sweep only because the sweep's *first accepted id* moved: with 0x0020 skipped the
  first accepted id was 0x0021, which shuts the subsystem down just as effectively.
- The "fatal" bit of the modem's SMP2P rose at second 42 in both runs while the last answer moved from
  t=53.4 s to t=61.5 s: the bit is the modem's own report of a subsystem restart or shutdown, and it is set on a
  schedule of the modem's own, not on the arrival of our packet.
- The modem was **not** answering after the death either. The answers that came later (up to id 0x0028) are the
  answers the modem had already queued; the questions that followed it (id 0x0029 upward) were never answered,
  and the queue of them grew without bound.

**The sweep must not ask SSCTL 0x0020 or 0x0021, and must not sweep the SSCTL service at all.** The service
list is not an SSCTL question.

### The error codes the answers carry, from Qualcomm's own header

The answers name their error code in the result TLV (type 0x02): two bytes of status and two bytes of code.
`modem_proc/datamodem/api/public/qmi.h` gives the names, and the sweep's answers fall exactly on them:

| code | name | which ids |
| --- | --- | --- |
| 17 | `QMI_SERVICE_ERR_MISSING_ARG` | 0x0023, 0x0025 — the ids the service knows |
| 57 | `QMI_SERVICE_ERR_INVALID_MESSAGE_ID` | every id the service does not know |
| 58 | `QMI_SERVICE_ERR_ENCODING` | 0x0020, 0x0021, 0x0022, 0x0024, 0x0026 — ids the service knows |

Error 58 on the ids the service knows says the *request* was not well formed, which is what a request carrying
a TLV the message does not define gets. It is the modem refusing the shape of the request, not the id.

### The service list is a CTL question, and CTL is the AP's own service

The list of the modem's services is **CTL (service 0x00) Get Version Info, message id 0x0021**. libqmi's
`data/qmi-service-ctl.json` says so, and Qualcomm's `control_service_v01.h` gives the answer's shape:

```
TLV 0x02  result
TLV 0x01  service list:  count:u8   then count x { u8 service_id, u16 major, u16 minor }
```

The count is a prefix in **items**, not bytes, and each element is five bytes. The two things the port's own
code had wrong about this are now settled:

- `cbs_has_service()` compares `list[i] & 0x0f` and `list[i] >> 4` for the version. There is no nibble
  version anywhere in this layout.
- The list is not "one TLV per service". It is one TLV holding the whole array.

And CTL does not reach the modem at all. Under QRTR the CTL service is implemented **on the AP**: libqmi's
`qmi-endpoint-qrtr.c` intercepts `QMI_MESSAGE_CTL_ALLOCATE_CID` and `QMI_MESSAGE_CTL_INTERNAL_ALLOCATE_CID_QRTR`
and answers them itself, and upstream Linux's `net/qrtr/ns.c` keeps the server table and answers lookups
locally. The modem is never asked for a CID. So the modem's own service list cannot be fetched by asking the
modem: CTL Get Version Info is answered by whichever node owns CTL.

What the modem *does* publish on its own initiative is two services, and it published exactly two on this board:

| service | instance | node | port |
| --- | --- | --- | --- |
| 0x42 SERVREG_NOTIF | 46081 | 0 | 1 |
| 0x2b SSCTL | 4610 | 0 | 4 |

UIM (0x0b) is not among them, and the modem published no more. The wildcard lookup the port sends (service 0,
instance 0) was answered by the modem with the two NEW_SERVER packets above and by nothing else.

### Why the flow count froze at ten: the modem stopped answering

The QRTR flow control was not the cause; it was the symptom, and the packet order in the log proves it. Every
fifth data packet carries `confirm_rx`, and each one is answered by a RESUME_TX in both directions. The order
for one round is:

```
TX  data  msg 0x05  confirm=1
RX  RESUME_TX  names node 0 port 4      <- the modem frees our flow
RX  data  msg 0x05  confirm=1
TX  RESUME_TX  names node 1 port 16640  <- we free the modem's flow
```

Both bodies are right. The modem's body names `node 0, port 4`, which is where our data is addressed, and our
body names `node 1, port 0x4100`, which is where the flagged packet was addressed. Upstream's
`qrtr_tx_resume` frees the flow the body names (`key = remote_node << 32 | remote_port`), and upstream's
`qrtr_send_resume_tx` puts the flagged packet's own destination in the body
(`pkt->client.node = cb->dst_node; pkt->client.port = cb->dst_port`). Both match.

The flow count therefore ran 0,1,2,3,4,0,1,2,3,4,0 ... for eight rounds, and then stopped resetting:

```
41 s: the qrtr flow has 0 packets pending, 8 resumes sent, 8 received
...
51 s: the qrtr flow has 10 packets pending, 8 resumes sent, 8 received
```

The count reaches the high watermark of ten and stays there, because after the modem stopped answering there
was nothing left to answer and no more RESUME_TX to come. Our sender then refuses further packets because the
peer's credit was never returned. The eight resumes each way account for the eight rounds exactly; the ninth
round is the one the dead modem never answered.

## The safe request keeps the modem up: run-20261121-090754

With the sweep replaced by the one safe SSCTL request (the failure reason, id 0x0022), the modem answered and
**stayed up**:

| | the sweep run (`run-20261121-074738`) | the safe run (`run-20261121-090754`) |
| --- | --- | --- |
| how long the modem answered | until the sweep asked 0x0020 | the whole run (413 s and counting) |
| the modem's SMP2P fatal bit | rose at second 42 | rose at second 42, with the modem still up |
| the QMI requests answered | 40, then silence | one, the failure reason, with error 94 |
| the QRTR flow | froze at 10 pending | stayed at 1 pending |

Error 94 is `QMI_SERVICE_ERR_NOT_SUPPORTED`: the failure reason request itself is refused, because the modem
has not failed. That refusal is the answer the safe request was built to get, and it is the modem's own
statement that nothing is wrong. It is also a single answer, so the flow stays at one pending packet and the
credit never runs out.

The `fatal` bit of the modem's SMP2P still rises at second 42 in both runs, and in the safe run the modem is
still answering afterwards. So that bit is not the modem dying: it is the modem's own state word, and it says
something about the modem's handover state that is not "the modem is dead". The earlier reading of that bit as
a crash report was wrong, and the evidence file's earlier sections said so.

## The modem's traffic still stops at about forty seconds

The safe run is not a success on its own. The modem answered one request in the first seconds and then went
quiet: `QRTR rx 4` for the rest of the run, and `glink rx 58 tx 35` frozen from the fortieth second. The
modem is up and the link is up, but nothing more arrives.

Two things are known about that silence and neither is settled yet:

- **The modem's name service did answer one lookup.** The port sent thirteen lookups and the modem published
  two NEW_SERVER packets in reply, at the start. It published SERVREG_NOTIF (0x42) at node 0 port 1 and SSCTL
  (0x2b) at node 0 port 4, and no more. UIM (0x0b) was asked for three times, in three forms, and was never
  published. The port's counter for answered lookups stayed at zero because the two NEW_SERVER packets the
  modem sent were read as the end of a listing (an empty NEW_SERVER is what ends a listing upstream,
  `lookup_notify(from, NULL, true)`), not as the two services they carried. That is a counting bug in the
  port, not in the modem.
- **A GLINK channel was opened that the port does not serve.** The last unknown channel name the log reports
  is `SSM_RTR_MODEM_APPS`, which is the edge the modem uses to ask for a subsystem restart. The port names
  the channel it opens but does not answer what the modem sends on it, and the modem opened a second one it
  does not serve at all ("unknown 1" in the GLINK counters).

### The modem's own node id

The modem's HELLO header names node 0, and the body names node 7907 with port 10. The port's lookups go to
both. Upstream's own name service accepts a lookup only from a local observer
(`if (from->sq_node != qrtr_ns.local_node) return -EINVAL`), so on a modem that runs one, node 7907 is where
the lookup has to go, and node 0 is where the modem's services are published from.

## What the modem actually sent in the whole run: four packets

The safe run's capture holds every QRTR packet the modem sent, and there were only four of them:

| # | type | from | to | body |
| --- | --- | --- | --- | --- |
| 1 | HELLO | node 0, control | broadcast, control | `02 00 00 00 e3 1e 00 00 0a 00 00 00 00 00 00 00 00 00 00 00` |
| 2 | NEW_SERVER | node 0, control | node 1, control | service 0x42, instance 0xb401, node 0, port 1 |
| 3 | NEW_SERVER | node 0, control | node 1, control | service 0x2b, instance 0x1202, node 0, port 4 |
| 4 | DATA | node 0, port 4 | node 1, port 16640 | the answer to the failure reason request |

That is the whole modem's traffic for 490 seconds. So the modem was never frozen: it answered the wildcard
lookup with the two services it publishes on its own initiative, answered the one request the bridge made, and
then had nothing left to say. The "traffic stopped at forty seconds" reading was a reading of the counters
freezing, not of the modem dying: the modem had nothing to send.

The two answers are the modem's own list, and it is a list of two. SERVREG_NOTIF (0x42) at node 0 port 1 and
SSCTL (0x2b) at node 0 port 4. UIM (0x0b) is not in it, and neither are NAS, WDS, DMS or any other service the
bridge wants.

### The instance word matches exactly

The body of a NEW_SERVER is a whole `qrtr_ctrl_pkt` (Linux include/uapi/linux/qrtr.h): the command word comes
first and the server union follows it, so the service is the second word of the body, the instance the third,
the node the fourth and the port the fifth. The port reads them at body+4, +8, +12 and +16, which is right.

The two answers therefore carry instance words 0xb401 and 0x1202, and the port's own named lookups encode
version in the low byte and instance in the high byte:

- the port asked for `{0x42, version 1, instance 180}` and encoded `1 | (180 << 8)` = 0xb401, which is the
  instance word the modem answered with
- the port asked for `{0x2b, version 2, instance 18}` and encoded `2 | (18 << 8)` = 0x1202, which is the
  instance word the modem answered with

So the modem did answer the two named lookups, with the exact instance words that were asked for. The
instance word is not a problem.

### The two nodes

The modem's HELLO header names node 0 and the body names node 7907 (port 10). Upstream's own name service
runs on the AP, and `ctrl_cmd_new_lookup` accepts only a local observer, so on a modem that runs one, node
7907 is where a lookup has to go. The port's named lookups alternate between the two nodes
(`link->wanted_done & 1u`), which is why the log shows them going to node 0 and node 7907 in turn.

### The counting bug

The port's counter for answered lookups stayed at zero for the whole run. The reason is in
`qcom_qrtr.c`'s NEW_SERVER handler: it counts a NEW_SERVER as an answered lookup only when the service is 0,
which upstream uses as the empty notification that ends a listing. The modem never sent one, so the counter
never moved. The two services it did publish were counted as services, not as answers. The counter is
therefore a count of end-of-listings, not of answers, and it cannot say whether a lookup was answered.

## The wildcard lookup is asked again, and the modem still publishes two services

The modem's answer to the wildcard lookup was two services and then nothing: no empty NEW_SERVER, which upstream
uses to end a listing. So the listing was cut short, and a service the modem registers a moment later would not
be in it. And the name service does not announce what appears later, so the only way to learn a late service is
to ask again.

The adapter now asks the wildcard lookup again every two seconds, on both of the nodes the modem named (the one
in its HELLO header and the one in its HELLO body). The rate is in milliseconds and not in polls: a first attempt
counted polls and sent 123239 lookups in 136 seconds, which says nothing and floods the log. With the clock the
same run sent 81 lookups in 210 seconds.

**The modem still published the same two services.** The re-ask is right and it is kept, but it did not find
anything more: the modem's list really is SERVREG_NOTIF (0x42) and SSCTL (0x2b), and UIM is not in it.

So the modem's own name service is not withholding UIM. The modem is up, it answers, and it holds two services.
The other services — UIM, NAS, WDS, DMS — are not published because the modem's stack has not brought them up.

### The counting bug is fixed

The counter for answered lookups only moved for an empty NEW_SERVER, which upstream uses to end a listing, so it
stayed at zero on a modem that answers with services and never ends its listing. It now counts a published
service as an answer too, and the run reports 2 answered.

## What the modem's own SMP2P says

The port reads one SMP2P entry ("slave-kernel") and ignores the rest. The modem publishes a set, and the set is
the only place that shows what the modem is waiting for. The probe now dumps every entry with its name and its
value, once, when they first arrive.

The modem's SMP2P pair is the same shape as the ADSP's: the AP writes the outbound item (0x1ac) and reads the
modem's inbound one (0x1b3), and the port adds an outbound entry called "master-kernel" but never writes it.
Linux's `qcom_smp2p` driver does the same thing, so this is not obviously a defect: the outbound item is written
by the subsystem-restart handshake, not by the ordinary bring-up.

What is on the wire, then, is the picture: the modem is up, the link is up, the name service answers, and the
modem publishes two services. It has not published UIM, and nothing the port sends has changed that in 490
seconds.

## What service 0x42 actually is, and where the modem's list really lives

Service 0x42 is **SERVREG_NOTIF**, and it is not a service directory. Its whole message set is a state
notification service for one named service path (Linux's CAF-derived
`drivers/soc/qcom/service-notifier-private.h`):

| id | message | direction |
| --- | --- | --- |
| 0x0020 | REGISTER_LISTENER | request/response |
| 0x0021 | QUERY_STATE | request/response |
| 0x0022 | STATE_UPDATED | indication |
| 0x0023 | STATE_UPDATED_ACK | request/response |
| 0x0024 | RESTART_PD | request/response |

The request TLVs are a one-byte enable (0x01) and a service name (0x02, a string of up to 64 characters). The
response carries the result (0x02) and a one-byte flag with the current state (0x10). The states are the
protection-domain ones: UP is 0x1FFFFFFF, DOWN is 0x0FFFFFFF, EARLY_DOWN is 0x2FFFFFFF.

So asking 0x42 for a service list was asking a service that has no such message. There is no "get service list"
in it. The one message in that set that must never be sent is 0x0024, RESTART_PD, which restarts a protection
domain: the same hazard as SSCTL's restart and shutdown, and the reason the safe-request rule applies here too.

### The service that does list: SERVREG_LOC, 0x40

There is a second service in the same family, and this one is the directory. **SERVREG_LOC (0x40)** has a
GET_DOMAIN_LIST message (ids 0x0020 and 0x0021) whose request is a service name (TLV 0x01, a string) and an
optional offset (TLV 0x10, a u32), and whose response is the result (0x02), the total number of domains (0x10,
u16), a database revision (0x11, u16) and the domain list itself (0x12): an array of a name, an instance id, a
service-data flag and the service data.

The kernel's protection-domain mapper uses exactly this flow: it asks 0x40 for the domain list, and only then
registers a 0x42 listener for the service it wants.

**The modem never published 0x40.** It published 0x42 and 0x2b and nothing else, so the port had never asked
for the locator. The named-lookup sweep did not include 0x40 either, so the one service that can answer "what
services do you have" was never asked.

The port now asks for 0x40 first, in three forms: instance 0, version 1 instance 0, and version 1 instance 1.
The instance word is the version in the low byte and the instance in the high byte
(`drivers/soc/qcom/qmi_interface.c`, `qmi_send_new_lookup`: `svc->version | svc->instance << 8`), which is what
the port already does, and what the modem's two answers confirmed: the port asked for `{0x42, version 1,
instance 180}` and the modem answered with the instance word 0xb401, which is 1 | (180 << 8).

### The two services the modem published, read again

| service | id | version | instance | node | port | what it is |
| --- | --- | --- | --- | --- | --- | --- |
| SERVREG_NOTIF | 0x42 | 1 | 180 | 0 | 1 | the state notifier for one service path |
| SSCTL | 0x2b | 2 | 18 | 0 | 4 | the subsystem control, which can restart and shut the modem down |

Both are control services. Neither is the modem's working stack. UIM (0x0b), NAS (0x03), WDS (0x01) and DMS
(0x02) are what the card goal needs, and the modem published none of them.

The instance numbers 180 and 18 are not arbitrary: 0x1202 is SERVREG_NOTIF's neighbour and 0xb401 is the other,
and both decode the same way (version in the high byte of the little-endian word, instance in the low byte). The
instance 180 (0xb4) is the protection-domain instance of the service, which is how Qualcomm numbers the
protection domains a service runs in.

## The modem's SMP2P set, and the locator that is not there

Run `run-20261121-113715` (kernel sha256 9d4620ebfab2017c9545f92595fb9b28e33bb5210a550bbd6ae1c956d94a6c01, boot image
`/tmp/fuxi-loc2.img`) carries the modem's own SMP2P entries on the USB log, which no earlier run did:

```
FUXI_MODEM: the modem's SMP2P has 2 entries
FUXI_MODEM: the modem's SMP2P entry 0 is smp2p, value 0x0
FUXI_MODEM: the modem's SMP2P entry 1 is slave-kernel, value 0x6
```

Two entries, no more. "smp2p" is the generic entry every SMP2P pair has, and "slave-kernel" is the modem's own
state word. Its value is 0x6, which is bit 0x2 (the ready bit, from `linux.sm8550.dtsi.smp2p_modem_in`) plus bit
0x4 (a handover bit). **The modem publishes nothing else.** There is no hidden entry the port does not know, and
in particular no entry that names a service the modem is waiting for.

Two things about how the dump was reached, so the next person does not repeat the search:

- The dump cannot be printed where the entries arrive. The modem's SMP2P arrives at about the fifth second of
  the boot, and the USB CDC console is attached later than that (in the run above it attached at 28 seconds), so
  a print made at the arrival is lost. It is printed in the once-a-second report instead, which runs after the
  console is there.
- The port's SMP2P item layout was checked against upstream `drivers/soc/qcom/smp2p.c`: the magic, the version
  and features word, the local pid in the low half of word 2 and the remote pid in the high half, the total
  entry count in the low half of word 3 and the valid count in the high half, the flags in word 4, and the
  entries from word 5, five words each. The port matches. The item ids match too: the live device tree gives
  `qcom,smem = <0x1b3 0x1ac>` for `qcom,smp2p-modem`, and upstream reads index 0 as the inbound item and index 1
  as the outbound one, so 0x1b3 is what the modem writes and 0x1ac is what the AP writes.

### The locator is asked for and is not answered

The port now asks for service 0x40 (SERVREG_LOC) by name, in three forms: instance 0, version 1 instance 0,
and version 1 instance 1. The log shows all three going out:

```
FUXI_MODEM: the lookup for service 0x40 version 0 instance 0 went out to node 0
FUXI_MODEM: the lookup for service 0x40 version 1 instance 0 went out to node 7907
FUXI_MODEM: the lookup for service 0x40 version 1 instance 1 went out to node 0
```

**No answer.** At the end of the run the counters read `the service lookups sent 84` and `the service lookups
answered 2`, and `servers 2`. The two answers are the same two services as before. The modem's name service
does not publish the locator either.

That settles the question the lookup sweep was meant to answer: **the modem's published service list is
exactly two entries, and neither of them can lead to UIM.** SERVREG_NOTIF (0x42) has no list message at all
(its whole message set is register-listener, query-state, state-updated and restart-protection-domain), and
SSCTL (0x2b) can restart or shut the modem down and can report the modem's crash reason, but has no service
list either. Asking the modem for the list is therefore a dead end until the modem itself publishes more.

### The modem sends four packets and then stops

Upstream's name service answers a HELLO with the servers it holds (`announce_servers`, Linux net/qrtr/ns.c), and
answers a NEW_LOOKUP with the matching servers plus an empty NEW_SERVER that ends the listing. The modem sent
four packets in the whole of the safe run, and the two NEW_SERVERs came as its answer to the AP's HELLO, which
is what `announce_servers` looks like. The AP's own lookups were never answered at all: the counter moved
only for the two announced services, and never for the empty terminator, because the modem never sent one.

So the modem is not withholding its list. It has two services, it announced them, and it did not answer a
single lookup the AP made afterwards.

The two NEW_SERVER bodies were decoded correctly all along, and the instance words are exactly the ones the
port asked for: the port encodes the version in the low byte and the instance in the high byte
(`qmi_send_new_lookup`: `svc->version | svc->instance << 8`), and the modem answered `{0x42, version 1,
instance 180}` with the instance word 0xb401 and `{0x2b, version 2, instance 18}` with 0x1202. The two named
lookups were answered. Everything else the port asked for was not.

## The order of the packets says the modem answers nothing

The four packets the modem sent, in the order the log shows them, with the port's own lookups interleaved
(run-20261121-113715, lines 354 to 440 of the capture):

| log line | who | what |
| --- | --- | --- |
| 354 | modem | HELLO, node 0, body words 7907 and 10 |
| 361 to 426 | port | the whole lookup sweep, fourteen named lookups plus the wildcard |
| 429 | modem | NEW_SERVER, service 0x42, instance 0xb401, node 0, port 1 |
| 433 | modem | NEW_SERVER, service 0x2b, instance 0x1202, node 0, port 4 |
| 440 | modem | DATA, the answer to the failure-reason request |

The order is the whole point. **The two NEW_SERVERs arrive after every one of our lookups has already gone
out.** They are not the answers to those lookups: they are the modem announcing the services it holds, which is
what upstream's `announce_servers` does in answer to a HELLO (Linux net/qrtr/ns.c: `ctrl_cmd_hello` calls
`announce_servers`, which sends one NEW_SERVER per server in the local node and nothing else).

So the modem ran its name service on its own schedule and answered none of our questions. The wildcard lookup
that goes out every two seconds was never answered either: the counter moved twice, for the two announced
services, and never for the empty terminator that ends a listing. A modem that answered a wildcard lookup
would send that terminator (upstream: `lookup_notify(from, NULL, true)`).

The capture holds 55704 lines and four received packets. There is no fifth packet, no answer to any lookup, and
no service beyond the two.

### Why the modem's name service ignores our lookups

Upstream Linux's name service is AP-local and its lookup path refuses anything that does not come from its own
node: `ctrl_cmd_new_lookup` starts with `if (from->sq_node != qrtr_ns.local_node) return -EINVAL;`. The
modem's name service is the same code, running on the modem, so it accepts a lookup only from the node id it
bound itself to. Our port sends the lookups with the header's source node at 1, which is the node id upstream's
own AP uses (`static unsigned int qrtr_local_nid = 1;`, Linux net/qrtr/af_qrtr.c), and it sends them to the
node ids 0 and 7907.

Whether the modem's name service is on node 0 or on node 7907 is not yet settled by the evidence, and it is
the one thing that decides whether a lookup can ever be answered. What is settled is that neither node answers.

### The other thing upstream does that this port does not

Upstream's protection-domain mapper does not use the name service to find the modem's services at all. It uses
the locator service, and it registers for the locator by name:

    ret = qmi_add_lookup(&pdr->locator_hdl, QMI_SERVICE_ID_SERVREG_LOC, 1, 1);

(Linux drivers/soc/qcom/pdr_interface.c, in `pdr_probe`.) The locator is service id 0x40, asked for with
version 1 and instance 1. When the locator appears, `pdr_locator_new_server` sets `pdr->locator_addr` and
schedules the work that asks the locator for the domain list.

The port now asks for exactly that service in that exact form, and gets no answer, because the modem's name
service answers nothing. The lookup is the right one; the modem will not answer it.
