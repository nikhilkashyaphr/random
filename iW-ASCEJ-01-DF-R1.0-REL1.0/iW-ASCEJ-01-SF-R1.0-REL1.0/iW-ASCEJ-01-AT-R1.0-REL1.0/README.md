# iW-ASCEJ-01-AT-R1.0-REL1.0 — Acceptance Test

| Path | Contents |
|---|---|
| `iW-ASCEJ-01-ATP-R1.0-REL1.0.pdf` (+ `.md`) | **Acceptance Test Procedure**: test environment, 10 automated tests (TS-01…TS-10), 20 bench tests (BT-01…BT-20) with objective, set-up, steps and expected result |
| `iW-ASCEJ-01-ATR-R1.0-REL1.0.xlsx` | **Acceptance Test Report**. *Summary* holds the live status and sign-off. *Automated Results* has one row per test. *Check Detail* has all 244 checks, verbatim. *Bench Record* has yellow cells for the board results. |
| `01_Logs/` | Raw logs of the automated run that the ATR records (`TS-00` is the `run_all.sh` summary; `TS-01`…`TS-10` are the per-test `ctest -V` outputs) |

Status at delivery: automated **10/10 tests, 244/244 checks PASS**. The bench
tests are open until they are run on the target board.
