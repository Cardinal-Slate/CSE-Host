# CSE-Host

The self-hosting runtime: run a program, follow its tail. A program is a structure — the `emit`/`run`/`tail`
effects made opaque. Slate-only — **usable on its own**, not tied to a language.

## Role

**Depends on [CSE-PAR](https://github.com/Cardinal-Slate/CSE-PAR)** (the floor's schedule),
**[CSE-Pool](https://github.com/Cardinal-Slate/CSE-Pool)** (cells), and a floor such as
**[CSE-RNS](https://github.com/Cardinal-Slate/CSE-RNS)** to actually compute a body; it reuses CSE-DSA's
walk and the spine throughout.

A program is a cell: its **payload is the body** (a leaf the floor runs) and its **rest is the tail** (the
program that runs next). The three effects are then trivial and opaque:

- `cse_host_emit(body, tail)` — compose a program (just `cse_cell`);
- `cse_host_run(prog)` — run its body through the floor (`cse_par_run`);
- `cse_host_tail(prog)` — the hand-off (the program's own next).

And because the tail *is* the next, the trampoline is the **shared walk**:

```
cse_host(seed)  ==  cse_map(cse_host_run, seed, cse_pool())
```

Finite programs compose over time into a service — no stack, no growing graph, no native driver. Only
slate measurements cross; a program never touches a byte (that stays in the floor provider, below the
seam). This is the opaque form of `eval.hpp`'s `emit`/`run`/`tail` — the byte carrier is gone because a
program already *is* the structure.

## Build

```
make            gate + standalone header + test, then libcse-host.a
make check      the gate and the test only
```

Build the deps first:
`make -C ../CardinalSlate lib && make -C ../CSE-DSA lib && make -C ../CSE-IO lib && make -C ../CSE-PAR lib && make -C ../CSE-Pool lib && make -C ../CSE-RNS lib`.

## License

MIT OR Apache-2.0.
