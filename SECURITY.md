# Security policy

Slate is a numerics library: its core promise is that a result is either exact or a fail-closed
refusal, never a silent wrong answer. That makes its security surface two concrete things — the
memory-safety of the portable C floor, and the integrity of the exact-or-refuse contract — not a
network or auth surface. This policy covers both.

## Supported versions

Slate is pre-1.0. Only the latest `0.MINOR` line receives security fixes; there is no back-porting
to an earlier `0.MINOR`.

| Version | Supported |
|---------|-----------|
| 0.1.x   | Yes — the current line |
| < 0.1.0 | No |

Versions are semantic (`include/slate/version.h`); pre-1.0, a MINOR release may change API and a
PATCH release is fixes only ([CHANGELOG.md](CHANGELOG.md)).

## What counts as a vulnerability here

The high-value reports for a correctness library are the ones that break a stated guarantee:

- **A memory-safety defect in the C floor or the C ABI** — an out-of-bounds read/write, a
  use-after-free, an integer overflow that reaches an allocation size, or any undefined behavior in
  `core/lib/rns/` or the seam. The sanitizer and static-analysis gates (`make SAN=1 check`,
  `make analyze`) exist to catch these before they ship; a case that passes those gates and is still
  unsound is exactly what we want to hear about.
- **A break of the exact-or-refuse contract** — an input that makes a shipping entry point return a
  wrong value instead of the exact value or a typed refusal. A false-accept in RNS reconstruction, a
  bad-prime path that returns garbage rather than escalating to the host floor, or a GPU backend that
  produces a value diverging from the CPU floor without refusing, all qualify. (The raw
  `slate_fold` / `slate_reconstruct` primitives are documented as unchecked — a wrong value from
  those, used outside the checked `slate_eval` wrapper, is a documentation/usage matter, not a
  vulnerability. A wrong value from a *checked* entry is a vulnerability.)
- **A crash, hang, or unbounded allocation reachable from untrusted input** to a public entry point.

Slate has no network listener, no privileged process, and no credential handling; there is no auth
or transport surface to report against.

## Reporting a vulnerability

Report privately — please do not open a public issue for a suspected vulnerability.

- **Preferred:** GitHub's private vulnerability reporting for this repository (the repo's
  **Security → Report a vulnerability** tab), which keeps the report and the fix discussion private
  until a release.
- **Email:** `abdobrian@gmail.com` <!-- maintainer: replace with a dedicated security alias if you set one up -->.

Please include enough to reproduce: the platform and arch (arm64 / x86_64 / wasm32), the backend
linked (host floor, Metal, Vulkan, CUDA, WebGPU), whether it reproduces on the CPU floor alone, and a
minimal input or program. A reproducer against the C floor is the most actionable form, since the
floor is the single reference every backend is gated against.

## What to expect

This is a small, pre-1.0 project maintained on a best-effort basis; the timelines below are targets,
not a contractual SLA.

- **Acknowledgement** within 5 business days.
- **An initial assessment** — whether it reproduces and how we read the severity — within 10 business
  days.
- **Coordinated disclosure:** we will agree a disclosure date with you, fix in a private branch, and
  release a patched `0.MINOR.PATCH`. Credit in the release notes if you want it; anonymity if you
  prefer.

If you do not hear back within the acknowledgement window, please follow up — a missed message is far
more likely than a deliberate silence.
