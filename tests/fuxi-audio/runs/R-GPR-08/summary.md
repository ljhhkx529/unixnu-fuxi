# R-GPR-08

- Result: `BLOCKED_GPR`
- When: 2026-11-09T23:14:02+08:00
- Kernel: `76fad7ba4a150814a6acdcb5320aa5c158a70025195853b16486ff461cbe80ef`
- Boot: `853c5ead61fdd2a2f61fa61f05d2bb663672d9df37ecdea5e70c62f2e9ff69dc`
- CDC session: `d259acb8eb88bce7`
- Stage / last_error: `13 / -60`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x6`, ready `True`
- Next: review graph-open code and add a host-side packet/CDC isolation test before re-enabling

Graph-open-only code is compiled but disabled after I-GRAPH-OPEN-01 lost CDC. No graph was opened in this run.
