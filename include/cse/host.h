/// cse/host.h — the execution driver (the trampoline). It iterates a step over a program (a psda) until
/// the step signals a fixed point, and returns the result. The step is passed in opaquely — the driver
/// knows nothing about what it is driving, only that programs are psda. This is what makes it
/// self-hosting: the runtime is a slate loop over slate values, not foreign machinery. Depends on the
/// spine only. SPDX-License-Identifier: MIT OR Apache-2.0
#pragma once
#include "slate/psda.h"

/// one step of a program: given the pool and the current program, return the next program, or 0 = done.
typedef slate_psda *(*cse_step)(slate_psda **pool, slate_psda *prog);

/// drive `seed` to its fixed point: apply `step` until it returns 0, then return the last program.
slate_psda *cse_host(cse_step step, slate_psda **pool, slate_psda *seed);
