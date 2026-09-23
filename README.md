# CSE-Host

The C ABI and the composition root.

`array.h` (the front door as opaque handles, fragments kept as leaves and loaded by their word, splice,
`slate_dag_codec`), `number.h`, `reals.h`, and `engine_cabi.cpp`, which binds one platform, one codegen,
and the linked backends.

A program is a leaf: one cell per byte, under its own word, on the whole lens that word carries — the same
shape as every other row. Its name is that word and nothing else. A reader walks the cells, `word ‖ 0`,
`word ‖ 1`, … to the first cell nobody has; nothing anywhere says how many there are, because the records
are the count. A walk that stopped early is refused by the fragment parser, never run.

**Depends on:** everything

Part of the Cardinal-Slate engine. The umbrella, [CSE](https://github.com/Cardinal-Slate/CSE), pins every part at a
tag and runs the full gate over the merge of the parts; `make check` here delegates to it.

## Layout

Paths are the same as in the umbrella's merged view, so this repo drops into it unchanged.

| dir | what |
|---|---|
| `include/slate/` | 4 headers |
| `core/lib/` | 1 source |
| `tests/` | 17 gates |

## Provenance

Extracted from [CSE-Core](https://github.com/Cardinal-Slate/CSE-Core) at `6d3be1b` (plus the codec lift into `slate/codec.hpp`).

## Crossing includes

None.
