// The C ABI door: slate/slate.h (build a record, run it, read the reading), slate/embed.h (an embedder's effects,
// sandbox, async, search) and slate/tune.h (cost knobs) over Slate::DagBuild.
//
// SPDX-License-Identifier: MIT OR Apache-2.0
//
// Pure wrapper: a builder owns an Arena (single-threaded, so a SlateDag is one caller's), and every entry is a
// thin pass-through — node ids in, a result handle out. No exception crosses the C boundary: bad_alloc (or
// anything else) becomes the entry's refusal name. Exposes only the C symbols those three headers declare; the
// contracts live on those declarations.
#include <functional>
#include <unistd.h>      /* the engine's built-in fd byte-stream: read/write/close */
#include <poll.h>        /* accept polls with a timeout so a blocked wait stays cancellable */
#include "slate/providers/host/io.hpp"   /* the file/tcp open providers + install_io_providers() */
#include "slate/engine/engine.hpp"
#include "slate/engine/ops.hpp"
#include "slate/slate.h"
#include "slate/embed.h"
#include "slate/tune.h"
#include "slate/dag/build.hpp"
#include "slate/dag/search.hpp"   /* DagBuild::search — the S axis, exposed as slate_dag_search */
#include "slate/async/scope.hpp"   /* the fiber runtime — compiled into the archive so async is a pure-C surface */
#include "slate/engine/context.hpp"
#include "slate/trace.h"

/* ===== The array front door: Slate::DagBuild (dag/build.hpp) exposed as opaque C handles ===================
 *
 * A SlateDag owns an Arena DAG; each builder call is a thin pass-through to DagBuild, whose node handles are
 * already plain ints — so nothing composed crosses the ABI, only node ids in and a result handle out. run()
 * is the same product-grid dispatch (dag/lower/install.hpp) the C++ surface runs; it hands back an owned
 * ArrayReading that outlives its builder. Every array op (map / gather / transpose / matmul / conv / dot) is
 * built *from* these calls in the caller's language, never bound one at a time. A bad node/carrier id is
 * caught at the boundary and poisons the builder, so run() refuses rather than dereferencing garbage.
 * No exception crosses the C boundary. */

/* The self-hosting seam (emit/run) — one process-wide, engine-owned FragOps table, installed on every builder's
 * Envelope so a running .slate can author and run other fragments with no host code. Defined at file end (it
 * calls the C-ABI entry points below); forward-declared here for the SlateDag ctor. */
static const Slate::FragOps *engine_frag_ops();

/* ---- the engine's built-in fd byte-stream: read/write/close on an open fd. Generic and scheme-free — it has no
 * open/connect and no file/tcp knowledge. Opening (the file/tcp specifics) lives in the provider registry, gated by
 * the compose grant (io_policy) that says which paths are mounted and which endpoints exposed; this seam only moves
 * bytes on an fd a provider already opened. Installed on env.io_stream by every SlateDag. Operands cross by
 * reference (SlateOperand); a byte is the low byte of each element; ops[0] is the word of the open resource, and
 * the fd it names arrives resolved in the context (the engine keeps the table; the fd never enters a word).
 * ----
 *   op = blob[0]: read (chunk=blob[1..4]) -> bytes read; write (ops[1]=data) -> count(8 LE); close -> 0(8 LE). */
static inline uint8_t sl_opbyte(const SlateOperand *o, uint64_t i) {
  return ((const uint8_t *)o->plane)[i * (o->elem_bytes ? o->elem_bytes : 1)];
}
static int builtin_io_stream(const char *cls, const uint8_t *blob, uint64_t blen, const SlateOperand *ops,
                             uint32_t nops, uint8_t **out, uint64_t *outn, void *user) {
  (void)cls;
  if (blen < 1 || nops < 1 || !ops[0].plane) return 1;
  Slate::IoStreamCtx *ctx = static_cast<Slate::IoStreamCtx *>(user);   /* scoped ram store + cancel gate + the fd */
  if (!ctx || ctx->fd < 0) return 1;
  int64_t fd = ctx->fd;
  auto ret8 = [&](int64_t v) -> int {
    uint8_t *o = (uint8_t *)std::malloc(8); if (!o) return 1;
    uint64_t u = (uint64_t)v; for (int i = 0; i < 8; i++) o[i] = (uint8_t)(u >> (8 * i));
    *out = o; *outn = 8; return 0;
  };
  const bool is_read = Slate::blob_verb_is(blob, blen, "read"), is_write = Slate::blob_verb_is(blob, blen, "write"),
             is_close = Slate::blob_verb_is(blob, blen, "close"), is_accept = Slate::blob_verb_is(blob, blen, "accept");
  uint64_t rn = 0; const uint8_t *rest = Slate::blob_verb_rest(blob, blen, &rn);   /* the verb's own bytes */

  /* a tmpfs handle: route to the per-container ram store, not a syscall (the fd is marked above any host fd). */
  if (ctx && ctx->mem && ctx->mem->is_mem(fd)) {
    Slate::MemFs::Handle *h = ctx->mem->get(fd);
    if (!h) return 1;
    if (is_read) {
      if (rn < 4) return 1;
      uint32_t chunk = (uint32_t)rest[0] | ((uint32_t)rest[1] << 8) | ((uint32_t)rest[2] << 16) | ((uint32_t)rest[3] << 24);
      if (chunk == 0 || chunk > (1u << 30)) return 1;
      std::vector<uint8_t> b = ctx->mem->read(h, chunk);
      uint8_t *o = (uint8_t *)std::malloc(b.size() ? b.size() : 1); if (!o) return 1;
      if (!b.empty()) std::memcpy(o, b.data(), b.size());
      *out = o; *outn = b.size(); return 0;
    }
    if (is_write) {
      if (nops < 2) return 1;
      uint64_t n = ops[1].n; std::vector<uint8_t> tmp((size_t)n);
      for (uint64_t i = 0; i < n; i++) tmp[i] = sl_opbyte(&ops[1], i);
      int64_t wr = ctx->mem->write(h, tmp.data(), n);           /* -1 = past the tmpfs cap or an overflow: refuse */
      if (wr < 0) return 1;
      return ret8(wr);
    }
    if (is_close) { ctx->mem->close(fd); return ret8(0); }   /* free the slot for reuse */
    return 1;                                                   /* accept on a ram file is nonsense */
  }

  /* host-fd isolation: a read/write/close/accept may only touch an fd this container opened. A fabricated or
   * cross-container fd number is refused, so co-tenant containers in one process can't reach each other's fds. */
  if (ctx && ctx->mem && !ctx->mem->owned_fds.count((int)fd)) return 1;

  /* poll a host fd with the cancel gate before a blocking verb, so a stalled socket read/write can't hang a server
   * (a regular file polls ready immediately). No gate installed: skip polling and let the syscall run. */
  auto wait_ready = [&](short events) -> int {
    if (!(ctx && ctx->proceed)) return 0;
    for (;;) {
      if (!ctx->proceed(ctx->proceed_user)) return -1;
      struct pollfd pfd; pfd.fd = (int)fd; pfd.events = events; pfd.revents = 0;
      int pr = ::poll(&pfd, 1, 200);
      if (pr < 0) return -1;
      if (pr > 0) return 0;
    }
  };

  if (is_read) {
    if (rn < 4) return 1;
    uint32_t chunk = (uint32_t)rest[0] | ((uint32_t)rest[1] << 8) | ((uint32_t)rest[2] << 16) | ((uint32_t)rest[3] << 24);
    if (chunk == 0 || chunk > (1u << 30)) return 1;
    if (wait_ready(POLLIN)) return 1;                            /* cancelled while waiting for data */
    uint8_t *buf = (uint8_t *)std::malloc(chunk); if (!buf) return 1;
    ssize_t r = ::read((int)fd, buf, chunk); if (r < 0) { std::free(buf); return 1; }
    *out = buf; *outn = (uint64_t)r; return 0;
  }
  if (is_write) {
    if (nops < 2) return 1;
    if (wait_ready(POLLOUT)) return 1;                           /* cancelled while waiting to send */
    uint64_t n = ops[1].n; uint8_t *tmp = (uint8_t *)std::malloc(n ? n : 1); if (!tmp) return 1;
    for (uint64_t i = 0; i < n; i++) tmp[i] = sl_opbyte(&ops[1], i);
    ssize_t w = ::write((int)fd, tmp, (size_t)n); std::free(tmp); if (w < 0) return 1;
    return ret8((int64_t)w);
  }
  if (is_close) {
    ::close((int)fd);
    if (ctx && ctx->mem) {                                       /* release the fd-cap slot and drop ownership */
      if (ctx->mem->open_host_fds > 0) ctx->mem->open_host_fds--;
      ctx->mem->owned_fds.erase((int)fd);
    }
    return ret8(0);
  }
  if (is_accept) {                                             /* a new client fd off a listening fd */
    /* poll with a short timeout so a blocked accept stays cancellable: between polls, ask the gate whether to keep
     * waiting. Without a gate this is a plain blocking accept. */
    for (;;) {
      if (ctx && ctx->proceed && !ctx->proceed(ctx->proceed_user)) return 1;   /* cancelled while waiting */
      struct pollfd pfd; pfd.fd = (int)fd; pfd.events = POLLIN; pfd.revents = 0;
      int pr = ::poll(&pfd, 1, (ctx && ctx->proceed) ? 200 : -1);
      if (pr < 0) return 1;
      if (pr == 0) continue;                                   /* timed out: re-check the gate and wait again */
      int c = ::accept((int)fd, nullptr, nullptr);
      if (c < 0) return 1;
      if (ctx && ctx->mem) {                                   /* the accepted client fd counts against the cap + is owned */
        if (ctx->mem->open_host_fds >= ctx->mem->host_fd_cap) { ::close(c); return 1; }
        ctx->mem->open_host_fds++;
        ctx->mem->owned_fds.insert(c);
      }
      return ret8((int64_t)c);
    }
  }
  return 1;
}

struct SlateDag {
  Slate::Arena arena;
  Slate::DagBuild build;
  bool err;
  bool want_device;   /* the scope's device-executor choice; run() opens a DeviceScope so dispatch routes to it */
  Slate::EffectHost effect_host;   /* the OS-effect vtable; env().effect_host points here once installed */
  Slate::IoPolicy io_policy;       /* the compose grant (mounts/ports); env().io_policy points here, filled by setters */
  Slate::MemFs    memfs;           /* the per-container ram store backing tmpfs; env().memfs points here — scoped */
  Slate::ProviderRegistry providers;  /* this container's own scheme->provider table; env().providers points here */
  std::vector<uint8_t> tail_prog_buf;   /* the tail-continuation slot: "slate.tail" writes the next program's word here */
  bool tail_flag_buf = false;           /* set when a tail hand-off was recorded; the run trampoline reads it */
  std::vector<int64_t> dims_buf;        /* the dims of the last dispatch (or the ones an ask brought): the ask's dims */
  explicit SlateDag(size_t abytes) : arena(abytes), build(arena), err(false), want_device(false) {
    arena.env().frag_ops  = engine_frag_ops();  /* self-hosting seam: emit/run reach the engine, not a host */
    arena.env().frag_self = this;               /* typed owner back-pointer; emit verifies it matches the arena */
    arena.env().tail_prog = &tail_prog_buf;     /* this container is its own tail target; a tick shares this slot */
    arena.env().tail_flag = &tail_flag_buf;
    /* the io capability, container-style: open (the scheme-specific step) routes through the provider registry
       gated by io_policy (the compose grant — which paths are mounted, which endpoints exposed); once open returns
       an fd the engine moves bytes on it itself (read/write/close) via the built-in byte-stream. io_policy starts
       empty (fail-closed: every open refuses until a compose spec grants a mount or a port). */
    Slate::install_io_providers(providers);   /* bind the available ways into this container's own registry, not a global */
    arena.env().io_policy = &io_policy;
    arena.env().memfs = &memfs;
    arena.env().providers = &providers;
    arena.env().io_stream = reinterpret_cast<decltype(arena.env().io_stream)>(&builtin_io_stream);
  }
  SlateDag() : SlateDag(((size_t)1) << 20) {}
};

struct SlateArray {
  Slate::ArrayReading ar;
  Slate::Codec codec;   /* the region's codec, copied at the dispatch: what spells this reading's cell words */
  mutable slate_entry entries[8]; mutable uint64_t height = 0;   /* the reading, as entries, filled on ask */
};

/* a node id is valid iff the builder is live, un-poisoned, and the id indexes a node already built into it. */
static inline bool sd_node_ok(SlateDag *b, int32_t id) {
  return b && !b->err && id >= 0 && (size_t)id < b->arena.lower_node_count();
}

extern "C" SlateDag *slate_dag_new(void) try {
  return new SlateDag();
} catch (...) {
  return nullptr;
}

extern "C" SlateDag *slate_dag_new_sized(uint64_t arena_bytes) try {
  return new SlateDag(arena_bytes ? (size_t)arena_bytes : (((size_t)1) << 20));
} catch (...) {
  return nullptr;
}

extern "C" void slate_dag_free(SlateDag *b) {
  delete b;
}

/* Scope on the builder: each setter patches the builder arena's Envelope (the same measurement context the
 * C++ Region guards patch), and slate_dag_run activates it for the dispatch. A knob picks who computes or the
 * a-priori height it computes to, never the value — so every setter is exact-preserving. All patch the same
 * Envelope, so calls compose; the last write to an axis wins. */
extern "C" const char *slate_dag_potential(SlateDag *b, uint32_t bits) {
  if (!b) return "args";
  b->arena.env().height_bits = (long long)bits;   /* Potential: the RNS value-model height ceiling */
  return nullptr;
}
extern "C" const char *slate_dag_threads(SlateDag *b, uint32_t n) {
  if (!b) return "args";
  b->arena.env().thread_cap = (int)n;             /* host fan-out cap; 0 = hardware concurrency */
  return nullptr;
}
extern "C" const char *slate_dag_device(SlateDag *b, int on) {
  if (!b) return "args";
  b->want_device = on != 0;                       /* run() opens a DeviceScope so dispatch routes to the device */
  return nullptr;
}
extern "C" const char *slate_dag_jit(SlateDag *b, int on) {
  if (!b) return "args";
  b->arena.env().driver_jit = (int8_t)(on ? 1 : 0);
  return nullptr;
}
extern "C" const char *slate_dag_vec(SlateDag *b, int on) {
  if (!b) return "args";
  b->arena.env().driver_vec = (int8_t)(on ? 1 : 0);
  return nullptr;
}
extern "C" const char *slate_dag_telemetry(SlateDag *b, const char *level) {
  int32_t l = slate_level_of(level);
  if (!b || l < 0) return "args";
  b->arena.env().telemetry = l;                   /* the level the name stands for (-1 on the Envelope = unset) */
  return nullptr;
}
extern "C" const char *slate_dag_trace(SlateDag *b, slate_emit emit, void *user) {
  if (!b) return "args";
  b->arena.env().trace = emit ? std::make_shared<Slate::TraceSink>(emit, user) : nullptr;
  return nullptr;
}
extern "C" const char *slate_dag_width(SlateDag *b, uint32_t prec_bits) {
  if (!b) return "args";
  b->arena.env().width_target = prec_bits;        /* real-read bracket width target (2^-prec_bits) */
  b->arena.env().config_set |= Slate::Envelope::CFG_WIDTH;   /* mark set: default-looking values still inherit-override */
  return nullptr;
}
extern "C" const char *slate_dag_work(SlateDag *b, uint64_t cap) {
  if (!b) return "args";
  b->arena.env().work_cap = cap;                  /* real-read refine-iteration cap; 0 = the loop's own cap */
  return nullptr;
}
extern "C" const char *slate_dag_mem(SlateDag *b, uint64_t bytes) {
  if (!b) return "args";
  b->arena.env().mem_budget = bytes;              /* per-read scratch cap; past it a real read refuses, never OOMs */
  b->arena.env().config_set |= Slate::Envelope::CFG_MEM;
  return nullptr;
}
extern "C" const char *slate_dag_channel(SlateDag *b, const char *pin, const char *const *allow, uint32_t nallow) {
  if (!b || (nallow && !allow)) return "args";
  int p = pin ? Slate::engine::channel_type_of(pin) : (int)Slate::engine::ChannelType::Auto;
  if (p < 0) return "args";
  uint32_t mask = 0xFFFFFFFFu;
  if (allow) {
    mask = 0;
    for (uint32_t i = 0; i < nallow; i++) {
      int t = Slate::engine::channel_type_of(allow[i]);
      if (t < 0) return "args";
      mask |= 1u << t;
    }
  }
  b->arena.env().channels.pin = (Slate::engine::ChannelType)p;   /* the lane the name stands for */
  b->arena.env().channels.allow_mask = mask;                    /* the permitted lanes, as named */
  b->arena.env().config_set |= Slate::Envelope::CFG_CHANNELS;
  return nullptr;
}
/* ---- the lens and the roster: what this container computes, and the whole lens its words name ----
 *
 * Unlike every other knob on a builder these reach the word: the lens is written into a reading's record, so a
 * value read through one lens is a different row from the same value read through another. That is the point —
 * it is what lets hosts divide a number between them and still agree on what each piece is called, with no
 * message between them. A container that carries a roster names the roster in its words and computes one share
 * of it, so two machines on two shares of one roster ask for the same rows. */

/* the pool's rule at the door, one body: each prime at least 2, under the lane's width, actually prime, not the
 * pool's guard, no repeats. The same rule for a lens and for a roster — they are the same kind of list. */
static const char *lens_primes_of(const int64_t *primes, uint32_t k, std::vector<int64_t> &out) {
  const int64_t guard = Slate::rns_pool().second;   /* the pool's reserved check channel — not a pinnable one */
  out.clear();
  out.reserve(k);
  for (uint32_t i = 0; i < k; i++) {
    const int64_t p = primes[i];
    if (p < 2 || p >= ((int64_t)1 << 24)) return "args";        /* outside the width the residue lane carries */
    if (p == guard || !Slate::is_prime(p)) return "args";       /* the pool's own rule: prime, and not its guard */
    for (uint32_t j = 0; j < i; j++) if (primes[j] == p) return "args";   /* a repeat is a non-squarefree modulus */
    out.push_back(p);
  }
  return nullptr;
}

/* What a share is, one body: a contiguous sub-range of the roster — roster[lo, hi) for some 0 <= lo < hi <= k.
 * Every division of a roster hands each unit exactly one such run, so this is the whole of what the engine can
 * say about a share. How many units there are, and which run is this one's, is the deployment's rule — the same
 * on every unit, read at start and asked of the seam — never a container's setting and never in a word. */
static bool roster_subrange(const std::vector<int64_t> &roster, const std::vector<int64_t> &ps) {
  if (ps.empty() || ps.size() > roster.size()) return false;
  for (size_t lo = 0; lo + ps.size() <= roster.size(); lo++)
    if (std::equal(ps.begin(), ps.end(), roster.begin() + (ptrdiff_t)lo)) return true;
  return false;
}

extern "C" const char *slate_dag_lens(SlateDag *b, const int64_t *primes, uint32_t k) {
  if (!b || (k && !primes)) return "args";
  std::vector<int64_t> ps;
  if (const char *rc = lens_primes_of(primes, k, ps)) return rc;
  Slate::engine::ChannelPolicy &cp = b->arena.env().channels;
  /* with a roster set these are the channels this container computes: one share of it, which is a contiguous
     run of its primes (and never none — a container carrying a roster carries one of its shares). */
  if (!cp.roster.empty() && !roster_subrange(cp.roster, ps)) return "args";
  cp.primes = std::move(ps);
  b->arena.env().config_set |= Slate::Envelope::CFG_CHANNELS;
  return nullptr;
}
extern "C" const char *slate_dag_roster(SlateDag *b, const int64_t *primes, uint32_t k) {
  if (!b || (k && !primes)) return "args";
  std::vector<int64_t> rs;
  if (const char *rc = lens_primes_of(primes, k, rs)) return rc;
  Slate::engine::ChannelPolicy &cp = b->arena.env().channels;
  /* a lens already pinned must still be a share of the roster being set — the two are one statement */
  if (!rs.empty() && !cp.primes.empty() && !roster_subrange(rs, cp.primes)) return "args";
  cp.roster = std::move(rs);
  b->arena.env().config_set |= Slate::Envelope::CFG_CHANNELS;
  return nullptr;
}


/* The RNS channel cost model — the throughput and dispatch coefficients the planner ranks a lane by. Calibration
 * for cross-hardware benchmarking: it moves which lane the planner picks, never the value (every lane
 * reconstructs the same exact rational). A negative coefficient leaves that one unchanged. */
extern "C" const char *slate_dag_channel_cost(SlateDag *b, const char *lane, double gmacs, double dispatch_ms) {
  if (!b) return "args";
  int t = Slate::engine::channel_type_of(lane);
  if (t < 0) return "args";
  Slate::engine::ChannelPolicy &cp = b->arena.env().channels;
  if (gmacs >= 0)       cp.gmacs[t] = gmacs;
  if (dispatch_ms >= 0) cp.dispatch_ms[t] = dispatch_ms;
  b->arena.env().config_set |= Slate::Envelope::CFG_CHANNELS;   /* a calibrated channel policy is a local set */
  return nullptr;
}
extern "C" const char *slate_dag_decomp_cost(SlateDag *b, double narrow, double wide) {
  if (!b) return "args";
  Slate::engine::ChannelPolicy &cp = b->arena.env().channels;
  if (narrow >= 0) cp.decomp_ms_per_elem_narrow = narrow;
  if (wide   >= 0) cp.decomp_ms_per_elem_wide   = wide;
  b->arena.env().config_set |= Slate::Envelope::CFG_CHANNELS;
  return nullptr;
}

/* ---- OS-effect surface: grant capabilities, install the host vtable, and build effect nodes. An effect node
 * is resolved to a leaf by effect_eval before dispatch (the floor stays pure); no capability + no host => every
 * effect refuses (a bottom reading), never a wrong value. ---- */
/* The compose grant, through the C door. Each setter calls the one grant body (effect/eval.hpp) that the
 * "slate.compose" effect calls too, so a container's sandbox is declared one way whichever door asked. */
extern "C" const char *slate_dag_effect_caps(SlateDag *b, const char *const *classes, uint32_t n) try {
  if (!b || (!classes && n)) return "args";
  for (uint32_t i = 0; i < n; i++) if (!classes[i] || !*classes[i]) return "args";   /* check first: a refusal changes nothing */
  Slate::Envelope &env = b->arena.env();
  env.effect_caps.clear();                                   /* this call says the whole set, not an addition */
  for (uint32_t i = 0; i < n; i++) Slate::grant_cap(env, classes[i]);
  env.config_set |= Slate::Envelope::CFG_CAPS;
  return nullptr;
} catch (const std::bad_alloc &) { return "nomem"; }
/* Compose grant, mount a volume: expose the sandbox path `prefix` onto the real host path `real` (Docker's -v).
 * `writable` 0 is a read-only mount. A file open is remapped through the covering mount; an unmounted path refuses.
 * This is the config paradigm — the same shape as slate_dag_effect_caps — a .slate compose spec drives it. */
extern "C" const char *slate_dag_mount(SlateDag *b, const char *prefix, const char *real, int writable) {
  if (!b || !prefix || !real) return "args";
  return Slate::grant_mount(b->arena.env(), prefix, real, writable != 0) ? nullptr : "args";
}
/* Compose grant, expose an endpoint: permit a socket provider to reach host:port (Docker's -p). `host` may be null
 * or "" to match any host at that port. `listen` 0 permits a connect (client); nonzero permits a bind (reserved). */
extern "C" const char *slate_dag_expose(SlateDag *b, const char *host, uint16_t port, int listen) {
  if (!b) return "args";
  return Slate::grant_expose(b->arena.env(), host ? host : "", port, listen != 0) ? nullptr : "args";
}
/* Compose grant — a tmpfs mount (Docker's --tmpfs): `prefix` is ram-backed, its files living in this container's
 * scoped MemFs (never a host path, never a process global). `writable` 0 makes it read-only. */
extern "C" const char *slate_dag_tmpfs(SlateDag *b, const char *prefix, int writable) {
  if (!b || !prefix) return "args";
  return Slate::grant_tmpfs(b->arena.env(), prefix, writable != 0) ? nullptr : "args";
}
/* Install the deadline/cancel gate: `proceed(user)` is called at each effect boundary; returning 0 aborts the
 * effect (a clean refusal, not a crash). This is how a wall-clock deadline, a cpu/effect budget, or a cancel of a
 * long-running server loop is enforced without the pure floor ever reading a clock — the embedder owns the policy. */
extern "C" const char *slate_dag_proceed(SlateDag *b, int (*proceed)(void *user), void *user) {
  if (!b) return "args";
  b->arena.env().proceed = proceed;
  b->arena.env().proceed_user = user;
  return nullptr;
}
extern "C" const char *slate_dag_effect_host(SlateDag *b,
    int (*perform)(const char *, const uint8_t *, uint64_t, uint8_t **, uint64_t *, void *),
    int (*begin)(const char *, const uint8_t *, uint64_t, void *, void *), void *user) {
  if (!b) return "args";
  b->effect_host.perform = perform;    /* synchronous; blocks the caller */
  b->effect_host.begin = begin;        /* optional async; parks the fiber, completed via slate_effect_complete */
  b->effect_host.user = user;
  b->arena.env().effect_host = (perform || begin) ? &b->effect_host : nullptr;
  return nullptr;
}
/* Build an effect node: class `cls` (by name), static request params [blob, blob+blen), and `nargs`
 * computed-argument child node ids (forced to values at resolve time). Returns the node id, or -1 on bad args
 * (poisons the builder). */
extern "C" int32_t slate_dag_effect(SlateDag *b, const char *cls, const uint8_t *blob, uint64_t blen,
                                    const int32_t *args, uint32_t nargs) try {
  if (!b || b->err || !cls || !*cls || (!blob && blen) || (!args && nargs)) { if (b) b->err = true; return -1; }
  for (uint32_t i = 0; i < nargs; i++)
    if (!sd_node_ok(b, args[i])) { b->err = true; return -1; }
  return b->build.effect(cls, blob, blen, args, nargs);
} catch (...) { if (b) b->err = true; return -1; }

/* Build an array-effect: the byte/message (consume) form. Performs the effect at dispatch and fills a reserved
 * carrier with `len` byte-cells; read them with slate_dag_load. Returns the carrier id, or UINT32_MAX on error. */
extern "C" uint32_t slate_dag_effect_array(SlateDag *b, const char *cls, const uint8_t *blob, uint64_t blen,
                                           const int32_t *args, uint32_t nargs, int64_t len) try {
  if (!b || b->err || !cls || !*cls || (!blob && blen) || (!args && nargs) || len < 0) { if (b) b->err = true; return UINT32_MAX; }
  for (uint32_t i = 0; i < nargs; i++)
    if (!sd_node_ok(b, args[i])) { b->err = true; return UINT32_MAX; }
  return b->build.effect_array(cls, blob, blen, args, nargs, (long long)len);
} catch (...) { if (b) b->err = true; return UINT32_MAX; }

/* Install the receipt-operand io host. SlateOperand is layout-identical to Slate::IoOperand, so the C callback is
 * stored through a cast into the vtable slot. */
extern "C" const char *slate_dag_codec(SlateDag *b,
    int (*encode)(const uint8_t *, uint64_t, const uint8_t *, uint64_t, uint8_t **, uint64_t *, void *),
    int (*decode)(const uint8_t *, uint64_t, const uint8_t *, uint64_t, uint8_t **, uint64_t *, void *),
    int (*put)(const uint8_t *, uint64_t, const uint8_t *, uint64_t, const uint8_t *, uint64_t, void *),
    const uint8_t *secret, uint64_t sn, void *user) try {
  if (!b || (!secret && sn)) return "args";
  Slate::Codec &c = b->arena.env().codec;
  c.encode = encode; c.decode = decode; c.put = put;
  c.secret = sn ? std::make_shared<const Slate::WBuffer>(Slate::bytes_buffer(secret, (size_t)sn)) : nullptr;
  c.user = user;
  return nullptr;
} catch (const std::bad_alloc &) { return "nomem"; }
extern "C" const char *slate_dag_effect_host_io(SlateDag *b,
    int (*perform)(const char *, const uint8_t *, uint64_t, const SlateOperand *, uint32_t, uint8_t **, uint64_t *, void *),
    void *user) {
  if (!b) return "args";
  b->effect_host.perform_io = reinterpret_cast<decltype(b->effect_host.perform_io)>(perform);
  b->effect_host.user = user;
  b->arena.env().effect_host = &b->effect_host;
  return nullptr;
}

/* Build a receipt-operand io effect: operands are carrier ids passed by reference; fills an out carrier of
 * `out_len`. The out carrier's receipt is the verdict. */
extern "C" uint32_t slate_dag_effect_io(SlateDag *b, const char *cls, const uint8_t *blob, uint64_t blen,
                                        const uint32_t *operand_carriers, uint32_t noperands, int64_t out_len) try {
  if (!b || b->err || !cls || !*cls || (!blob && blen) || (!operand_carriers && noperands) || out_len < 0) {
    if (b) b->err = true; return UINT32_MAX;
  }
  uint32_t ncar = (uint32_t)b->arena.lower_carriers().size();
  for (uint32_t i = 0; i < noperands; i++)
    if (operand_carriers[i] >= ncar) { b->err = true; return UINT32_MAX; }
  return b->build.effect_io(cls, blob, blen, operand_carriers, noperands, (long long)out_len);
} catch (...) { if (b) b->err = true; return UINT32_MAX; }

/* consume: a broker read whose cursor is a word. Reads up to `len` bytes of `handle` after the chunk `cursor`
 * names into a carrier (returned cid, read with slate_dag_load); after slate_dag_run, slate_dag_cursor gives this
 * chunk's word — carry that, not a position. */
extern "C" uint32_t slate_dag_consume(SlateDag *b, const char *cls, const uint8_t *handle, uint64_t hlen,
                                      const uint8_t *cursor, uint64_t cn, int64_t len) try {
  if (!b || b->err || !cls || !*cls || (!handle && hlen) || (!cursor && cn) || len < 0) { if (b) b->err = true; return UINT32_MAX; }
  return b->build.consume(cls, handle, hlen, cursor, cn, (long long)len);
} catch (...) { if (b) b->err = true; return UINT32_MAX; }

/* The cursor Receipt for a consume `cid`, valid after slate_dag_run: out->work is the next offset (the position,
 * carried in provenance), out->closure is 0 Closed (drained — a short read hit the end) or 4 Incomplete (more to
 * read). Persist this Receipt as the commit and re-present out->work as the next consume's offset. */
extern "C" const char *slate_dag_cursor(SlateDag *b, uint32_t cid, slate_cursor *out) try {
  if (!b || !out) return "args";
  const uint8_t *w; size_t wn; const char *closure;
  if (!b->build.cursor(cid, &w, &wn, &closure)) return "refused";   /* not run yet, or not a consume */
  out->word = w; out->wn = wn; out->closure = closure;
  return nullptr;
} catch (...) { return "internal"; }

/* ---- fiber concurrency surface (C): run work on the fiber pool so many async effects overlap on one thread.
 * Inside a `body`/`task` callback you are on a fiber, so slate_dag_run parks on a host's async begin instead of
 * blocking a worker; slate_effect_complete (from any thread) wakes it. This exposes the scheduler to pure C —
 * no C++ headers, no lambdas. ---- */

/* The host's async completion callback (declared in async/effect_bridge.hpp) — a real archive symbol so a
 * pure-C host can link it. Copies the result into the effect's cell and wakes the parked fiber. */
extern "C" void slate_effect_complete(void *slot, const uint8_t *data, uint64_t n) {
  Slate::scope_detail::EffCell *c = static_cast<Slate::scope_detail::EffCell *>(slot);
  if (!c) return;
  if (n && data) {
    c->out = static_cast<uint8_t *>(std::malloc((size_t)n));
    if (c->out) std::memcpy(c->out, data, (size_t)n);
  }
  c->outn = c->out ? n : 0;
  Slate::scope_detail::wake(c);   /* set done + enqueue every waiter (async/park.hpp) */
}

/* Run `body` inside a fiber scope on the pool; blocks the caller until the scope's spawned work joins. Inside
 * `body` you hold a SlateScope* to spawn concurrent tasks. */
extern "C" void slate_scope_run(void (*body)(SlateScope *scope, void *user), void *user) try {
  if (!body) return;
  Slate::run_scope([&](Slate::Scope s) -> int {
    body(reinterpret_cast<SlateScope *>(&s), user);
    return 0;
  });
} catch (...) { /* no exception crosses the C boundary */ }

/* Spawn `task` as a concurrent fiber in `scope`; the scope joins it before slate_scope_run returns. Each task
 * runs on the pool, so its slate_dag_run parks (not blocks) on async effects, overlapping the other tasks. */
extern "C" void slate_scope_spawn(SlateScope *scope, void (*task)(void *user), void *user) try {
  if (!scope || !task) return;
  Slate::Scope *s = reinterpret_cast<Slate::Scope *>(scope);
  s->spawn([task, user]() { task(user); });
} catch (...) {}

/* Source-in for a reader: raw bytes as their unsigned value, packed at the min width (int8 for a byte
 * ≤127, int16 for 0-255) with one bulk copy — no int64 vector, no per-value pack. This is the fast
 * text-source path (a reader's source is bytes).
 *
 * Not the same registration as slate_dag_carrier below: this one packs unsigned byte values (0..255) straight
 * into a WBuffer by hand for speed; that one hands signed int64 cells to DagBuild::carrier, which builds its
 * own carrier from the full int64 domain. Different input domains, so this is its own body, not a caller of
 * that one (or the reverse). */
static std::shared_ptr<Slate::WBuffer> wbuf_from_bytes(const uint8_t *by, uint64_t n) {
  uint8_t mx = 0;
  for (uint64_t i = 0; i < n; i++) if (by[i] > mx) mx = by[i];
  auto b = std::make_shared<Slate::WBuffer>((size_t)n, mx <= 127 ? 1 : 2);
  if (mx <= 127) std::memcpy(b->plane(), by, (size_t)n);                       /* int8: direct copy */
  else { int16_t *d = (int16_t *)b->plane(); for (uint64_t i = 0; i < n; i++) d[i] = (int16_t)by[i]; }
  b->set_bound((int64_t)mx, false);   /* the true value bound (a raw plane write leaves it width-implied) */
  return b;
}
extern "C" uint32_t slate_dag_carrier_bytes(SlateDag *b, const uint8_t *bytes, uint64_t n) try {
  if (!b || (!bytes && n)) return UINT32_MAX;
  return b->build.a.lower_register_carrier_buf(wbuf_from_bytes(bytes, n));
} catch (...) { return UINT32_MAX; }
/* The U/E split: replace carrier `cid`'s data in place, then re-run the same construction over the new
 * source with no rebuild and no recompile (the compiled leaf is carrier-independent). This is the fast
 * per-source dispatch — compile the reader once, feed it many sources.
 *
 * One body, two doors: the guard and the refusal names are the same; the doors differ only in how the new
 * source is built (raw bytes, or int64 cells). */
template <class Swap>
static const char *dag_carrier_swap(SlateDag *b, Swap swap) try {
  if (!b) return "args";
  swap();
  return nullptr;
} catch (const std::bad_alloc &) { return "nomem"; }
  catch (...) { return "internal"; }

extern "C" const char *slate_dag_carrier_set_bytes(SlateDag *b, uint32_t cid, const uint8_t *bytes, uint64_t n) {
  if (!bytes && n) return "args";
  return dag_carrier_swap(b, [&] { b->build.a.lower_set_carrier_buf(cid, wbuf_from_bytes(bytes, n)); });
}
extern "C" const char *slate_dag_carrier_set(SlateDag *b, uint32_t cid, const int64_t *vals, uint64_t n) {
  if (!vals && n) return "args";
  return dag_carrier_swap(b, [&] { b->build.a.lower_set_carrier(cid, std::vector<int64_t>(vals, vals + n)); });
}

/* There is no door here that stops a construction to bytes and reads it back. Every step is a row already:
 * the word of a step is the name of what it computed, so where a construction has got to is what its store
 * holds under its words, and the root's word is the whole of it. The durable, position-independent token a
 * caller carries is the root's fragment: keep it with slate_dag_save_fragment (which hands back its word), and
 * recover it with slate_frag_load + slate_dag_splice, which replays the construction into a
 * fresh builder and returns a new root id. A raw int32 node id is a within-build positional handle, never
 * identity across time. The token addresses the op1-5 (add/sub/mul/mulh/asr) plain-positional-integer-carrier
 * subset only; a root reached through an RNS/ℚ (carrier_q), f32, or width>8 carrier must declare that carrier
 * a splice hole (re-supplied at splice) or keep the id path — such carriers are not fragment-addressable. */

extern "C" int32_t slate_dag_param(SlateDag *b, uint32_t slot) try {
  if (!b || b->err) { if (b) b->err = true; return -1; }
  return b->build.param(slot);
} catch (...) { if (b) b->err = true; return -1; }

extern "C" int32_t slate_dag_lit(SlateDag *b, int64_t v) try {
  if (!b || b->err) { if (b) b->err = true; return -1; }
  return b->build.lit((long long)v);
} catch (...) { if (b) b->err = true; return -1; }

/* Reflection surface: read one node's structure off the graph (the engine's own lower_view). A fragment spliced
 * into a builder is a graph of these; walking them (0..root) recovers the exact construction — op + child ids +
 * leaf kind/value — so a caller can see what a fragment is composed of, structurally, with no collision. kind:
 * 0 op, 1 shard, 2 lit, 3 carrier, 4 big. For an op, `op` is the opcode and a/b are child node ids (-1 absent);
 * for a lit, lit_n/lit_d are the value. Any out may be NULL. */
/* the names of a lowered node's kind and op — what lower_view holds, said */
static const char *low_kind_name(int k) {
  static const char *const names[5] = { "op", "param", "lit", "carrier", "big" };
  return (k >= 0 && k < 5) ? names[k] : "op";
}
extern "C" const char *slate_dag_node(SlateDag *b, int32_t id, const char **kind, const char **op,
                              int32_t *a, int32_t *bchild, int64_t *lit_n, int64_t *lit_d) try {
  if (!b || !b->build.a.real_valid(id)) return "args";
  Slate::Arena::LowNode v = b->build.a.lower_view(id);
  if (kind) *kind = low_kind_name(v.kind);
  if (op) *op = v.kind == 0 ? slate_op_name(v.op) : nullptr;
  if (a) *a = v.a;
  if (bchild) *bchild = v.b;
  if (lit_n) *lit_n = v.lit_n;
  if (lit_d) *lit_d = (int64_t)v.lit_d;
  return nullptr;
} catch (...) { return "internal"; }

/* Register signed int64 cells as a carrier — see wbuf_from_bytes above for why this is not the same body as
 * the bytes registration (slate_dag_carrier_bytes): unsigned bytes vs signed int64 cells, two domains. */
extern "C" uint32_t slate_dag_carrier(SlateDag *b, const int64_t *vals, uint64_t n) try {
  if (!b || b->err || (!vals && n)) { if (b) b->err = true; return UINT32_MAX; }
  return b->build.carrier(std::vector<int64_t>(vals, vals + n));
} catch (...) { if (b) b->err = true; return UINT32_MAX; }

extern "C" uint32_t slate_dag_carrier_q(SlateDag *b, const int64_t *nums, uint64_t n,
                                        int64_t den, int32_t hbits) try {
  if (!b || b->err || (!nums && n) || den == 0 || hbits < 0) { if (b) b->err = true; return UINT32_MAX; }
  /* a common-denominator rational operand: cell i = nums[i]/den, folded into residues provisioned to the
   * whole computation's height `hbits` (see WBuffer::rns_rational / bench/gemm_frac.cpp). run() scopes a
   * Potential to the declared height so the dispatched result provisions enough channels to Wang-recover
   * num/den. The height a carrier declares is the same axis the Potential knob sets, so it is raised there —
   * one declared height on the Envelope, not a second copy remembered beside it. */
  auto buf = std::make_shared<Slate::WBuffer>(
      Slate::WBuffer::rns_rational(std::vector<int64_t>(nums, nums + n), den, hbits));
  unsigned cid = b->build.carrier(std::static_pointer_cast<const Slate::WBuffer>(buf));
  if ((long long)hbits > b->arena.env().height_bits) b->arena.env().height_bits = (long long)hbits;
  return cid;
} catch (...) { if (b) b->err = true; return UINT32_MAX; }

extern "C" int32_t slate_dag_load(SlateDag *b, uint32_t cid, int32_t idx) try {
  if (!sd_node_ok(b, idx) || cid >= b->arena.lower_carriers().size()) { if (b) b->err = true; return -1; }
  return b->build.load(cid, idx);
} catch (...) { if (b) b->err = true; return -1; }

extern "C" int32_t slate_dag_add(SlateDag *b, int32_t x, int32_t y) try {
  if (!sd_node_ok(b, x) || !sd_node_ok(b, y)) { if (b) b->err = true; return -1; }
  return b->build.add(x, y);
} catch (...) { if (b) b->err = true; return -1; }

extern "C" int32_t slate_dag_mul(SlateDag *b, int32_t x, int32_t y) try {
  if (!sd_node_ok(b, x) || !sd_node_ok(b, y)) { if (b) b->err = true; return -1; }
  return b->build.mul(x, y);
} catch (...) { if (b) b->err = true; return -1; }
extern "C" int32_t slate_dag_div(SlateDag *b, int32_t x, int32_t y) try {
  if (!sd_node_ok(b, x) || !sd_node_ok(b, y)) { if (b) b->err = true; return -1; }
  return b->build.div(x, y);
} catch (...) { if (b) b->err = true; return -1; }

extern "C" int32_t slate_dag_sub(SlateDag *b, int32_t x, int32_t y) try {
  if (!sd_node_ok(b, x) || !sd_node_ok(b, y)) { if (b) b->err = true; return -1; }
  return b->build.sub(x, y);
} catch (...) { if (b) b->err = true; return -1; }

extern "C" int32_t slate_dag_mulh(SlateDag *b, int32_t x, int32_t y) try {
  if (!sd_node_ok(b, x) || !sd_node_ok(b, y)) { if (b) b->err = true; return -1; }
  return b->build.mulh(x, y);
} catch (...) { if (b) b->err = true; return -1; }

extern "C" int32_t slate_dag_asr(SlateDag *b, int32_t x, int32_t sh) try {
  if (!sd_node_ok(b, x) || !sd_node_ok(b, sh)) { if (b) b->err = true; return -1; }
  return b->build.asr(x, sh);
} catch (...) { if (b) b->err = true; return -1; }

/* The run trampoline. `cur` is the name of the program to run first (empty: the dispatch already ran, `ar` is
 * its reading, and the loop only continues if it handed off). If the dispatch handed off via "slate.tail", resolve
 * the next program's name through this container's store and run what it names in a fresh arena that shares this
 * container (grant / fds / providers), and loop. Each hand-off is a finite dispatch, so a cycle of finite programs
 * (a service) is a chain of finite dispatches — no native stack, no growing graph, the container (e.g. a listening
 * fd) carried across. A program is a leaf: the slot holds its word, the store holds its byte cells, and a
 * fleet of ticks all read one program with nothing shipped between them. A word the store does not hold — or
 * one whose leaf is not whole yet — ends the service with a refusal, never a wrong tick. The tick writes its own hand-off back to this
 * container's tail slot, which this loop reads; a tick that hands off to nothing ends the trampoline. The result
 * buffer owns its cells (outlives the tick builder), so each tick's builder is freed — nothing accumulates. */
static Slate::ArrayReading run_ticks(SlateDag *b, std::vector<uint8_t> cur, const std::vector<long long> &dv,
                                     Slate::ArrayReading ar) {
  while (b->tail_flag_buf) {
    b->tail_flag_buf = false;
    if (!b->tail_prog_buf.empty()) { cur.swap(b->tail_prog_buf); b->tail_prog_buf.clear(); }   /* hand off to a new program */
    else if (cur.empty()) break;               /* a repeat-tail before any named hand-off: nothing to repeat */
    /* else: tail_prog empty + cur set = a repeat-tail — run the same program again (a finite program that loops) */
    if (cur.empty()) return Slate::ArrayReading{};   /* a word of no bytes names nothing */
    SlateDag *t = new (std::nothrow) SlateDag();
    if (!t) return Slate::ArrayReading{};
    Slate::Envelope &te = t->arena.env(), &be = b->arena.env();
    /* Inherit the container's whole scope by value — config (Potential, budget, executor, telemetry, channels,
     * caps, deadline gate) plus the shared container bindings (grant/fds/providers/host) plus the tail slot —
     * instead of the old hand-picked subset that silently dropped Potential/budget/executor across a hand-off.
     * te keeps its own frag_self (owner check); a tick may still override any axis locally, scoped to itself. */
    te.inherit_from(be);
    /* the program's leaf, walked out of this container's store by its word — the store is the memory */
    SlateFrag *f = slate_frag_load(t, cur.data(), (uint64_t)cur.size());
    if (!f) { delete t; return Slate::ArrayReading{}; }
    int32_t tr = -1; slate_dag_splice(t, f, nullptr, 0, &tr);
    slate_frag_free(f);
    if (tr < 0) { delete t; return Slate::ArrayReading{}; }
    { Slate::CtxScope tcs(t->arena); ar = t->build.run(tr, dv); }
    delete t;                                 /* free the tick's arena — the result buffer already owns its cells */
  }
  return ar;
}

/* what both entries set up: the builder's own Envelope active for the dispatch, the device scope, the Potential */
struct RunScope {
  Slate::CtxScope cs;
  std::optional<Slate::engine::DeviceScope> dev;
  std::optional<Slate::Potential> pot;
  explicit RunScope(SlateDag *b) : cs(b->arena) {
    if (b->want_device) dev.emplace();
    /* the result height is the declared Potential — the knob's, raised by any rational (RNS) carrier that needs
     * more channels to reconstruct. A pure-int64 build with nothing declared leaves this 0 and skips the guard. */
    long long h = b->arena.env().height_bits;
    if (h > 0) pot.emplace(h);
  }
};
/* The dims of this dispatch, recorded on the container as it goes — they are part of what an ask carries, and
 * a container has nowhere else to keep them. `recall`: with no dims named, start over the ones the container
 * already carries (an ask's dims, or the last dispatch's); a container that carries none has no grid to run.
 * A run of a named root never recalls — its dims are the caller's, as they have always been. */
static bool run_dims(SlateDag *b, bool recall, const int64_t *dims, uint32_t ndims, std::vector<long long> &dv) {
  if (recall && !dims && !ndims) {
    if (b->dims_buf.empty()) return false;
    dv.assign(b->dims_buf.begin(), b->dims_buf.end());
    return true;
  }
  if ((!dims && ndims) || !ndims) return false;
  dv.resize(ndims);
  for (uint32_t i = 0; i < ndims; i++) { if (dims[i] < 0) return false; dv[i] = (long long)dims[i]; }
  b->dims_buf.assign(dims, dims + ndims);   /* what this container last ran over: the ask's dims */
  return true;
}

/* Keep the construction rooted at `root` as a leaf, and name it: the graph's bytes through the same door as any
 * bytes handed in — one cell per byte under the word of (the lens in the clear ‖ the bytes), exactly as the emit
 * door keeps one — and that word set as this container's program. A machine that takes this
 * container's ask then has a name for the construction and needs none of its bytes: with a roster the leaf is
 * sliced across the carrying machines by the door in front of the store, and gathered whole by anyone who asks
 * for the word. A graph that is not fragment-addressable (an RNS/ℚ, f32 or wide carrier — see
 * slate_dag_save_fragment), or a store that kept nothing, leaves the run unaffected and the ask naming no
 * program. */
static void keep_program_leaf(SlateDag *b, int32_t root) {
  uint8_t *w = nullptr;
  uint64_t wn = 0;
  if (slate_dag_save_fragment(b, root, nullptr, 0, &w, &wn) != nullptr) return;
  b->arena.env().program.assign(w, w + wn);
  std::free(w);
}

/* One body for both dispatch doors: the builder's own scope, the first step, then the trampoline. `root` >= 0
 * runs that root first; `root` < 0 starts the program the context names — the first step is a hand-off to that
 * word, and from there it is the same trampoline as a run that handed off. */
static SlateArray *dag_dispatch(SlateDag *b, int32_t root, const int64_t *dims, uint32_t ndims) {
  std::vector<long long> dv;
  if (!b || !run_dims(b, root < 0, dims, ndims, dv)) return nullptr;
  if (root >= 0 && !b->arena.env().channels.roster.empty() && b->arena.env().program.empty())
    keep_program_leaf(b, root);                /* a container carrying a roster names its construction first */
  if (root < 0 && b->arena.env().program.empty()) return nullptr;   /* no program: nothing to start */
  RunScope scope(b);
  Slate::ArrayReading first;
  if (root >= 0) {
    first = b->build.run(root, dv);
  } else {
    b->tail_flag_buf = true;                                /* the first step is a hand-off to the configured word */
    b->tail_prog_buf = b->arena.env().program;
  }
  Slate::ArrayReading ar = run_ticks(b, {}, dv, std::move(first));
  if (!ar.buffer()) return nullptr;            /* refused dispatch (non-lowerable / >int64): no wrong value */
  return new SlateArray{std::move(ar), b->arena.env().codec};
}

extern "C" SlateArray *slate_dag_run(SlateDag *b, int32_t root,
                                     const int64_t *dims, uint32_t ndims) try {
  if (!sd_node_ok(b, root)) return nullptr;
  return dag_dispatch(b, root, dims, ndims);
} catch (...) {
  return nullptr;
}

/* start: run the program the context names. No graph is built here — which row runs is context, set by
 * slate_dag_program, the way the lens and the store are. */
extern "C" SlateArray *slate_dag_start(SlateDag *b, const int64_t *dims, uint32_t ndims) try {
  return dag_dispatch(b, -1, dims, ndims);
} catch (...) {
  return nullptr;
}

extern "C" const char *slate_dag_program(SlateDag *b, const uint8_t *word, uint64_t n) {
  if (!b || (n && !word)) return "args";
  b->arena.env().program.assign(word, word + n);
  return nullptr;
}

/* ---- the ask: a container's run context as bytes, so another machine runs the same construction on its own
 * share. Host's own format, little-endian, fixed order and byte exact (embed.h spells it):
 *
 *     [k u32][roster prime i64] * k
 *     [ndims u32][dim i64] * ndims [wn u64][program word u8] * wn
 *
 * Nothing rides in front of it: no tag byte, no version byte. The construction does not travel — only its word
 * does. The program is a leaf like any bytes handed in, so with a roster it is already sliced across the
 * carrying machines and the taker walks it back through the same door it reads any other row through. There is
 * no count beside the word: the taker walks the cells to the first one nobody has, and the records are the
 * count. `wn` 0 is no program. Which share the taker carries is the taker's own primes, handed to
 * slate_dag_take_ask beside the ask. The division — how many units the roster is cut into — is not here and
 * never was in a word: it is the deployment's rule, the same on every unit, so re-dividing a roster leaves
 * every word it ever named unchanged.
 *
 * The ask itself travels the same way: these bytes are kept as a leaf on the roster lens, exactly as the
 * construction is (leaf_put, one cell per byte under the word of the lens in the clear ‖ the bytes), and what
 * is handed back is the ask's word. The bytes never cross a door — an Interest for an ask's word is the one
 * packet that says "run this". ---- */
static void ask_put_u32(std::vector<uint8_t> &b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}
static void ask_put_u64(std::vector<uint8_t> &b, uint64_t v) {
  for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i)));
}
/* the reader side: every read is bounds-checked against the ask's end, so a short or malformed ask refuses
 * rather than reading past it. */
struct AskRd {
  const uint8_t *p; uint64_t n, cur;
  bool u32v(uint32_t &v) { if (cur + 4 > n) return false; v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[cur + (uint64_t)i] << (8 * i); cur += 4; return true; }
  bool u64v(uint64_t &v) { if (cur + 8 > n) return false; v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[cur + (uint64_t)i] << (8 * i); cur += 8; return true; }
  bool i64v(int64_t &v) { uint64_t u = 0; if (!u64v(u)) return false; v = (int64_t)u; return true; }
};

extern "C" int slate_dag_ask(SlateDag *b, uint8_t **word, uint64_t *wn_out) try {
  if (!b || !word || !wn_out) return 1;
  const Slate::Envelope &e = b->arena.env();
  const std::vector<int64_t> &roster = e.channels.roster;
  /* the program's word, as it stands: nothing rides beside it */
  const uint8_t *w = e.program.data();
  const size_t wn = e.program.size();
  std::vector<uint8_t> a;
  ask_put_u32(a, (uint32_t)roster.size());
  for (int64_t p : roster) ask_put_u64(a, (uint64_t)p);
  ask_put_u32(a, (uint32_t)b->dims_buf.size());
  for (int64_t d : b->dims_buf) ask_put_u64(a, (uint64_t)d);
  ask_put_u64(a, (uint64_t)wn);
  a.insert(a.end(), w, w + wn);
  /* the ask goes through the door like every other bytes handed in: one cell per byte, under the word of the
     lens in the clear ‖ the bytes, on the roster lens when this container carries a share of one. The word is
     all that leaves — an Interest for it is "run this ask". A store that kept nothing refuses. */
  const Slate::Word key = b->arena.leaf_key(a.data(), a.size());
  if (key.empty() || !Slate::leaf_put(b->arena.store_env(), b->arena.env().codec, key, a.data(), a.size()))
    return 1;
  uint8_t *o = (uint8_t *)std::malloc(key.size());
  if (!o) return 1;
  std::memcpy(o, key.data(), key.size());
  *word = o; *wn_out = (uint64_t)key.size();
  return 0;
} catch (...) { return 1; }

extern "C" const char *slate_dag_take_ask(SlateDag *b, const uint8_t *word, uint64_t wn,
                                          const int64_t *primes, uint32_t k) try {
  if (!b || !word || !wn || (k && !primes)) return "args";
  /* the ask is a leaf: walk its cells out of this container's store — word ‖ 0, word ‖ 1, … to the first cell
     nobody has, the records are the count. Nothing under the word is not an ask; a leaf the door could only
     half gather is not one yet, and says so rather than taking a short ask. */
  std::vector<uint8_t> ask;
  bool partial = false;
  if (!Slate::leaf_get(b->arena.store_env(), b->arena.env().codec, Slate::Word(word, word + wn), ask, partial))
    return partial ? "partial" : "args";
  const uint64_t n = (uint64_t)ask.size();
  AskRd r{ ask.data(), n, 0 };
  uint32_t rk = 0, ndims = 0;
  if (!r.u32v(rk)) return "args";
  std::vector<int64_t> roster(rk);
  for (uint32_t i = 0; i < rk; i++) if (!r.i64v(roster[i])) return "args";
  if (!r.u32v(ndims)) return "args";
  std::vector<int64_t> dims(ndims);
  for (uint32_t i = 0; i < ndims; i++) { if (!r.i64v(dims[i]) || dims[i] < 0) return "args"; }
  uint64_t pwn = 0;
  if (!r.u64v(pwn) || r.cur + pwn > r.n) return "args";
  const uint8_t *prog = ask.data() + r.cur;
  r.cur += pwn;
  /* the roster first, then this machine's own share of it, then what to run and over what. The lens is cleared
     first so an ask always lands on a fresh statement — a container's old primes are not a reason to refuse
     the roster its ask names. */
  b->arena.env().channels.primes.clear();
  if (const char *rc = slate_dag_roster(b, rk ? roster.data() : nullptr, rk)) return rc;
  if (const char *rc = slate_dag_lens(b, k ? primes : nullptr, k)) return rc;
  /* the construction does not travel: the ask names it by its word, and this container walks the leaf out of
     its own store through the same door as any other row — with a roster, the share it carries, gathered whole
     by the door. */
  if (const char *rc = slate_dag_program(b, pwn ? prog : nullptr, pwn)) return rc;
  b->dims_buf = std::move(dims);
  return nullptr;
} catch (...) { return "internal"; }

/* ---- run an ask by its word: take it, start it, and say what the root is ----
 *
 * This is the unit's whole answer to an Interest for an ask's word: an Interest for a word that names an ask
 * is "run it". Take the ask (the leaf walked out of this container's store), start the program it names over
 * the dims it carries, and hand back the reading's own word — the key the root's per-cell rows are kept under,
 * so whoever asked can walk them.
 *
 * Whether the root is whole is read off the store, never remembered: cell 0's row through the container's own
 * codec — the door in front of the store. The row (0) means every share of that cell has landed and the root is
 * whole; not-whole (2) means this unit's share is there and another carrier's is not — the stopped state, which
 * is "share", not a failure; nothing anywhere (1) is "share" too when this unit carries one share of a roster
 * (its own share landed through the same door the gather now reports nothing for), and otherwise a store that
 * kept nothing: the refusal. */
extern "C" const char *slate_dag_run_ask(SlateDag *b, const uint8_t *word, uint64_t wn,
                                         const int64_t *primes, uint32_t k,
                                         uint8_t **root, uint64_t *rn) try {
  if (!b || !word || !wn || (k && !primes) || !root || !rn) return "args";
  *root = nullptr; *rn = 0;
  if (const char *rc = slate_dag_take_ask(b, word, wn, primes, k)) return rc;
  SlateArray *a = slate_dag_start(b, nullptr, 0);
  if (!a) return "refused";                      /* no program, no leaf, or a tick refused: no wrong reading */
  uint8_t *rw = nullptr; uint64_t rwn = 0;
  if (const char *rc = slate_array_word(a, &rw, &rwn)) { slate_array_free(a); return rc; }
  uint8_t *cw = nullptr; uint64_t cwn = 0;
  if (const char *rc = slate_array_cell_word(a, 0, &cw, &cwn)) { std::free(rw); slate_array_free(a); return rc; }
  std::vector<uint8_t> row;
  const int rc0 = Slate::store_get_rc(a->codec, cw, (size_t)cwn, row);
  std::free(cw);
  const Slate::Envelope &e = b->arena.env();
  /* this unit carries one share of a roster someone else also carries: its lens is a proper run of the roster,
     so there are other carriers whose rows the gather is reporting nothing for. The division itself is never
     read here — the primes say it. */
  const bool carries_share = !e.channels.primes.empty() && e.channels.primes.size() < e.channels.roster.size();
  slate_array_free(a);
  if (rc0 != 0 && rc0 != 2 && !(rc0 == 1 && carries_share)) { std::free(rw); return "refused"; }
  *root = rw; *rn = rwn;                         /* on a reading, the root's word — whole or stopped */
  return rc0 == 0 ? nullptr : "share";
} catch (...) { return "internal"; }

extern "C" uint64_t slate_array_size(const SlateArray *a) {
  return a ? (uint64_t)a->ar.size() : 0;
}

/* The reading's own word: the key its per-cell rows are kept under (cells_put spells each cell
 * word_cat(key, word_i64(i))). This is the name a run hands back so another machine can walk the root cell by
 * cell — the whole reading under one word, not one cell of it. */
extern "C" const char *slate_array_word(const SlateArray *a, uint8_t **out, uint64_t *n) try {
  if (!a || !out || !n) return "args";
  const Slate::WBuffer &w = a->ar.word();
  const size_t wn = Slate::plane_bytes(w);
  if (!wn) return "args";                       /* a refused dispatch carries no word */
  uint8_t *o = (uint8_t *)std::malloc(wn);
  if (!o) return "internal";
  std::memcpy(o, (const uint8_t *)w.raw_plane(), wn);
  *out = o; *n = (uint64_t)wn;
  return nullptr;
} catch (...) { return "internal"; }

/* The word of one cell of a reading: the reading's own word with the cell's index after it — the same name the
 * engine keeps that cell's row under (cells_put spells it word_cat(key, word_i64(i))), through the same codec
 * the dispatch ran with. A deployment hands it to the door in front of the store to ask whether the root is
 * whole; nothing here asks the engine anything. */
extern "C" const char *slate_array_cell_word(const SlateArray *a, uint64_t i, uint8_t **out, uint64_t *n) try {
  if (!a || !out || !n) return "args";
  const Slate::WBuffer &w = a->ar.word();
  const size_t wn = Slate::plane_bytes(w);
  if (!wn) return "args";                       /* a refused dispatch carries no word */
  const Slate::Word key((const uint8_t *)w.raw_plane(), (const uint8_t *)w.raw_plane() + wn);
  const Slate::Word iw = Slate::word_i64(a->codec, (int64_t)i);
  const Slate::Word ki = Slate::word_cat(a->codec, { &key, &iw });
  if (ki.empty()) return "args";
  uint8_t *o = (uint8_t *)std::malloc(ki.size());
  if (!o) return "internal";
  std::memcpy(o, ki.data(), ki.size());
  *out = o; *n = (uint64_t)ki.size();
  return nullptr;
} catch (...) { return "internal"; }

/* The result's reading — the certification carried with the result, no cell values pulled. The primary
 * way to consume a result: read the frame, keep the values resident (chain another build, or persist with
 * keep the values resident). Each entry is what the frame holds, by name; a refused dispatch has no valued domain, so
 * the domain entry is absent there. */
extern "C" const char *slate_array_receipt(const SlateArray *a, const slate_entry **entries, int32_t *n) try {
  if (!a || !entries || !n) return "args";
  const Slate::Receipt &f = a->ar.frame();
  int32_t k = 0;
  a->entries[k++] = slate_entry_text("verdict", slate_verdict_name((int32_t)f.verdict));
  /* Domain from the result's representation, which is tighter than the frame's coarse classification: a
     positional / float buffer holds two's-complement integers (den==1 by construction) → ℤ; an RNS buffer is
     the rational tier → ℚ. This agrees with the fragment's static iface (the join of operand domains) instead
     of always reporting ℚ. */
  if (f.has_value()) a->entries[k++] = slate_entry_text("domain", (a->ar.buffer() && a->ar.buffer()->is_rns()) ? "Q" : "Z");
  a->entries[k++] = slate_entry_text("path", slate_path_name((int32_t)f.path));
  a->entries[k++] = slate_entry_text("mode", slate_mode_name((int32_t)f.mode));
  a->entries[k++] = slate_entry_text("closure", slate_closure_name((int32_t)f.closure));
  a->entries[k++] = slate_entry_text("evidence", slate_evidence_name((int32_t)f.evidence));
  a->entries[k++] = slate_entry_text("outcome", slate_outcome_name((int32_t)f.outcome));
  a->height = f.h_bits;
  a->entries[k++] = SLATE_ENTRY("height", a->height, "bits:u64");
  *entries = a->entries; *n = k;
  return nullptr;
} catch (...) { return "internal"; }

/* The int64 readback, one body: a reading is a sign and two binaries; this reads that pair back as int64 or
   refuses by name. INT64_MIN = -2^63: its magnitude sits at the wide-value ceiling and has no positive int64
   form, yet the signed output holds it — so it is handed back directly (never -(int64_t)2^63, which is
   undefined). Anything wider refuses rather than reading a wrong value. Every door that pulls an int64 out of
   a reading (the array door, the search door) comes here. */
static const char *reading_i64(const Slate::Receipt &r, int64_t *num, int64_t *den) {
  if (!r.has_value()) return "refused";
  const uint64_t TOP = (uint64_t)1 << 63;
  if (r.num.size() > 1 || r.den.size() > 1) return "wide";
  uint64_t nmag = r.num.empty() ? 0 : r.num[0];
  uint64_t dmag = r.den.empty() ? 1 : r.den[0];
  if (r.sign && nmag == TOP && dmag == 1) { *num = INT64_MIN; *den = 1; return nullptr; }
  if (nmag >= TOP || dmag >= TOP) return "wide";
  int64_t nn = (int64_t)nmag;
  *num = r.sign ? -nn : nn;
  *den = (int64_t)(dmag ? dmag : 1);
  return nullptr;
}

/* Unsafe per-cell int64 pull: reconstructs a value per cell and drops the receipt (the line-by-line
 * anti-pattern). Prefer slate_array_receipt + keeping the result resident; extract a final answer in bulk
 * via slate_array_records. Kept for host FFIs that genuinely need a raw int lane. */
extern "C" const char *slate_array_i64_unsafe(const SlateArray *a, int64_t *num, int64_t *den) try {
  if (!a || !num || !den) return "args";
  const size_t n = a->ar.size();
  for (size_t i = 0; i < n; i++)
    if (const char *e = reading_i64(a->ar.cell(i), num + i, den + i)) return e;
  return nullptr;
} catch (const std::bad_alloc &) { return "nomem"; }
  catch (...) { return "internal"; }

extern "C" const char *slate_array_records(const SlateArray *a, uint64_t *out,
                                   uint64_t out_bytes, uint64_t *out_stride) try {
  if (!a) return "args";                 /* `out` may be NULL: a stride-only probe (see below) */
  const size_t n = a->ar.size();
  std::vector<Slate::Receipt> recs;
  recs.reserve(n);
  size_t L = 1;
  for (size_t i = 0; i < n; i++) {
    Slate::Receipt r = a->ar.cell(i);
    if (!r.has_value()) return "refused";
    if (r.num.size() > L) L = r.num.size();
    if (r.den.size() > L) L = r.den.size();
    recs.push_back(std::move(r));
  }
  const size_t stride = 3 + 2 * L;
  if (out_stride) *out_stride = stride;
  /* stride-only probe (out == NULL) or an undersized buffer both return EOUTSIZE with *out_stride set — size
     then fill, matching slate_frag_iface which accepts NULL for a count-only probe. */
  if (!out || out_bytes < (uint64_t)n * stride * 8) return "outsize";
  std::memset(out, 0, (size_t)n * stride * 8);
  for (size_t s = 0; s < n; s++) {
    const Slate::Receipt &r = recs[s];
    uint64_t *o = out + s * stride;
    o[0] = 1;
    o[1] = (uint64_t)(r.sign && !(r.num.size() == 1 && r.num[0] == 0) ? 1 : 0);
    o[2] = L;
    for (size_t i = 0; i < r.num.size(); i++) o[3 + i] = r.num[i];
    for (size_t i = 0; i < r.den.size(); i++) o[3 + L + i] = r.den[i];
  }
  return nullptr;
} catch (const std::bad_alloc &) { return "nomem"; }
  catch (...) { return "internal"; }

extern "C" void slate_array_free(SlateArray *a) {
  delete a;
}

/* ------------------------------------------------------------------------------------------------------------
 * Fragment libraries: a fragment is a serialized DAG piece with typed holes, kept as a leaf. save_fragment
 * records the sub-DAG rooted at a node as normalized builder instructions (the portable source form — never
 * lowered nodes) plus a carrier table (a hole carries only a receipt; a baked constant carries its int64
 * cells) and a crc, keeps those bytes as byte cells under their own word, and hands back that word — the
 * whole name; load walks that leaf back, cell 0, cell 1, … to the first cell nobody has, and validates it
 * fully before the fragment is usable; splice replays it into another builder, so node ids remap and receipts
 * compose at the seam (receipt-in / receipt-out). There is no file and no stream, and nothing outside the
 * bytes says how many there are: the store's word already says what the bytes are, so they carry no marker of
 * their own — only the counts the parser reads them by, and a crc over the body, which is what catches a walk
 * that a lost cell cut short.
 * ---------------------------------------------------------------------------------------------------------- */
namespace {

constexpr uint64_t kFragMaxCount = (uint64_t)1 << 30; /* sanity cap on instr / carrier / param / data counts */
/* the features a fragment may declare (its header lists the names it uses; a name this engine does not know refuses
   the load): "division" (mulh/asr instrs), "effects" (scalar OS-effect nodes: a trailing effect table),
   "effect-arrays" (byte / io effect carriers: a trailing effect-array table). */
static const char *const kFragFeatures[] = { "division", "effects", "effect-arrays", "rational" };
/* the domain a name stands for: "" -1 (derive), "N" 0 … "C" 4; -2 for a name that is not a domain */
static int frag_domain_of(const std::string &s) {
  if (s.empty()) return -1;
  for (int d = 0; d < 5; d++) if (s == slate_domain_name(d)) return d;
  return -2;
}
constexpr uint64_t kFragBlobMax = (uint64_t)1 << 30;  /* sanity cap on one effect's static request blob */
constexpr uint32_t kFragNameMax = 1u << 12;           /* sanity cap on a class name */

/* one normalized builder instruction (a fragment replays these). kind: 0 param(aux=slot) 1 lit(imm=value)
   2 load(aux=carrier-table idx, a=index instr) 3 op(aux=opcode 1..5, a,b=child instrs) 4 effect(aux=effect-table idx).
   An effect makes the fragment a pure function of (graph, journal) rather than of the graph alone — it carries
   flags bit2, so an engine that does not know effects rejects it (unknown flag) instead of misreading it. */
struct FragInstr { uint32_t kind; uint32_t aux; int32_t a; int32_t b; int64_t imm; };

/* a fragment effect node: its class, static request blob, and the instr indices of its computed args. Held in a
   side table (like carriers) because it is variable-length; the effect table is a trailing section present iff
   flags bit2, so a pure fragment is byte-identical to a v1 fragment with no effects. */
struct FragEffect { std::string cls; std::vector<uint8_t> blob; std::vector<int32_t> args; };

/* a fragment effect-array carrier: the byte broker (effect_array) or receipt-operand io (effect_io). The named
   carrier's cells are filled at run() by performing class `cls` with `blob` into `len` cells. Byte form → args
   (instr indices); io form → operand carriers (by reference). Trailing table, present iff flags bit3. */
struct FragEffectArray {
  uint32_t carrier;              /* carrier-table index this effect fills */
  std::string cls;
  int32_t consume = 0;           /* a consume: `cursor` is the word of the chunk before it (empty = the start) */
  std::vector<uint8_t> cursor;
  int64_t  len;
  std::vector<uint8_t> blob;
  std::vector<int32_t> args;                 /* byte form */
  int32_t  io = 0;                           /* 1 = receipt-operand form */
  std::vector<uint32_t> operand_carriers;    /* io form: carrier-table indices of operands (by reference) */
};

/* a fragment carrier: a hole (caller supplies) or a baked positional-int constant. */
struct FragCarrier {
  int32_t is_hole;              /* 1 = hole, 0 = baked constant */
  int32_t domain;              /* 1 ℤ, 2 ℚ */
  uint64_t h_bits;             /* declared height the fragment provisions for */
  int32_t hole_rank;           /* splice-order index if a hole, else -1 */
  std::vector<int64_t> data;   /* baked cell values (empty for a hole or an effect-array carrier) */
  int32_t eff = -1;            /* in-memory only: index into SlateFrag::effect_arrays if effect-filled, else -1 */
};

/* crc32 (zlib poly, reflected 0xEDB88320) — integrity against truncation/corruption, not a security boundary.
   Folded in as the body streams, so there is no second pass and no accumulator copy. On arm64 the CRC32
   instruction (__crc32d) computes this exact polynomial 8 bytes at a time (~1 byte/cycle), so a large baked
   payload's crc is bandwidth-bound, not the per-byte bottleneck it was; the 256-entry table is the portable
   fallback and handles the sub-8-byte tail. Both encoder and decoder use this one function, so the value the
   hardware and table paths produce is identical (little-endian: __crc32d consumes p[0] first, as the table does). */
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif
/* the polynomial's 256 steps — a true constant of the poly, not a remembered computation: built at compile
   time, so nothing is computed once and held across calls. */
struct FragCrcTable {
  uint32_t T[256] = {};
  constexpr FragCrcTable() { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1))); T[i] = c; } }
};
inline constexpr FragCrcTable kFragCrcTable{};
inline uint32_t frag_crc32_update(uint32_t c, const uint8_t *p, size_t n) {
#if defined(__ARM_FEATURE_CRC32)
  while (n >= 8) { uint64_t v; std::memcpy(&v, p, 8); c = __crc32d(c, v); p += 8; n -= 8; }   /* hardware, 8B/step */
#endif
  for (size_t i = 0; i < n; i++) c = kFragCrcTable.T[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return c;
}
inline uint32_t frag_crc32(const uint8_t *p, size_t n) { return frag_crc32_update(0xFFFFFFFFu, p, n) ^ 0xFFFFFFFFu; }

}  // namespace

struct SlateFrag {
  uint32_t nparams = 0;
  int32_t root = -1;
  uint32_t nholes = 0;
  std::vector<std::string> features;   /* the names the header declares */
  uint64_t potential = 0;
  slate_iface_receipt root_receipt{};
  bool has(const char *feature) const { for (const auto &f : features) if (f == feature) return true; return false; }
  std::vector<FragInstr> instr;
  std::vector<FragCarrier> carriers;
  std::vector<FragEffect> effects;         /* present iff "effects"; a kind=4 instr's aux indexes here */
  std::vector<FragEffectArray> effect_arrays;  /* present iff "effect-arrays"; each names a carrier it fills */
};

extern "C" const char *slate_dag_save_fragment(SlateDag *b, int32_t root, const uint32_t *holes, uint32_t nholes,
                                       uint8_t **word, uint64_t *wn) try {
  if (!b || b->err || !word || !wn || !sd_node_ok(b, root) || (nholes && !holes)) return "args";
  /* `root` must carry a stamped reading — read_ can lag the node count, and an emit-supplied id could point
     into that gap (a raw lit / forced big leaf). Refuse rather than read read_ out of bounds. */
  if ((size_t)root >= b->build.reading_count()) return "args";
  size_t N = b->arena.lower_node_count();

  /* mark nodes reachable from root (follow op children a,b and a carrier-load's index child a). */
  std::vector<char> reach(N, 0);
  std::vector<int32_t> st{root};
  while (!st.empty()) {
    int32_t id = st.back(); st.pop_back();
    if (id < 0 || (size_t)id >= N || reach[(size_t)id]) continue;
    reach[(size_t)id] = 1;
    if (b->arena.lower_is_param(id) || b->arena.lower_is_lit(id)) continue;
    if (b->arena.lower_is_big(id)) return "args";      /* a materialized big leaf is not source-form */
    if (b->arena.lower_is_carrier(id)) {
      int32_t ia = b->arena.lower_a(id); if (ia >= 0) st.push_back(ia);          /* the load's index child */
      if (const auto *pe = b->build.pending_effect_of(b->arena.lower_carrier_of(id)))  /* byte effect-array: follow args */
        if (!pe->io) for (int32_t ai : pe->args) if (ai >= 0) st.push_back(ai);
      continue;
    }
    if (b->arena.is_effect(id)) {                                 /* an effect node: follow its computed args */
      uint32_t na = b->arena.effect_arg_count(id);
      for (uint32_t k = 0; k < na; k++) { int32_t ai = b->arena.effect_arg(id, k); if (ai >= 0) st.push_back(ai); }
      continue;
    }
    uint32_t op = b->arena.lower_op(id);
    if (!(op == SLATE_OP_ADD || op == SLATE_OP_MUL || op == SLATE_OP_SUB || op == SLATE_OP_MULH || op == SLATE_OP_ASR || op == SLATE_OP_DIV)) return "args";               /* only add/sub/mul/mulh/asr cross the fragment ABI */
    int32_t ia = b->arena.lower_a(id), ib = b->arena.lower_b(id);
    if (ia >= 0) st.push_back(ia);
    if (ib >= 0) st.push_back(ib);
  }

  /* dense remap in increasing id order (children precede parents — the arena only ever appends). */
  std::vector<int32_t> dense(N, -1);
  uint32_t ninstr = 0;
  for (size_t id = 0; id < N; id++) if (reach[id]) dense[id] = (int32_t)ninstr++;

  auto is_declared_hole = [&](uint32_t cid) {
    for (uint32_t h = 0; h < nholes; h++) if (holes[h] == cid) return true;
    return false;
  };

  /* carrier table: every carrier the fragment needs, first-reference order. Added when a reachable load reads it,
     OR when it is an operand of a reachable io effect (an io operand crosses by reference — no load node), so
     ensure_carrier registers recursively (an io operand may itself be an io out carrier: a composite in one file). */
  const auto &cs = b->arena.lower_carriers();
  std::vector<int32_t> ctab(cs.size(), -1);
  uint32_t fragPot = (uint32_t)b->arena.env().height_bits;   /* the declared height, the one the Envelope holds */
  std::vector<FragCarrier> carriers;
  std::vector<FragEffectArray> effect_arrays;
  bool cerr = false;
  std::function<int(uint32_t)> ensure_carrier = [&](uint32_t cid) -> int {
    if (cid >= cs.size() || !cs[cid]) { cerr = true; return -1; }
    if (ctab[cid] >= 0) return ctab[cid];
    int idx = (int)carriers.size();
    ctab[cid] = idx;
    carriers.push_back(FragCarrier{});                     /* placeholder: keep idx stable across recursion */
    const Slate::WBuffer &w = *cs[cid];
    FragCarrier fc{};
    fc.domain = w.is_rns() ? 2 : 1;
    fc.h_bits = fragPot;
    fc.hole_rank = -1;
    if (const Slate::DagBuild::PendingEffect *pe = b->build.pending_effect_of(cid)) {
      fc.is_hole = 0;
      FragEffectArray fea{};
      fea.carrier = (uint32_t)idx;
      fea.cls = pe->cls;
      fea.len = pe->len;
      fea.blob = pe->blob;
      fea.io = pe->io ? 1 : 0;
      fea.consume = pe->consume ? 1 : 0; fea.cursor = pe->cursor_in;
      if (pe->io) {
        for (uint32_t oc : pe->operands) { int oi = ensure_carrier(oc); if (oi < 0) return -1; fea.operand_carriers.push_back((uint32_t)oi); }
      } else {
        for (int32_t ai : pe->args) fea.args.push_back(dense[(size_t)ai]);
      }
      fc.eff = (int32_t)effect_arrays.size();
      effect_arrays.push_back(std::move(fea));
    } else {
      fc.is_hole = is_declared_hole(cid) ? 1 : 0;
      if (!fc.is_hole) {
        if (w.is_rns() || w.is_f32() || w.width() > 8) { cerr = true; return -1; }   /* only positional int can bake */
        fc.data.resize(w.size());
        for (size_t i = 0; i < w.size(); i++) fc.data[i] = w.get(i);
      }
    }
    carriers[(size_t)idx] = std::move(fc);
    return idx;
  };
  for (size_t id = 0; id < N; id++) {
    if (!reach[id] || !b->arena.lower_is_carrier((int32_t)id)) continue;
    if (ensure_carrier(b->arena.lower_carrier_of((int32_t)id)) < 0) return "args";
  }
  if (cerr) return "args";
  uint32_t nfound = 0;
  for (auto &fc : carriers) if (fc.is_hole) fc.hole_rank = (int32_t)nfound++;
  if (nfound != nholes) return "args";                 /* a declared hole that root never reads */

  uint32_t nparams = 0;
  bool has_division = false, has_effects = false, has_rational = false;
  for (size_t id = 0; id < N; id++) {
    if (!reach[id]) continue;
    if (b->arena.lower_is_param((int32_t)id)) { uint32_t s = b->arena.lower_slot((int32_t)id); if (s + 1 > nparams) nparams = s + 1; }
    else if (b->arena.is_effect((int32_t)id)) { has_effects = true; }     /* effectful: pure fn of (graph, journal) */
    else if (!b->arena.lower_is_lit((int32_t)id) && !b->arena.lower_is_carrier((int32_t)id)) {
      uint32_t op = b->arena.lower_op((int32_t)id); if (op == SLATE_OP_MULH || op == SLATE_OP_ASR) has_division = true;
      if (op == SLATE_OP_DIV) has_rational = true;
    }
  }
  const bool has_arrays = !effect_arrays.empty();   /* has effect-array / io carriers — the trailing table */

  /* effect table: each reachable effect node's class + static blob + arg instr refs (dense indices). Built here
     so the emit loop can put a kind=4 instr carrying its table index; the table itself trails the carriers. */
  std::vector<int32_t> etab(N, -1);
  std::vector<FragEffect> effects;
  for (size_t id = 0; id < N; id++) {
    if (!reach[id] || !b->arena.is_effect((int32_t)id)) continue;
    etab[id] = (int32_t)effects.size();
    FragEffect fe;
    fe.cls = b->arena.effect_class((int32_t)id);
    const uint8_t *bp = b->arena.effect_blob_ptr((int32_t)id);
    fe.blob.assign(bp, bp + b->arena.effect_blob_len((int32_t)id));
    uint32_t na = b->arena.effect_arg_count((int32_t)id);
    for (uint32_t k = 0; k < na; k++) fe.args.push_back(dense[(size_t)b->arena.effect_arg((int32_t)id, k)]);
    effects.push_back(std::move(fe));
  }

  const Slate::Receipt &rr = b->build.reading(root);

  std::vector<uint8_t> buf;
  auto putb = [&](const void *p, size_t n) { const uint8_t *q = (const uint8_t *)p; buf.insert(buf.end(), q, q + n); };
  auto putu = [&](uint64_t v) { putb(&v, 8); };
  auto puti = [&](int64_t v) { putb(&v, 8); };
  auto put32 = [&](uint32_t v) { putb(&v, 4); };
  auto puti32 = [&](int32_t v) { putb(&v, 4); };
  auto putname = [&](const char *s) { uint32_t n = s ? (uint32_t)std::strlen(s) : 0; put32(n); if (n) putb(s, n); };
  putu(ninstr); putu((uint64_t)dense[(size_t)root]); putu(nparams); putu((uint64_t)carriers.size()); putu(fragPot);
  put32((uint32_t)has_division + (uint32_t)has_effects + (uint32_t)has_arrays + (uint32_t)has_rational);   /* the features this fragment uses, by name */
  if (has_division) putname("division");
  if (has_rational) putname("rational");
  if (has_effects)  putname("effects");
  if (has_arrays)   putname("effect-arrays");
  putname("scalar"); putname(nullptr); putname(slate_mode_name((int32_t)rr.mode)); putu(rr.h_bits);   /* root receipt: kind, domain=derive, mode, h */
  for (size_t id = 0; id < N; id++) {
    if (!reach[id]) continue;
    if (b->arena.lower_is_param((int32_t)id)) { put32(0); put32(b->arena.lower_slot((int32_t)id)); puti32(-1); puti32(-1); puti(0); }
    else if (b->arena.lower_is_lit((int32_t)id)) { put32(1); put32(0); puti32(-1); puti32(-1); puti(b->arena.lower_lit((int32_t)id)); }
    else if (b->arena.lower_is_carrier((int32_t)id)) {
      uint32_t cid = b->arena.lower_carrier_of((int32_t)id);
      put32(2); put32((uint32_t)ctab[cid]); puti32(dense[(size_t)b->arena.lower_a((int32_t)id)]); puti32(-1); puti(0);
    } else if (b->arena.is_effect((int32_t)id)) {
      put32(4); put32((uint32_t)etab[id]); puti32(-1); puti32(-1); puti(0);   /* aux = effect-table index */
    } else {
      put32(3); put32(b->arena.lower_op((int32_t)id));
      puti32(dense[(size_t)b->arena.lower_a((int32_t)id)]); puti32(dense[(size_t)b->arena.lower_b((int32_t)id)]); puti(0);
    }
  }
  for (auto &fc : carriers) {
    put32((uint32_t)fc.is_hole); putname(slate_domain_name(fc.domain)); putu(fc.h_bits); putu((uint64_t)fc.data.size());
    if (!fc.data.empty()) putb(fc.data.data(), fc.data.size() * 8);   /* one bulk append, not a per-cell loop */
  }
  if (has_effects) {                                                  /* trailing effect table (only if effectful) */
    putu((uint64_t)effects.size());
    for (auto &fe : effects) {
      put32((uint32_t)fe.cls.size()); if (!fe.cls.empty()) putb((const uint8_t *)fe.cls.data(), fe.cls.size());
      putu((uint64_t)fe.blob.size());
      if (!fe.blob.empty()) putb(fe.blob.data(), fe.blob.size());
      putu((uint64_t)fe.args.size());
      for (int32_t ai : fe.args) puti32(ai);
    }
  }
  if (has_arrays) {                                                   /* trailing effect-array table (byte / io) */
    putu((uint64_t)effect_arrays.size());
    for (auto &fea : effect_arrays) {
      put32(fea.carrier); put32((uint32_t)fea.cls.size()); if (!fea.cls.empty()) putb((const uint8_t *)fea.cls.data(), fea.cls.size()); puti(fea.len);
      put32((uint32_t)fea.io); put32((uint32_t)fea.consume);
      putu((uint64_t)fea.cursor.size()); if (!fea.cursor.empty()) putb(fea.cursor.data(), fea.cursor.size());
      putu((uint64_t)fea.blob.size());
      if (!fea.blob.empty()) putb(fea.blob.data(), fea.blob.size());
      if (fea.io) {
        putu((uint64_t)fea.operand_carriers.size());
        for (uint32_t oc : fea.operand_carriers) put32(oc);
      } else {
        putu((uint64_t)fea.args.size());
        for (int32_t ai : fea.args) puti32(ai);
      }
    }
  }
  put32(frag_crc32(buf.data(), buf.size()));

  /* The construction, written out, is a leaf: it goes through the same door as any bytes handed in and lands as
     byte cells under the word of (the lens in the clear ‖ the bytes). A leaf already there is a hit and is not
     put again. With a roster the door in front of the store slices it across the carrying machines, so anyone
     who asks for the word gathers it whole by walking the cells. Nothing says how many there are: the records
     are the count. A store that kept nothing refuses — a program nobody can read is not a program. */
  const Slate::Word key = b->arena.leaf_key(buf.data(), buf.size());
  if (key.empty() || !Slate::leaf_put(b->arena.store_env(), b->arena.env().codec, key, buf.data(), buf.size()))
    return "refused";
  uint8_t *o = (uint8_t *)std::malloc(key.size());
  if (!o) return "nomem";
  std::memcpy(o, key.data(), key.size());
  *word = o; *wn = (uint64_t)key.size();
  return nullptr;
} catch (const std::bad_alloc &) { return "nomem"; }
  catch (...) { return "internal"; }

/* The fragment's bytes, parsed. Everything is bounds-checked against the end of the leaf that was read, and the
   crc is folded in as the body is walked (no second pass, no copy). Header counts are untrusted — never
   resize() to one ahead of the bytes actually there. */
static SlateFrag *frag_parse(const uint8_t *p, size_t n) try {
  size_t cur = 0;
  uint32_t crc = 0xFFFFFFFFu;
  auto pull = [&](void *d, size_t m, bool track) -> bool {
    if (m > n - cur) return false;
    std::memcpy(d, p + cur, m);
    if (track) crc = frag_crc32_update(crc, p + cur, m);
    cur += m;
    return true;
  };
  uint64_t hdr[5];
  if (!pull(hdr, sizeof hdr, true)) return nullptr;
  uint64_t ninstr = hdr[0], root = hdr[1], nparams = hdr[2], ncarrier = hdr[3], pot = hdr[4];
  if (ninstr == 0 || ninstr > kFragMaxCount || nparams > kFragMaxCount || ncarrier > kFragMaxCount) return nullptr;
  if (root >= ninstr) return nullptr;

  auto f = std::make_unique<SlateFrag>();
  f->nparams = (uint32_t)nparams; f->root = (int32_t)root; f->potential = pot;

  /* a name: u32 len + text (len 0 = none) */
  auto pull_name = [&](std::string &s) -> bool {
    uint32_t n; if (!pull(&n, 4, true) || n > kFragNameMax) return false;
    s.assign((size_t)n, '\0'); return n == 0 || pull(s.data(), (size_t)n, true);
  };
  /* the features: every name must be one this engine knows — a feature it cannot honor refuses the load */
  uint32_t nfeat; std::string name;
  if (!pull(&nfeat, 4, true) || nfeat > 16) return nullptr;
  for (uint32_t i = 0; i < nfeat; i++) {
    if (!pull_name(name)) return nullptr;
    bool known = false; for (const char *k : kFragFeatures) if (name == k) known = true;
    if (!known) return nullptr;
    f->features.push_back(name);
  }
  /* the root receipt: kind, domain (none = derive), mode, h — each what the writer said, by name */
  std::string rkind, rdom, rmode; uint64_t rrh;
  if (!pull_name(rkind) || !pull_name(rdom) || !pull_name(rmode) || !pull(&rrh, 8, true)) return nullptr;
  if (rkind != "scalar") return nullptr;
  int rd = frag_domain_of(rdom); if (rd < -1) return nullptr;
  if (rmode != "compose" && rmode != "decompose") return nullptr;
  f->root_receipt.kind = "scalar"; f->root_receipt.domain = rd < 0 ? nullptr : slate_domain_name(rd);
  f->root_receipt.mode = rmode == "compose" ? "compose" : "decompose"; f->root_receipt.h_bits = rrh;

  /* Header counts are untrusted — never resize() to one ahead of the stream (a single flipped count with an
     intact crc would otherwise force a multi-GB alloc before the crc is even reached). Grow incrementally
     (reserve a bounded prefix, then push_back / bounded chunks), so allocation tracks bytes actually consumed:
     a lying count fails on the first missing byte, having grown only to what the stream really held. */
  f->instr.reserve((size_t)(ninstr < 65536 ? ninstr : 65536));
  for (uint64_t i = 0; i < ninstr; i++) {
    uint32_t kind, aux; int32_t a, b; int64_t imm;
    if (!pull(&kind, 4, true) || !pull(&aux, 4, true) || !pull(&a, 4, true) || !pull(&b, 4, true) || !pull(&imm, 8, true))
      return nullptr;
    switch (kind) {                                              /* back-references only ⇒ the graph is acyclic */
      case 0: if (aux >= nparams) return nullptr; break;
      case 1: break;
      case 2: if (aux >= ncarrier || a < 0 || (uint64_t)a >= i) return nullptr; break;
      case 3: if (!(aux == SLATE_OP_ADD || aux == SLATE_OP_MUL || aux == SLATE_OP_SUB || aux == SLATE_OP_MULH || aux == SLATE_OP_ASR || aux == SLATE_OP_DIV) ||
                  a < 0 || (uint64_t)a >= i || b < 0 || (uint64_t)b >= i) return nullptr; break;
      case 4: break;                                            /* effect: aux + args validated after the effect table */
      default: return nullptr;
    }
    f->instr.push_back(FragInstr{kind, aux, a, b, imm});
  }

  uint32_t nholes = 0;
  f->carriers.reserve((size_t)(ncarrier < 4096 ? ncarrier : 4096));
  for (uint64_t c = 0; c < ncarrier; c++) {
    uint32_t is_hole; uint64_t hbits, n; std::string dname;
    if (!pull(&is_hole, 4, true) || !pull_name(dname) || !pull(&hbits, 8, true) || !pull(&n, 8, true)) return nullptr;
    int32_t domain = frag_domain_of(dname);
    if (domain < 0 || n > kFragMaxCount) return nullptr;         /* a carrier's domain is always said */
    FragCarrier fc{};
    fc.is_hole = is_hole ? 1 : 0; fc.domain = domain; fc.h_bits = hbits;
    if (fc.is_hole) { if (n != 0) return nullptr; fc.hole_rank = (int32_t)nholes++; }
    else {                                                       /* baked data in bounded chunks: a lying length
                                                                    allocates only as fast as the stream yields */
      fc.hole_rank = -1;
      for (uint64_t got = 0; got < n;) {
        uint64_t take = (n - got < (1u << 20)) ? (n - got) : (1u << 20);   /* ≤ 8 MB per grow step */
        size_t old = fc.data.size();
        fc.data.resize(old + (size_t)take);
        if (!pull(fc.data.data() + old, (size_t)take * 8, true)) return nullptr;
        got += take;
      }
    }
    f->carriers.push_back(std::move(fc));
  }
  f->nholes = nholes;

  if (f->has("effects")) {                                      /* trailing effect table (bounded, streamed into crc) */
    uint64_t neffect;
    if (!pull(&neffect, 8, true) || neffect > kFragMaxCount) return nullptr;
    f->effects.reserve((size_t)(neffect < 4096 ? neffect : 4096));
    for (uint64_t e = 0; e < neffect; e++) {
      uint32_t cn; uint64_t bl;
      if (!pull(&cn, 4, true) || cn == 0 || cn > kFragNameMax) return nullptr;
      FragEffect fe{}; fe.cls.assign((size_t)cn, '\0');
      if (!pull(fe.cls.data(), (size_t)cn, true)) return nullptr;
      if (!pull(&bl, 8, true) || bl > kFragBlobMax) return nullptr;
      for (uint64_t got = 0; got < bl;) {                       /* blob in bounded chunks: a lying length can't preallocate */
        uint64_t take = (bl - got < (1u << 20)) ? (bl - got) : (1u << 20);
        size_t old = fe.blob.size(); fe.blob.resize(old + (size_t)take);
        if (!pull(fe.blob.data() + old, (size_t)take, true)) return nullptr;
        got += take;
      }
      uint64_t na;
      if (!pull(&na, 8, true) || na > kFragMaxCount) return nullptr;
      for (uint64_t k = 0; k < na; k++) {
        int32_t ai;
        if (!pull(&ai, 4, true) || ai < 0 || (uint64_t)ai >= ninstr) return nullptr;
        fe.args.push_back(ai);
      }
      f->effects.push_back(std::move(fe));
    }
  }
  if (f->has("effect-arrays")) {                               /* trailing effect-array table (byte / io) */
    uint64_t nea;
    if (!pull(&nea, 8, true) || nea > kFragMaxCount) return nullptr;
    f->effect_arrays.reserve((size_t)(nea < 4096 ? nea : 4096));
    for (uint64_t e = 0; e < nea; e++) {
      uint32_t carrier, cn; int64_t len; std::string cls;
      if (!pull(&carrier, 4, true) || !pull(&cn, 4, true) || cn == 0 || cn > kFragNameMax) return nullptr;
      cls.assign((size_t)cn, '\0');
      if (!pull(cls.data(), (size_t)cn, true) || !pull(&len, 8, true)) return nullptr;
      if (carrier >= f->carriers.size() || len < 0 || (uint64_t)len > kFragMaxCount) return nullptr;
      FragCarrier &fc = f->carriers[(size_t)carrier];
      if (fc.is_hole || !fc.data.empty() || fc.eff >= 0) return nullptr;   /* fresh non-hole, non-baked carrier */
      uint32_t io, consume;
      if (!pull(&io, 4, true) || io > 1 || !pull(&consume, 4, true) || consume > 1 || (io && consume)) return nullptr;
      uint64_t curn;
      if (!pull(&curn, 8, true) || curn > 4096) return nullptr;
      FragEffectArray fea{}; fea.carrier = carrier; fea.cls = cls; fea.len = len; fea.io = (int32_t)io; fea.consume = (int32_t)consume;
      fea.cursor.resize((size_t)curn); if (curn && !pull(fea.cursor.data(), (size_t)curn, true)) return nullptr;
      uint64_t bl;
      if (!pull(&bl, 8, true) || bl > kFragBlobMax) return nullptr;
      for (uint64_t got = 0; got < bl;) {
        uint64_t take = (bl - got < (1u << 20)) ? (bl - got) : (1u << 20);
        size_t old = fea.blob.size(); fea.blob.resize(old + (size_t)take);
        if (!pull(fea.blob.data() + old, (size_t)take, true)) return nullptr;
        got += take;
      }
      uint64_t na;
      if (!pull(&na, 8, true) || na > kFragMaxCount) return nullptr;
      if (io) {
        for (uint64_t k = 0; k < na; k++) { uint32_t oc; if (!pull(&oc, 4, true) || oc >= ncarrier) return nullptr; fea.operand_carriers.push_back(oc); }
      } else {
        for (uint64_t k = 0; k < na; k++) { int32_t ai; if (!pull(&ai, 4, true) || ai < 0 || (uint64_t)ai >= ninstr) return nullptr; fea.args.push_back(ai); }
      }
      fc.eff = (int32_t)f->effect_arrays.size();
      f->effect_arrays.push_back(std::move(fea));
    }
  }
  /* with the effect table known: every kind=4 instr's aux must index it, and each effect's args must be
     back-references (< that effect instr's own position) so replay maps them before use (acyclic). */
  for (uint64_t i = 0; i < ninstr; i++) {
    if (f->instr[(size_t)i].kind != 4) continue;
    uint32_t aux = f->instr[(size_t)i].aux;
    if (aux >= f->effects.size()) return nullptr;
    for (int32_t ai : f->effects[(size_t)aux].args) if (ai < 0 || (uint64_t)ai >= i) return nullptr;
  }

  uint32_t want;
  if (!pull(&want, 4, false) || want != (crc ^ 0xFFFFFFFFu)) return nullptr;   /* finalize the folded crc */
  if (cur != n) return nullptr;                       /* the leaf's count said exactly this many bytes */
  return f.release();
} catch (...) { return nullptr; }

/* A program is read by its word — `slate_dag_save_fragment` handed that word back, and nothing beside it. The
 * reader walks the cells word ‖ 0, word ‖ 1, … and stops at the first cell nobody has; the records are the
 * count. The bytes come out of this container's store through the same door every other row comes through, so
 * a container carrying a share of a roster gathers the whole leaf and one carrying none reads its own rows. A
 * word the store does not hold is a miss and refuses. A leaf the door could only half gather — a cell on some
 * carriers but not all — is not whole yet: it refuses too, never a guess and never a short program. A walk cut
 * short by a store that lost or was fed a bad cell lands on the parser, whose crc and exact-end check refuse
 * it. */
extern "C" SlateFrag *slate_frag_load(SlateDag *b, const uint8_t *word, uint64_t wn) try {
  if (!b || !word || !wn) return nullptr;
  std::vector<uint8_t> bytes;
  bool partial = false;
  if (!Slate::leaf_get(b->arena.store_env(), b->arena.env().codec, Slate::Word(word, word + wn), bytes, partial))
    return nullptr;
  if (bytes.size() > kFragMaxCount) return nullptr;
  return frag_parse(bytes.data(), bytes.size());
} catch (...) { return nullptr; }

extern "C" const char *slate_frag_iface(const SlateFrag *f, uint32_t *nparams, slate_hole *holes_out,
                                uint32_t *nholes_io, slate_iface_receipt *root_out) try {
  if (!f) return "args";
  if (nparams) *nparams = f->nparams;
  if (root_out) *root_out = f->root_receipt;
  if (nholes_io) {
    uint32_t cap = *nholes_io;
    *nholes_io = f->nholes;
    if (holes_out) {
      if (cap < f->nholes) return "outsize";
      for (const auto &fc : f->carriers) {
        if (!fc.is_hole) continue;
        slate_hole h{};
        h.slot = (uint32_t)fc.hole_rank;
        h.receipt.kind = "array"; h.receipt.domain = slate_domain_name(fc.domain); h.receipt.mode = nullptr; h.receipt.h_bits = fc.h_bits;
        holes_out[(size_t)fc.hole_rank] = h;
      }
    }
  }
  return nullptr;
} catch (...) { return "internal"; }

extern "C" const char *slate_dag_splice(SlateDag *b, const SlateFrag *f, const uint32_t *arg_carriers, uint32_t narg,
                                        int32_t *out_root) try {
  if (out_root) *out_root = -1;
  if (!b || b->err || !f || narg != f->nholes || (narg && !arg_carriers)) return "args";
  const auto &cs = b->arena.lower_carriers();
  /* typecheck every hole before mutating b, so a refusal leaves the builder untouched (atomic). */
  for (const auto &fc : f->carriers) {
    if (!fc.is_hole) continue;
    uint32_t supplied = arg_carriers[(size_t)fc.hole_rank];
    if (supplied >= cs.size() || !cs[supplied]) return "args";
    int sdom = cs[supplied]->is_rns() ? 2 : 1;
    if (fc.domain >= 0 && sdom > fc.domain) return "refused";   /* ℚ supplied into a ℤ hole */
  }
  /* register baked carriers into b; map fragment carrier idx -> b carrier id. */
  static const uint32_t kDeferEffCarrier = (uint32_t)-1;
  static const uint32_t kSplicingEffCarrier = (uint32_t)-2;   /* an effect carrier currently being spliced (cycle guard) */
  std::vector<uint32_t> cmap(f->carriers.size());
  for (size_t c = 0; c < f->carriers.size(); c++) {
    const FragCarrier &fc = f->carriers[c];
    cmap[c] = fc.eff >= 0 ? kDeferEffCarrier
                          : (fc.is_hole ? arg_carriers[(size_t)fc.hole_rank] : b->build.carrier(fc.data));
  }
  /* replay: remap dense id -> b node id; receipts compose in b->build as each op is emitted. */
  std::vector<int32_t> map(f->instr.size(), -1);

  /* Create a deferred effect-array carrier (and, recursively, the io operand carriers it references — an io
     operand has no load, so the replay loop never reaches it). Byte form rebuilds via effect_array with instr-arg
     values (must already be replayed); io form via effect_io with the operand carriers' b-ids. */
  std::function<uint32_t(uint32_t)> ensure_spliced = [&](uint32_t cix) -> uint32_t {
    if (cmap[cix] == kSplicingEffCarrier) { b->err = true; return 0; }   /* re-entry: a cyclic operand-carrier graph */
    if (cmap[cix] != kDeferEffCarrier) return cmap[cix];
    cmap[cix] = kSplicingEffCarrier;                                     /* mark in-progress before recursing */
    const FragEffectArray &fea = f->effect_arrays[(size_t)f->carriers[(size_t)cix].eff];
    if (fea.io) {
      std::vector<uint32_t> ops; ops.reserve(fea.operand_carriers.size());
      for (uint32_t oc : fea.operand_carriers) ops.push_back(ensure_spliced(oc));
      cmap[cix] = b->build.effect_io(fea.cls.c_str(), fea.blob.empty() ? nullptr : fea.blob.data(),
                                     (uint64_t)fea.blob.size(), ops.empty() ? nullptr : ops.data(),
                                     (uint32_t)ops.size(), fea.len);
    } else if (fea.consume) {
      cmap[cix] = b->build.consume(fea.cls.c_str(), fea.blob.empty() ? nullptr : fea.blob.data(), (uint64_t)fea.blob.size(),
                                   fea.cursor.empty() ? nullptr : fea.cursor.data(), (uint64_t)fea.cursor.size(), fea.len);
    } else {
      std::vector<int32_t> args; args.reserve(fea.args.size());
      for (int32_t ai : fea.args) { if (map[(size_t)ai] < 0) { b->err = true; return 0; } args.push_back(map[(size_t)ai]); }
      cmap[cix] = b->build.effect_array(fea.cls.c_str(), fea.blob.empty() ? nullptr : fea.blob.data(),
                                        (uint64_t)fea.blob.size(), args.empty() ? nullptr : args.data(),
                                        (uint32_t)args.size(), fea.len);
    }
    return cmap[cix];
  };

  for (size_t i = 0; i < f->instr.size(); i++) {
    const FragInstr &in = f->instr[i];
    int32_t nid;
    switch (in.kind) {
      case 0: nid = b->build.param(in.aux); break;
      case 1: nid = b->build.lit((long long)in.imm); break;
      case 2: {
        uint32_t bc = (cmap[in.aux] == kDeferEffCarrier) ? ensure_spliced(in.aux) : cmap[in.aux];
        if (b->err) return "internal";
        nid = b->build.load(bc, map[(size_t)in.a]);
        break;
      }
      case 4: {                                                  /* an effect node: remap its args, rebuild it */
        const FragEffect &fe = f->effects[(size_t)in.aux];
        std::vector<int32_t> args;
        args.reserve(fe.args.size());
        for (int32_t ai : fe.args) args.push_back(map[(size_t)ai]);
        nid = b->build.effect(fe.cls.c_str(), fe.blob.empty() ? nullptr : fe.blob.data(),
                              (uint64_t)fe.blob.size(), args.data(), (uint32_t)args.size());
        break;
      }
      default:
        switch (in.aux) {
          case SLATE_OP_ADD:  nid = b->build.add(map[(size_t)in.a], map[(size_t)in.b]); break;
          case SLATE_OP_MUL:  nid = b->build.mul(map[(size_t)in.a], map[(size_t)in.b]); break;
          case SLATE_OP_SUB:  nid = b->build.sub(map[(size_t)in.a], map[(size_t)in.b]); break;
          case SLATE_OP_MULH: nid = b->build.mulh(map[(size_t)in.a], map[(size_t)in.b]); break;
          case SLATE_OP_DIV:  nid = b->build.div(map[(size_t)in.a], map[(size_t)in.b]); break;
          default:            nid = b->build.asr(map[(size_t)in.a], map[(size_t)in.b]); break;
        }
    }
    map[i] = nid;
  }
  if (out_root) *out_root = map[(size_t)f->root];
  return nullptr;
} catch (const std::bad_alloc &) { if (b) b->err = true; return "nomem"; }
  catch (...) { if (b) b->err = true; return "internal"; }

extern "C" void slate_frag_free(SlateFrag *f) { delete f; }

/* The executable invariant, in one call: caps -> walk the program's leaf by its word -> carriers -> splice -> run.
 * Every consumer (the stdlib runtime, the slate CLI, any language binding) open-coded this same chain; here it
 * is once, over the ordinary C ABI it composes (no engine internals). The container is the caller's, because
 * the store is: a program is a leaf, and which store holds it is what the container says. The SlateArray it
 * returns is an independently owned buffer that outlives the builder, so the caller reads and frees it. Pure —
 * the fragment does any io itself through its granted providers. */
extern "C" SlateArray *slate_invoke(SlateDag *b, const uint8_t *word, uint64_t wn,
                                    const uint8_t *const *args, const uint64_t *arg_lens, uint32_t nargs,
                                    const char *const *caps, uint32_t ncaps, const int64_t *dims, uint32_t ndims) try {
  if (!b) return nullptr;
  if (ncaps) slate_dag_effect_caps(b, caps, ncaps);
  SlateFrag *f = slate_frag_load(b, word, wn);
  if (!f) return nullptr;
  std::vector<uint32_t> carriers;
  carriers.reserve(nargs);
  for (uint32_t i = 0; i < nargs; i++)
    carriers.push_back(slate_dag_carrier_bytes(b, args ? args[i] : nullptr, arg_lens ? arg_lens[i] : 0));
  int32_t root = -1; slate_dag_splice(b, f, carriers.empty() ? nullptr : carriers.data(), nargs, &root);
  slate_frag_free(f);
  if (root < 0) return nullptr;
  int64_t one = 1;
  return slate_dag_run(b, root, dims ? dims : &one, dims ? ndims : 1);
} catch (...) { return nullptr; }

/* The S axis over the C ABI, unified: one door does measurement (by shape) or value computation, selected by
 * `operation`.
 *   • operation < 0  → measurement: walk from `root` and collect the nodes whose carried shape {P, Λ} equals
 *     the target {potential, mode}. Matching is by the shape the builder already composed onto each node
 *     (DagBuild::search #1), never a value — a structural/relatedness query, decidable and cheap (O(1) per
 *     vertex, budgeted, resumable). No value is computed: out_num/out_den/out_undef stay at their defaults (0/1/0).
 *   • operation >= 0 → value: run the value search (DagBuild::search #2) over the collection rooted at
 *     `root`, where `operation` is a node whose Λ steers: a `sub`-built op (decompose) locates the element
 *     equal to `target` (a lit node); an `add`-built op (compose) aggregates the exact fold (pass target =
 *     lit(0)). Matched leaf ids go to out/out_count; the settled value (the located element, or the aggregate)
 *     is decoded into *out_num / *out_den. An undefined/refused value sets *out_undef=1; a value past int64
 *     returns "wide" rather than a wrong answer (mirrors the slate_array_i64_unsafe wide path). Fail-closed:
 *     a null/poisoned builder or an out-of-range root/target/operation refuses to "args".
 * Any output pointer may be NULL. */
extern "C" const char *slate_dag_search(SlateDag *b, int32_t root, uint64_t potential, const char *mode, uint64_t budget,
                                int32_t *out, uint32_t cap, uint32_t *out_count, uint64_t *out_visited,
                                int32_t *out_resumable, int32_t target, const char *extremum, int32_t operation,
                                int64_t *out_num, int64_t *out_den, int32_t *out_undef) try {
  if (out_count) *out_count = 0;
  if (out_visited) *out_visited = 0;
  if (out_resumable) *out_resumable = 0;
  if (out_num) *out_num = 0;
  if (out_den) *out_den = 1;
  if (out_undef) *out_undef = 0;
  if (!sd_node_ok(b, root)) return "args";
  const bool decompose = mode && std::strcmp(mode, "decompose") == 0;
  if (mode && !decompose && std::strcmp(mode, "compose") != 0) return "args";
  const bool ex_min = extremum && std::strcmp(extremum, "min") == 0, ex_max = extremum && std::strcmp(extremum, "max") == 0;
  if (extremum && !ex_min && !ex_max) return "args";

  if (operation < 0) {                                 /* measurement: by shape {P, Λ} — the current behavior */
    Slate::Shape shp{potential, decompose ? Slate::Receipt::Mode::Decompose
                                          : Slate::Receipt::Mode::Compose};
    Slate::Search r = b->build.search(root, shp, budget);
    if (out_visited) *out_visited = r.visited;
    if (out_resumable) *out_resumable = r.resumable ? 1 : 0;
    if (out_count) *out_count = (uint32_t)r.at.size();
    if (out && cap) {
      uint32_t n = (uint32_t)r.at.size() < cap ? (uint32_t)r.at.size() : cap;
      for (uint32_t i = 0; i < n; i++) out[i] = r.at[i];
    }
    return nullptr;
  }

  /* value: by computation. Guard target/operation node ids too (overload #2 also guards internally, but refuse
     early and uniformly here). `target` names the value sought (a lit node); `operation` steers by Λ. The two
     extremum names a direction, not a node — the extremal measurement (kSearchMin/kSearchMax in the builder). */
  const bool extremal = ex_min || ex_max;
  if (extremal) target = ex_max ? Slate::kSearchMax : Slate::kSearchMin;
  if ((!extremal && !sd_node_ok(b, target)) || !sd_node_ok(b, operation)) return "args";
  Slate::Search r = b->build.search(root, target, operation, budget);
  if (out_visited) *out_visited = r.visited;
  if (out_resumable) *out_resumable = r.resumable ? 1 : 0;
  if (out_count) *out_count = (uint32_t)r.at.size();
  if (out && cap) {
    uint32_t n = (uint32_t)r.at.size() < cap ? (uint32_t)r.at.size() : cap;
    for (uint32_t i = 0; i < n; i++) out[i] = r.at[i];
  }
  /* Decode the settled value → num/den through the one int64 readback: a refused/undefined fold sets
     *out_undef=1 (OK, no value); a value past int64 returns EWIDE (never a wrong answer). */
  Slate::Receipt rd = r.value.to_reading();
  if (!rd.has_value()) { if (out_undef) *out_undef = 1; return nullptr; }
  int64_t vn = 0, vd = 1;
  if (const char *e = reading_i64(rd, &vn, &vd)) return e;
  if (out_num) *out_num = vn;
  if (out_den) *out_den = vd;
  return nullptr;
} catch (...) { return "internal"; }

/* ---- the self-hosting seam: emit / run as engine-internal effect handlers --------------------------------
 * Installed on every builder's Envelope (SlateDag ctor). effect/eval.hpp resolves the reserved classes
 * "slate.emit" / "slate.run" through here in its pre-pass, so a running .slate authors and runs other
 * fragments with no host code. Both return a malloc'd buffer the engine frees. */
#include <cstdlib>
#include <cstring>
namespace {

/* run: frag_load the .slate bytes in `blob` and run it on a fresh builder (never `cx`), returning its Reading as
 * the effect's result bytes (8-byte LE int64 for a scalar reading). Journaled by the caller — the blob carries
 * the program bytes, so distinct programs get distinct journal keys. The nested run uses its own arena, so the
 * in-flight outer pre-pass is untouched (CtxScope nests, ForceGuard is same-thread reentrant). */
/* A self-hosting run has no nesting to bound: it is composed by splice (append the child into this graph) + a
 * forward force, so a run — even a run that runs another — is one flat forward sweep over the flattened graph,
 * not a stack of dispatches. There is no recursion depth to cap. */
/* splice: inline the `.slate` bytes in `prog` into this arena's graph (append — monotonic) and hand back the
 * spliced root node id. This is the composition half of a self-hosting run; the effect pre-pass (effect/eval.hpp,
 * a friend of Arena) forces that root with the ordinary forward sweep, so a run is one flat forward evaluation
 * with no nested dispatch and no native call stack. The child is part of this arena, so it shares the container's
 * grant/fds/providers by construction — a server loop keeps its socket for free. */
static int frag_splice_impl(Slate::Arena &cx, const uint8_t *prog, uint32_t n, int32_t *root) {
  if (!root) return 1;
  SlateDag *b = static_cast<SlateDag *>(cx.env().frag_self);
  if (!b || &b->arena != &cx) return 1;                    /* must own cx (guards a by-value async Envelope copy) */
  SlateFrag *f = frag_parse(prog, (size_t)n);              /* the leaf's bytes, already read by name upstream */
  if (!f) return 1;
  int32_t sroot = -1; slate_dag_splice(b, f, nullptr, 0, &sroot);      /* append the child into this graph */
  slate_frag_free(f);
  if (sroot < 0) return 1;
  *root = sroot;
  return 0;
}

/* run: a program is a leaf. The request blob (or the io operand) is the program's word, and nothing beside
 * it; the effect layer walks the leaf out of the store and splices what it finds. Bytes never cross a door,
 * and a word the store does not hold refuses. (Earlier forms — bytes in the blob, a 'C'|carrier_id — were removed: bytes
 * are not a name, and a carrier id is a local build handle, not an address.) */
/* emit: serialize the sub-DAG rooted at the node id encoded in `blob` (an i32), reusing the engine's own
 * save_fragment (normalization + crc), which keeps those bytes as a leaf through the door and hands back
 * their word. What comes back here is the program's name — that word — not its bytes. `cx` is a SlateDag's
 * arena (its first member), so we recover the builder to walk it. Built as an array-effect, which resolves in
 * DagBuild::run step 1 — before effect_eval collapses any effect node — so the target sub-DAG is intact.
 * save_fragment is a pure read of the graph (no force, no mutation), so re-entering it on the in-flight builder
 * is safe. */
static int frag_emit_impl(Slate::Arena &cx, const uint8_t *blob, uint32_t blen, uint8_t **out, uint64_t *outn) {
  if (blen < 4) return 1;
  int32_t node;
  std::memcpy(&node, blob, 4);
  /* Recover the owning builder via the typed back-pointer, and verify it actually owns this arena. A by-value
   * Envelope copy (async task spawn) would carry a frag_self pointing at a different SlateDag; the identity
   * check catches that and refuses rather than dereferencing a mismatched owner. */
  SlateDag *b = static_cast<SlateDag *>(cx.env().frag_self);
  if (!b || &b->arena != &cx) return 1;
  uint8_t *w = nullptr;
  uint64_t wn = 0;
  if (slate_dag_save_fragment(b, node, nullptr, 0, &w, &wn) != nullptr) return 1;
  *out = w; *outn = wn;          /* the leaf's word, as save_fragment malloc'd it: the answer is the name */
  return 0;
}

/* perform: the unified self-hosting seam entry. Only emit routes here now; a run ("slate.run") is no longer a
 * nested dispatch — the effect pre-pass composes it by `splice` (frag_splice_impl) + a forward force, so it never
 * takes this path (and never a native call stack). */
static int frag_perform_impl(Slate::Arena &cx, const char *cls, const uint8_t *blob, uint32_t blen,
                             uint8_t **out, uint64_t *outn) {
  if (cls && std::strcmp(cls, Slate::kFxEmit) == 0) return frag_emit_impl(cx, blob, blen, out, outn);
  return 1;   /* an unrecognized engine class: refuse cleanly */
}

} // namespace

static const Slate::FragOps *engine_frag_ops() {
  /* emit via perform; a run composes via splice (append into this graph) — the pre-pass forces it forward, so a
   * self-hosting run is one flat forward evaluation, never a nested dispatch on the native stack. The seam's own
   * `run` slot is left unwired: splice is the one body a run goes through. */
  static const Slate::FragOps ops = { .emit = frag_emit_impl, .perform = frag_perform_impl,
                                      .splice = frag_splice_impl };
  return &ops;
}
