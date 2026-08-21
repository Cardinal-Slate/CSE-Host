/// cse/host.h — the self-hosting runtime: run a program, follow its tail. Opaque.
/// SPDX-License-Identifier: MIT OR Apache-2.0
///
/// A program is a structure — a body (a leaf the floor runs) and a tail (the program that runs next).
/// These are the emit / run / tail effects made opaque: psda in, psda out, no byte carrier rebuilt
/// between hops. `cse_host` is the trampoline — run each program's body, hand off to its tail, collect
/// the results in order. Because a program's tail is its own next, the trampoline is just the shared
/// walk (cse_map), so finite programs compose over time with no stack and no growing graph. Only slate
/// measurements cross; a program never touches a byte — that stays in the floor provider, below the seam.
#pragma once
#include "slate/psda.h"

/// EMIT — make a program from a `body` (a leaf the floor runs) and a `tail` (the next program, or 0).
slate_psda *cse_host_emit(slate_psda *body, slate_psda *tail);

/// RUN — run one program's body through the floor; returns the value it computes.
slate_psda *cse_host_run(slate_psda *prog);

/// TAIL — the program this one hands off to, or nothing.
slate_psda *cse_host_tail(slate_psda *prog);

/// HOST — the trampoline: run `seed`, follow each tail, collect the values in order.
slate_psda *cse_host(slate_psda *seed);
