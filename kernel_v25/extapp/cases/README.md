# CE regression corpus

Two kinds of source live here:

* `*.GO` — scripts for the **Go subset interpreter** (`/bin/go.ce`).
* `*.C` — sources for the **target-one C compiler** (`/bin/cc.ce`), which
  compiles C *on CatOS itself* (not with the host gcc).

Both sets were ported verbatim from the original project's tree
(`extapp/cases/*.GO`, `extapp/cases/*.C`, `tests/CHARBUG.C`,
`tools/TNC2.C`); the file contents are byte-identical to the originals.

## Go scripts — runnable today

| script      | how to run                        | expected                        |
|-------------|-----------------------------------|---------------------------------|
| `DEMO.GO`   | `run /bin/go.ce`                  | `fib(10)= 55`, `demo done`      |
| `TEST.GO`   | `run /bin/go.ce /BIN/TEST.GO`     | 34 `PASS` lines, then `ALL PASS` |
| `MINI_GO.GO` | `run /bin/go.ce /BIN/MINI_GO.GO`  | prints `7`                      |
| `MINI_IN.GO` | second argument to `MINI_GO.GO`   | input consumed by `MINI_GO.GO`  |
| `SYN.GO`    | `run /bin/go.ce /BIN/SYN.GO`      | `syntax error at line 3`        |

The five `.GO` files are also embedded in the kernel image
(`build/gen_builtin.py`, the `OPTIONAL` list), so they are installed on the
FAT volume as `/bin/*.GO` at format time. `SYN.GO` is a deliberate
syntax error — it exists to prove that the lexer reports errors instead
of accepting them.

Host-side regression (no emulator needed) — see `tests/go_regress.py`
for the on-metal version and `kernel_v25/extapp/host_stub_go.c` for a
native build of the same interpreter:

```sh
gcc -O1 -g -I ../include host_stub_go.c go.c -o /tmp/go_host
cd cases && /tmp/go_host /BIN/TEST.GO      # -> ALL PASS
```

## C sources — require the cc.c compiler stages

> **These are NOT expected to compile yet.** The C compiler (`cc.c`) is
> being developed in stages, and three of these cases exercise language
> features that only the later stages implement. They are landed here
> **as the acceptance corpus for those stages**, not as a green test
> suite.

| case       | exercises                                              | needs            |
|------------|--------------------------------------------------------|------------------|
| `HELLO.C`  | target-one bootstrap: `api->` calls, recursion, `while`  | cc stage 1 (baseline) |
| `EXTRA.C`  | FIND-05 shift operand, FIND-04 `for`+`continue`, FIND-04b step-less `for`, FIND-02/03 global string initialisers | cc stage 1 |
| `NOCE.C`   | a source with **no** `ce_main` — cc must reject it at compile time and emit no image (FIND-01) | cc stage 1 (negative test) |
| `TNC2.C`   | `char *gv;` global initialised from a string literal (`gv = "hi"`), `main()` instead of `ce_main()` — a target-two (host gcc) probe | cc stage 2 |
| `STRUCT.C` | `struct` / `typedef` / `enum` / `.` / `->` / `sizeof`  | **cc stage 1–3** |
| `SWITCH.C` | `switch` / `case` / `default` / `break`                 | **cc stage 1–3** |
| `GOTO.C`   | `goto` / labels, prefix and postfix `++`/`--`          | **cc stage 1–3** |
| `CHARBUG.C`| FIND-S1-01: a `char` array member indexed with `[i]` must store **one byte**, not a 4-byte move that would clobber the adjacent `int` | **cc stage 1–3** |

The three cases marked *cc stage 1–3* (`STRUCT.C`, `SWITCH.C`, `GOTO.C`)
are the stage-1/2/3 acceptance tests of the C compiler: `STRUCT.C` is the
stage-1 test (declarations), `SWITCH.C` the stage-2 test (statements),
`GOTO.C` the stage-3 test (control flow). Until those compiler stages
land they are **expected to fail to compile** — that failure is the
remaining work, not a defect in this corpus.

`CHARBUG.C` is the regression case for FIND-S1-01; it is a stage-1–3
feature (struct member access) and therefore carries the same
dependency.

`TNC2.C` uses `main()` rather than `ce_main()` and is a *target-two*
probe: it is meant to be compiled by the **host** gcc, not by `cc.ce`.
It is kept here because it is the smallest possible reproducer for
global pointer initialisation from a string literal.

## Expected output of the C cases

Each case prints a single line; the comment at the top of each file
records the exact expected line.

```
HELLO.C    cc-compiled hello fib(10)=55 sum(5)=15
EXTRA.C    sh=12 t=8 c=3 z=12 msg=hi p1=A p2=B
STRUCT.C   x=7 y=10 gp=17 nm=hi A=0 B=1 C=10 D=11 sz=20
SWITCH.C   c1=100 c2=200 c5=500 c9=-1
GOTO.C     s=10 i=5 post=3 i2=4 pre=5
CHARBUG.C  x=42
```
