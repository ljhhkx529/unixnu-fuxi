# R-GPR-03

- Result: `BLOCKED_GPR`
- When: 2026-11-09T21:23:01+08:00
- Kernel: `2039fcfcb78ced76f37a0bb61f048d066a71390ac4bfae4cb80a462109d7f77c`
- Boot: `af1e1b182c63925533318bfe28432f0004af138276c54799b7254ab465ea2c18`
- CDC session: `82f24440a5bb5bdf`
- Stage / last_error: `13 / -60`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x6`, ready `True`
- Next: map BASIC_RESULT status 1 to the APM state machine or find the correct APM ready transition; do not open the speaker graph

Stable repeat of R-GPR-02 with raw-bytes reconstruction. Request and result opcode/token match; the DSP returns BASIC_RESULT status 1, not APM_SPF_STATE ready. No speaker graph or amplifier was started.
