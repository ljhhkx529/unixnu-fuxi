# 2026-11-19, the PAS matrix: the refusal is in the modem's blob

The image was flashed to the boot partition only (`fastboot flash boot`, serial b4c62a1e).
The kernel is `449d61e08d156e78842cbcd3f34bcffa45401427759a048b0eb9ce423cd4ed29`, the boot image
is the one built from it, and the kernel arguments carry `fuxi-adsp=1 fuxi-modem=11`.
Every run in this record used the same build tree and the same board; only the probe's mode and
the board argument changed.

## The question

The runtime path reached stage 3 (rails-voted) and then the secure world refused the modem device
tree blob's metadata with `0xFFCFFFBB`. The call's shape was the same as the ADSP's proven one, so
the difference was either the pas id or the image. The matrix was built to tell those apart.

## The matrix

Mode 5 makes the same `INIT_IMAGE` call four ways: the modem's blob and the ADSP's blob, each
against the modem's pas id (38) and the ADSP's (36). All four use the same command, the same
argument descriptor (two arguments, the second read-write), a 4044 byte metadata buffer and
neighbouring physical addresses.

| image | pas id 38 | pas id 36 |
| --- | --- | --- |
| `modem_dtb` | 0xFFCFFFBB refused | 0xFFCFFFBB refused |
| `adsp_dtb` | 0xFFCFFFBB refused | **0/0 accepted** |

The `adsp_dtb` blob is accepted by the same code that refuses the other three. So the command,
the argument descriptor, the argument order, the buffer's size and alignment, the metadata's
assembly and the physical address's shape are all right. The refusal follows the image.

## The instrument, checked before its answers were used

A tool that always says yes would have made the matrix meaningless, so the support query was asked
about ids that cannot work (mode 6):

| query | answer |
| --- | --- |
| support query, pas id 36 | 0/0 |
| support query, pas id 1 | 0/0 |
| support query, pas id 0xffff | 0xFFCFFFC0 |
| support query, pas id 0x1234 | 0xFFCFFFC0 |
| INIT_IMAGE, the modem's blob, pas id 0xffff | 0xFFCFFFE5 |
| INIT_IMAGE, the modem's blob, pas id 38 | 0xFFCFFFBB |

The query does discriminate. The secure world gives at least three different codes: an id it does
not know (0xFFCFFFE5), an id it knows but will not accept (0xFFCFFFC0), and an image it refuses
(0xFFCFFFBB).

## Every accepted id refuses the blob

Mode 9 asks the support query over the whole range and then hands the modem's blob to each id it
accepted. The secure world accepts 21 ids:

    1 4 9 12 13 14 15 18 19 20 21 23 26 27 28 33 34 35 36 37 38

Every one of them refuses the modem's blob with the same 0xFFCFFFBB. No id takes this blob, so
the problem is the blob.

## The two remaining paths, both tried

| what was tried | result |
| --- | --- |
| the blob's segment written into its own hole (0x9b000000) before INIT_IMAGE (mode 7) | still 0xFFCFFFBB |
| the metadata in the packed form, the headers with the hash segment right behind them (mode 11) | still 0xFFCFFFBB |

**Correction (2026-11-19, later run).** The mode 11 run also printed "the packed and the split forms
are byte for byte the same". That comparison was written with two identical branches
(`memcmp(...) ? "the same" : "the same"`) and both buffers were built from the same read, so it
could not fail and proved nothing. The claim above is withdrawn: the two forms were **not** shown
to be the same bytes. Mode 11 now counts the differing bytes and reads the packed form out of the
`.mdt` file, so the next run measures it.

## Both blobs are well formed

The modem's blob declares 452 + 60 = 512 bytes and the ADSP's declares 8316 + 60 = 8376; both are
self consistent, and both start with `43 46 47 4c` (CFGL). The modem's blob is **fixed** (its
segment names 0x9b000000) and the ADSP's is **relocatable**. Their hash segments are both 3896
bytes and 193 of those bytes differ, which is what two different firmwares should look like.

The modem's main image (149013672 bytes, 32 segments, pas id 4) is refused with the same code.

## What this means

The port's PAS call is proven correct: the ADSP's blob is accepted by the same code path. The
modem's blob and the modem's main image are refused by the secure world on this board. The next
thing to look at is where the modem's firmware comes from: both images are read out of the
`modem_a` partition, and `mountPartition()` in `package/FuxiAdspFirmware.c` tries `modem_b` first.
Which slot this phone's baseband actually runs from, and who signed the firmware in that slot,
has not been checked on the device.

## Safety

Nothing was reset and no processor was released: every call in this record is `INIT_IMAGE`,
`MEM_SETUP` or the support query, and the only register write of the session was the rails vote in
the earlier runtime run. No RF was touched and no partition other than `boot` was written.
