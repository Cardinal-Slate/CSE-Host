# Changelog

Semantic versions (`MAJOR.MINOR.PATCH`). Pre-1.0, a MINOR release may change API; a PATCH release is fixes only.

## 0.1.2

README: the includes that cross the dependency stack, listed.

## 0.1.1

Remove `tests/support/prng.h`, a test helper only CSE-RNS's gates use; it was copied into every part and collided in the umbrella's merged view.

## 0.1.0

Extracted from CSE-Core `6d3be1b`: The C ABI and the composition root. Paths unchanged; gates carried with the code.
