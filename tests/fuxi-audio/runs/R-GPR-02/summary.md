# R-GPR-02

- Result: `BLOCKED_GPR`
- When: 2026-11-09T20:55:30+08:00
- Kernel: `84f36ecf426f267559a4d4082b1973b63fb5bbe72703b1b7d0b89a8e03892520`
- Boot: `a957274f0f2bf3c6688adb1a14246998d41b30659b6fc9db934f82ffcb73a9df`
- CDC session: `54f10744dd315618`
- Stage / last_error: `13 / -60`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x6`, ready `True`
- Next: treat BASIC_RESULT status 1 as the DSP's APM response; inspect its APM/SPF state transition or wait policy before any graph work

Opcode/token/payload validation only. The query is correct; the DSP answers BASIC_RESULT status 1 instead of APM_SPF_STATE ready. No speaker graph or amplifier was started.
