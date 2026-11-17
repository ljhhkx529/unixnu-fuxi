# R-GPR-01

- Result: `BLOCKED_GPR`
- When: 2026-11-09T20:33:41+08:00
- Kernel: `4b108b038a7b6aa0f4eeafe519225178d5ae36bab4f2ea07a07149d48357b606`
- Boot: `10d1aebdd9b698a805c87207576768a4631afc52e46876b09a99db58b7eaae5c`
- CDC session: `2634a444dbffe9b8`
- Stage / last_error: `13 / -60`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x6`, ready `True`
- Next: inspect the APM GET_SPF_STATE opcode/payload handling and the DSP's BASIC_RESULT status 1; do not open the speaker graph yet

GLINK and the GPR channel are healthy (tx 108, rx 226). The SPF query path sent 50 bounded queries but only saw BASIC_RESULT status 1; no APM_SPF_STATE ready reply. No speaker graph or amplifier was started.
