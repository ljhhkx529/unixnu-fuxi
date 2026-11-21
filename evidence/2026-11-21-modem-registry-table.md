# The modem's own service registry table, read off the device

## What this round was for

The service registry locator (QRTR service 64, SERVREG_LOC) is the AP's: the modem's protection domains
register their services with a registry whose locator lives on the application processor. Asked for a service
by name, the locator answers with the domains that provide it and their QMI instance.

The table the locator answers out of is the modem's own, and it sits beside the firmware as a `.jsn` file.
Before anything is served on service 64, the table has to be read off the device: a table written from a
guess is a table that answers the modem wrongly.

This round read it. It changed nothing else.

## The step 0 inventory, and why the table is real

The earlier read-only inventory (`evidence/2026-11-19-modem-firmware-inventory.log`, the `fuxi-modem=1`
probe) already listed the firmware partition's `/image` directory: 448 entries, and among them the registry
tables `modemr.jsn` (710 bytes), `adspr.jsn` (513), `adsps.jsn` (515), `adspua.jsn` (703), `battmgr.jsn`
(516) and `cdspr.jsn` (513). `/image/modem_pr` holds the directories `mcfg` and `so`.

So the real domain table is on the device. The synthetic-table branch does not apply, and nothing here is
synthetic.

## The reader this round added

The probe had no plain-file reader. `fat_open` and `fat_read` live only inside `package/FuxiAdspFirmware.c`,
and the public firmware API reaches only the split modem images (`modem:NAME`) and VFS paths, so a file that
is one file could not be read.

One entry point was added, `fuxi_adsp_fw_read_file(path, size, out)` in `package/FuxiAdspFirmware.c`: it
mounts the firmware partition (the same one the listing uses, modem_b before modem_a), opens the named path
in the FAT volume and copies the file out whole, read only. The probe's new mode 14
(`package/FuxiModemProbe.cpp`, `dumpRegistryTable`) calls it and prints the bytes as hex and as ASCII.

The mode is read only: it reads the partition and prints. It starts no modem, votes no rail, maps no
window and writes no register. The capture confirms it — the run contains no `FUXI_MODEM: step:` line at all,
whereas every bring-up mode prints one.

## The run

| | |
| --- | --- |
| run | `run-20261121-155640` |
| capture | `/private/tmp/fuxi-usb/run-20261121-155640/xnu-usb.log` |
| kernel sha256 | `7251ddb0fb7a35f093775a0083136a8632643b1e181c72c32fef06a1c453eba6` |
| boot image | `/tmp/fuxi-jsn-dump.img`, sha256 `768386b4f178c7f42e27d4f3da1a47e21792b16abd350ca02a59b199e5a17bbc` |
| flashed | `fastboot -s b4c62a1e flash boot` only, then `reboot`. No other partition was written. |
| boot argument | `fuxi-modem=14` |

The dump is at lines 129 to 175 of the capture, and the whole file was recovered from it byte for byte: the
710 bytes of the hex column reassemble to the 710 bytes the ASCII column shows, and the two agree. The
recovered file's sha256 is `07138bd0e249fa43db4eeb1a4516037db3bfe4bb88285e8c36f594be2b2a4b43`.

## What the table says

```
{
    "sr_version": {
        "major": 1,
        "minor": 1,
        "patch": 1
    },
    "sr_domain": {
        "soc": "msm",
        "domain": "modem",
        "subdomain": "root_pd",
        "qmi_instance_id": 180
    },
    "sr_service": [
        {
            "provider": "tms",
            "service": "servreg",
            "service_data_valid": 0,
            "service_data": 0,
            "dcomment": "data can be used to configure ACK timeout.Make data_valid 1 as well.Unit is second"
        },
        {
            "provider": "gps",
            "service": "gps_service",
            "service_data_valid": 0,
            "service_data": 0,
            "dcomment": "No comment"
        }
    ]
}
```

## What it means

**One domain, two services, one instance.** The table names a single protection domain — `msm/modem/root_pd`
— with `qmi_instance_id` 180, and inside it two services: `tms/servreg` and `gps/gps_service`. Both carry
`service_data_valid` 0 and `service_data` 0, so neither carries service data: the "data" fields are the
optional extra a provider can publish, and neither of these two does.

**The reference implementation reads exactly this shape.** `hakios.mpss.c`'s `servreg_load` (lines 4492-4600
of `/tmp/hakios-extract/hakios-xnu-kernel-umi-fixes-main/drivers/modem/hakios.mpss.c`) reads the same seven
files (`modemr.jsn`, `adspr.jsn`, `adsps.jsn`, `adspua.jsn`, `battmgr.jsn`, `cdspr.jsn`, `wpssr.jsn`), pulls
`soc`, `domain`, `subdomain` and `qmi_instance_id` out of `sr_domain`, and then for each `provider`/`service`
pair in `sr_service` builds one entry whose name is `provider/service`, whose domain is
`soc/domain/subdomain` and whose instance is `qmi_instance_id`. On this table that yields:

```
tms/servreg      at msm/modem/root_pd, instance 180
gps/gps_service  at msm/modem/root_pd, instance 180
```

**The instance is 180, and that is the fact this round settles.** The reference publishes the locator itself
as instance 257 (`SERVREG_INSTANCE 257`), but that is the *locator's* instance, not the instance of the
services it maps. The services of this table are instance 180. The task's own suggested synthetic fallback
("msm/modem/root_pd, instance 180") happened to be right, but it is now read off the device rather than
assumed, and the two services are named.

**`tms/servreg` is the modem asking for itself.** The provider `tms` is the modem's own time-and-services
manager, and the service it registers is `servreg` — the service registry itself. So the modem's protection
domains register with a registry, and the registry's own service is one of the registered entries. That is
consistent with the modem's two published QRTR services being `SERVREG_NOTIF` (0x42) and `SSCTL` (0x2b):
0x42 is the registry's notification service, so the modem holds the registry and asks the AP to hold its
locator.

**`gps/gps_service` is the second entry, and it is the reason the table matters.** A locator that answers
only about `tms/servreg` would be incomplete: the modem asks for services by name, and `gps/gps_service` is
a name it can ask for. The other six `.jsn` files (`adspr`, `adsps`, `adspua`, `battmgr`, `cdspr`) carry
their own domains, so the full table is the union of seven files and not this one alone. The other six have
not been read yet; this round read one file, as it was asked to.

**`service_data_valid` is 0 for both, so the answer carries no service data.** The reference's answer writer
emits a zero validity byte and four zero bytes of service data for every entry, which matches this table.

## What is not established

- The other six registry tables (`adspr.jsn`, `adsps.jsn`, `adspua.jsn`, `battmgr.jsn`, `cdspr.jsn`,
  `wpssr.jsn` — the last one was not in the 2026-11-19 listing) have not been read. The full table is their
  union.
- The modem has not yet been asked anything. This round read a file; it published no locator, sent no
  NEW_SERVER and answered no request. Whether the modem asks for the locator at all is still open, and so is
  whether it asks for it by the name `tms/servreg`.
- The answer's TLV layout was not settled here. The reference builds it from Qualcomm's service registry
  header: TLV 0x02 carries the result, 0x10 the total domain count, 0x11 the table's revision and 0x12 the
  list, each entry being a length-prefixed domain string, a 4-byte instance and a validity byte with 4 bytes
  of service data. The *request*'s TLV 0x01 is the wanted service name, not the domain list; that is the
  reference's reading of the header and it is the thing to check against Qualcomm's own header before the
  answer is written.

## The porting record

`porting.toml`, the `network` area: the finding that the table is real and what it contains, the correction of
the lookup figure (below), and the evidence of this run.

### A correction to an earlier figure

The finding written on 2026-11-21 said the port sent "3042 wildcard lookups plus fourteen named ones" over
3110 seconds. **That is wrong, and it is wrong in a way that would mislead the next reader.** `3042` is a
timestamp, not a counter: in `run-20261121-113715` the line at that second reads
`FUXI_MODEM: 3042 s: the link is up, QRTR tx 87 rx 1, ...`. The counters in that run read
`the service lookups sent 84` and `the service lookups answered 2`, frozen at 84 from t=69 s to the end of
the capture at t=15351 s. The number of lookups that went out is 84, not 3042, and it stopped at 84 because
the link stopped accepting them, which is a different fact and is the one the earlier evidence file already
records correctly.

The evidence file `evidence/2026-11-21-modem-service-lookup.md` states this correctly (line 613: "the
counters read `the service lookups sent 84` and `the service lookups answered 2`"). Only the finding in
`porting.toml` carried the wrong number, and it has been corrected to quote this run's log.
