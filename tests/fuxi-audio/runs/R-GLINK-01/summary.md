# R-GLINK-01

- Result: `PASS_GLINK`
- When: 2026-11-09T20:22:04+08:00
- Kernel: `df1992448f047b13ee93b3cf15cbd678146c0a42d962b1cc8f213356590f0488`
- Boot: `4d2f1b43706960a66b4a086c9ab4f49e25f1149aa8a06d9a836703cf08298d1b`
- CDC session: `f31e6eeccd451476`
- Stage / last_error: `13 / 0`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x6`, ready `True`
- Next: advance the compile-time limit to 14 and send the bounded GPR/SPF readiness query; do not open the speaker graph

GLINK only. Item 480 was created by the DSP; 478/479 were allocated by the AP. No GPR/SPF query and no speaker graph were started.
