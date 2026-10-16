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

## Selecting a stored implementation

`slate/transfer.h` composes the existing ABI without adding an arithmetic operation or storage format:

* `slate_transfer_write` writes a fragment, loads the stored copy to validate it, and selects its word
  under a caller-supplied scope and operation name.
* `slate_transfer_read` reads that selection from the store on every call. `missing` permits a caller
  to choose its legacy implementation; a broken selected implementation refuses.
* `slate_transfer_invoke` reads the selection and invokes the stored construction. The existing engine
  keeps and reuses answers. Different selected constructions have different identities.
* `slate_transfer_mul31` is the first typed caller, using `rns.mul31` in the supplied scope. It accepts
  residues `a,b < p` and `2 <= p <= 2147483647`. `allow_legacy=0` disables its old-expression fallback;
  `used_store` reports which successful path supplied the result.

```c
/* b has the caller's store/codec installed; scope is its chosen namespace.
   program/n are CSE-RNS/core/programs/rns/mul31.slate read as bytes. */
const uint8_t name[] = "rns.mul31";
const char *err = slate_transfer_write(b, scope, scope_n, name, sizeof name - 1, program, n);
/* On a fresh builder attached to the same store: */
uint64_t answer;
int used_store;
if (!err) err = slate_transfer_mul31(b, scope, scope_n, 6, 7, 31, 0, &answer, &used_store);
/* answer == 11; used_store == 1. No caller-side multiplication fallback was allowed. */
```

Installation validates structure, not mathematical equivalence. Compare both implementations before
selecting one for use. Writes inherit the store's durability and concurrency behavior; selection is
not a new transactional registry. The selected word is executable code and its scope must be controlled
by the caller. The existing engine still executes the construction; this adapter does not remove it.

The focused gate is `make -C CSE-Core/.view -f Makefile.slate-full check-transfer`, after `make -C CSE-Core view`.
The umbrella's normal engine gate includes it as well.
