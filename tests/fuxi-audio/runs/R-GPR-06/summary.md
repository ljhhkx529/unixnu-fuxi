# R-GPR-06

- Result: `BLOCKED_GPR`
- When: 2026-11-09T22:33:59+08:00
- Kernel: `07720336a2e610a435953b6d27641e024fac281c4e97b1a4671e748b027a978c`
- Boot: `132948e03b6dc00df2e4058dff87d9b422b04b7a895a16c936560f747a4c0324`
- CDC session: `43851b4d550ea42d`
- Stage / last_error: `13 / -60`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x6`, ready `True`
- Next: do not retry the unstable 40-byte form; determine whether Fuxi firmware implements GET_SPF_STATE at all or use a different readiness gate only after explicit approval

Recovered workspace and device to the last stable 24-byte query. Status 1 is ADSP_EFAILED (general failure), not ready. The 40-byte q6apm-style APM-header image is recorded as incident I-APM-HEADER-01 and is not reused.
