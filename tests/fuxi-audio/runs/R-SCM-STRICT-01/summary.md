# R-SCM-STRICT-01

- Result: `PASS_READY`
- When: 2026-11-09T19:45:16+08:00
- Kernel: `33e076b8b161cb4b4b93e4298ff48ce59a9331c543733aa7f2696e625d6c91ef`
- Boot: `a45a4b8d1090bbef89b810724879526728ef11b0110952a7da520d5c57692551`
- CDC session: `1e1e1b6926a53273`
- Stage / last_error: `11 / 0`
- SCM mask: `0x3f`, last `0x0/0x0`
- SMP2P 443: present `True`, index `2`, value `0x2`, ready `True`
- Next: advance the compile-time limit to 13 and validate GLINK/adsp_apps; do not start the speaker graph yet

The SCM bug was x2..x5 mapping: x2 was a instead of pas. With x2=pas and strict x0==0 && x1==0, all six PAS stages returned 0/0, the DSP wrote item 443, and slave-kernel value=2 became ready. GLINK and speaker were not started.
