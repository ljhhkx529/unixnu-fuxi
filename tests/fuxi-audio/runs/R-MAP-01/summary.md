# R-MAP-01

- Result: `BLOCKED`
- When: 2026-11-10T01:28:00+08:00
- Kernel: `f58171b6d63dac4c71856555ea4e4684dfa90b764e8f4745e2acc96b660e2b5c`
- Boot: `97e1418b1a92a0a0f4b561c89ea88f3bf44ea5743c712feb082324b453435317`
- CDC session: `52d9ce5e21fea923`
- Stage / last_error: `13 / -34`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x6`, ready `True`
- Next: Reserve or allocate an owned low-DRAM PCM region; verify AP and DSP access before periods and amplifier enable.

DIAGSTATUS page 15 reason 26 means pa+4096 > 4 GiB. Graph control path closed; CDC PING survived. No PCM period, no amplifier operation, no sound. SCM/SMP2P details are carried from R-GRAPH-04, not remeasured individually.
