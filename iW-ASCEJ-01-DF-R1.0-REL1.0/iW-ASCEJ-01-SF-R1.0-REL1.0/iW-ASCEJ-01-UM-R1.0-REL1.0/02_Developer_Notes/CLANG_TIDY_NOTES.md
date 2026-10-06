# clang-tidy: readability-misleading-indentation

## What the check means

It fires when an `else` is not indented to the same column as its `if`. The
code is *correct* — the compiler pairs `if`/`else` by braces and grammar, not
by whitespace — but a reader scanning the indentation will pair them wrongly.
It is a readability check, not a correctness one. **Whitespace-only fix.**

## Sites found and fixed in this tree

| File | Line (as delivered) | `if` col | `else` col |
|---|---|---|---|
| `main.c` | menu `else if (choice == 14)` | 8 | 15 |
| `main.c` | menu `else if (choice == 15)` | 8 | 15 |
| `main.c` | menu `else if (choice == 16)` | 8 | 15 |
| `rfdc_cmd.c` | `else{` after the `}// gen3` block | 5 tabs | 6 tabs |

The three `main.c` hits are the tail of the 16-option menu chain: options 1–13
sit at column 8, and 14–16 drifted to 15 when they were added later. All four
realigned. Re-verified: **0 sites remaining**, and `gcc -Wall -Wextra
-Wmisleading-indentation` is clean across every module.

## How to fix it yourself

Align the `else` to its `if`. Nothing else changes.

```c
/* before - else drifted right */
        if (choice == 13) {
            ...
        }
               else if (choice == 14) {

/* after */
        if (choice == 13) {
            ...
        }
        else if (choice == 14) {
```

Watch for the tabs-vs-spaces case: a line that *looks* aligned in one editor
can be misaligned to the compiler if one line uses tabs and the other spaces.
`rfdc_cmd.c` is tab-indented; `main.c`'s live region is space-indented. Keep
each file consistent with itself.

## Suppressing (only when the check is wrong)

```c
// NOLINTNEXTLINE(readability-misleading-indentation)
```

Prefer fixing the indentation. In this codebase every instance was a genuine
drift, so nothing needed suppressing.

## Note on line numbers

If your line numbers differ from the table above, your `main.c` has diverged
from the delivered file — most likely because the archived commented-out
region (lines 1–2259) was deleted. That shifts every live line down by ~2259.
The delivered file still contains that region.
