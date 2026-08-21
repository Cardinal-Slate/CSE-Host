# Changelog

## [Unreleased]

- Initial cut: the self-hosting trampoline over psda programs.
- `cse_host_emit` composes a program (a body + a tail); `cse_host_run` runs a body through the floor;
  `cse_host_tail` is the hand-off; `cse_host` is the trampoline — the shared walk (`cse_map`) running each
  program along the tail chain. Opaque, slate-only.
