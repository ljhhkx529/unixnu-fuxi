# R-SCM-BUG-01

- Result: `BLOCKED_PRE_SCM_FIX`
- When: 2026-11-09T19:34:01+08:00
- Kernel: `b3e103da83bb50d191d8c744c6062063be818d44dcdea051a08df5c3bac99ce3`
- Boot: `036af01d73ed024a8d1b12f857a70bab2dc2e441c3ffddcf037b7b7d1576058d`
- CDC session: `9569b2330663151a`
- Stage / last_error: `11 / -6`
- SCM mask: `0x0`, last `0xcfffe5/0x0`
- SMP2P 443: present `False`, index `-1`, value `0x0`, ready `False`
- Next: None

Confirmed with an invalid PAS id 0xdead returning the same 0xfb9 cookie as valid ids: the cookie was not evidence. The next round fixed x2=pas and restored strict 0/0.
