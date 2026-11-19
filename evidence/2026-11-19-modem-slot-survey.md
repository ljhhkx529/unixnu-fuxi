# 2026-11-19, the slot survey: both slots carry the modem, and they differ

Two runs, each its own image flashed to the boot partition only (`fastboot flash boot`, serial
b4c62a1e). The first carried `fuxi-adsp=1 fuxi-modem=12` and listed the two slots side by side;
the second carried `fuxi-modem=13` and offered each slot's own blob to the secure world. Every run
used the same build tree and the same board; only the probe's mode and the board argument changed.

## The question

The PAS matrix had shown that the secure world refuses the modem's blob while accepting the ADSP's,
so the refusal is in the blob. Two readings of the board's layout were possible: the modem's
firmware sits in one slot and the other is empty, or the same firmware sits in both. The earlier
record had named `modem_a` without having measured it, because the reader takes the first of
`modem_b` and `modem_a` that carries a FAT volume and never says which one it used.

## The two slots are different partitions

| slot | LUN | first block | blocks |
| --- | --- | --- | --- |
| `modem_a` | 4 | 5126 | 114688 |
| `modem_b` | 4 | 283283 | 114688 |

They are two distinct regions of the same LUN, the same size, in the same order a phone's two slots
are laid out. The GPT name lookup matches the name exactly, so these are two entries, not one
partition seen twice.

## Both slots carry the modem's firmware

| slot | entries in /image | `modem.mdt` | `modem_dtb.mdt` |
| --- | --- | --- | --- |
| `modem_a` | 443 | 6364 bytes | 4044 bytes |
| `modem_b` | 443 | 6364 bytes | 4044 bytes |

The two slots are not one empty and one full: both hold the modem's own header files, with the
same sizes.

## But they are different firmware

The two slots' `modem_dtb` blobs were read out whole and compared byte for byte:

    7992 bytes compared, 95 differ, the first at 4533

The blobs are the same length and the difference starts at offset 4533, which is inside the hash
segment (the header part is 148 bytes and the hash segment is 3896 bytes, so the hash segment runs
from 4096 to 7992). The difference is in the signed part, not in the structure: the two slots hold
two different builds of the modem firmware.

## What this means

The slot is not the reason the secure world refuses the modem's blob: both slots have firmware, and
the refusal was measured against the one the reader picked, which is `modem_b`. What is new is that
the two slots are different builds, so **one of them may be the one the secure world accepts** and
the other the one it refuses. The mode 13 probe offers each slot's own blob to the secure world in
turn, which is the test that follows.

## Each slot's own blob, offered in turn

Mode 13 reads each slot's blob out whole, parses it from those bytes, builds the metadata the same
way the runtime path builds it, and hands it to the secure world with the modem's pas id. Each is
preceded by a shutdown of that pas id, and each gets its own metadata buffer.

| slot | blob | metadata | pas id 38 |
| --- | --- | --- | --- |
| `modem_a` | 7992 bytes, 3 segments, fixed, hash 2 | 4044 bytes at 0x8d0b3f000 | 0xFFCFFFBB refused |
| `modem_b` | 7992 bytes, 3 segments, fixed, hash 2 | 4044 bytes at 0x8d0b60000 | 0xFFCFFFBB refused |

Both are refused with the same code. So **neither build is accepted**, and the slot question is
closed for good: the refusal belongs to something the two builds share.

What the two builds share is the part the secure world authenticates: the metadata's structure, the
header's program headers, the segment's address (0x9b000000, the board's own
`q6_mpss_dtb_region`), and the fact that the image is **fixed** rather than relocatable. The ADSP's
blob, which is accepted, is **relocatable**. That difference is now the first thing left to check,
and it is a fact about the board's reserved-memory layout rather than about the modem's code.

## Safety

Nothing was reset and no processor was released: the survey only mounts the two partitions read
only and lists a directory, and the mode 13 calls are INIT_IMAGE behind a PAS_SHUTDOWN. No RF was
touched and no partition other than `boot` was written.
