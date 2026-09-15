# CSE-Host

The C ABI and the composition root.

`array.h` (the front door as opaque handles, fragments, splice, stop/restore, `slate_dag_codec`), `number.h`, `reals.h`, and `engine_cabi.cpp`, which binds one platform, one codegen, and the linked backends.

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
