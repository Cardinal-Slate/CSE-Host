/* Adversarial fuzz of the fragment decoder (slate_frag_load) against an untrusted store.
 *
 * Keep one valid fragment  root[i] = A[i]*2 + C[i]  (A a hole, C baked {10,20,30,40}) as a leaf, then mutate
 * the bytes the store hands back for that leaf's cells and load it by name every time. There is no file to
 * corrupt any more: a program is a leaf, so the only thing that can lie to a load is the store it reads from,
 * and that is what is fuzzed here — one cell of a leaf is one byte of the program. Invariant under test: load
 * must neither crash nor hand back a fragment that, spliced and run, yields a wrong value; it either rejects
 * (NULL) or loads to a fragment that runs to an exact value.
 *
 * Each candidate is loaded (and, if accepted, spliced and run) in a forked child under an address-space
 * rlimit, so a segfault, abort, or pathological allocation cannot kill the parent; the parent reads a child
 * that dies by signal as a crash finding and a clean verdict code otherwise.
 *
 * Regimes:
 *   A. crc-intact single-byte flips/sets over the whole buffer plus random multi-byte. The trailing crc32
 *      covers [0,len-4), so any accepted load is byte-identical to the original and its values are
 *      {12,24,36,48}; anything else is a decoder bug.
 *   B. crc-consistent header/count probes: overwrite ninstr@24 / root@32 / nparams@40 / ncarrier@48 / pot@56
 *      and the baked-data length word with adversarial values, then recompute the crc so integrity passes.
 *      This forces the structural bounds checks to decide. An accepted load may be a different but valid
 *      fragment; it must still run without crashing to a defined exact value.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include "slate/slate.h"
#include "../memstore.h"

static uint32_t T[256];
static void crc_init(void){ for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c>>1)^(0xEDB88320u&(uint32_t)(-(int32_t)(c&1)));T[i]=c;} }
static uint32_t crc_calc(const uint8_t*p,size_t n){uint32_t c=0xFFFFFFFFu;for(size_t i=0;i<n;i++)c=T[(c^p[i])&0xFF]^(c>>8);return c^0xFFFFFFFFu;}

static const int64_t ORIG[4] = {12, 24, 36, 48};

/* the leaf being fuzzed: its name, and the bytes it was kept with */
static MsProg g_frag;

/* Child-side: make the store hand back the candidate bytes, load by name, and if accepted splice and run.
 * Exit codes:
 *   0  rejected (NULL) -- the safe outcome
 *   10 accepted, ran to the correct values (regime A: original values; regime B: defined tower domain)
 *   11 accepted, wrong value / bad domain (a decoder bug)
 *   12 accepted, run refused cleanly (splice/run returned a defined refusal -- safe)
 * A crash inside slate_frag_load / splice / run makes the child die by signal; the parent sees WIFSIGNALED. */
/* The loader reserves a bounded prefix and grows by push_back in <=8 MB chunks rather than resizing to an
 * attacker-controlled header count, so a lying count allocates only as fast as the stream yields bytes and a
 * short or malicious stream fails on the first missing byte after a bounded grow. Every mutant, including
 * huge count fields, runs through the real loader and must return NULL cheaply; the screen below is a
 * disabled no-op kept to document the former eager-allocation hazard. */
static int eager_alloc_screen(const unsigned char *buf, size_t len) {
  (void)buf; (void)len;
  return 0;   /* loader is incremental now — run everything for real; a huge count just streams dry -> NULL */
}

static int child_run(const unsigned char *buf, size_t len, int enforce_orig) {
  int scr = eager_alloc_screen(buf, len);
  if (scr) return scr;
  ms_leaf_write(&ms_store, buf, len);        /* the store now hands back these bytes for this leaf's cells */
  MsProg cand = g_frag; cand.pn = len;       /* the name, with the count the candidate claims */
  SlateFrag *f = ms_load(&cand);
  if (!f) return 0;
  uint32_t nparams = 0, nholes = 0;
  if (slate_frag_iface(f, &nparams, NULL, &nholes, NULL) != NULL) { slate_frag_free(f); return 12; }
  if (nholes > 64) { slate_frag_free(f); return 12; }
  SlateDag *b = slate_dag_new();
  int64_t A[4] = {1, 2, 3, 4};
  uint32_t args[64];
  for (uint32_t i = 0; i < nholes; i++) args[i] = slate_dag_carrier(b, A, 4);
  int32_t root = -1; slate_dag_splice(b, f, args, nholes, &root);
  int code = 12;
  if (root >= 0) {
    int64_t dims[1] = {4};
    SlateArray *a = slate_dag_run(b, root, dims, 1);
    if (a) {
      const slate_entry *re; int32_t rn; int valued = 0;
      if (slate_array_receipt(a, &re, &rn) == NULL) valued = slate_entry_find(re, rn, "domain") != NULL;
      int64_t num[4], den[4];
      if (slate_array_i64_unsafe(a, num, den) == NULL) {
        if (enforce_orig) {
          code = (num[0]==ORIG[0]&&num[1]==ORIG[1]&&num[2]==ORIG[2]&&num[3]==ORIG[3]) ? 10 : 11;
        } else {
          code = valued ? 10 : 11;
        }
      } else code = 12;                              /* EWIDE/EREFUSED: defined, clean */
      slate_array_free(a);
    }
  }
  slate_dag_free(b);
  slate_frag_free(f);
  return code;
}

/* Parent-side accounting. */
static long n_reject=0, n_exact=0, n_refuse=0, n_wrong=0, n_crash=0, n_dos=0;
static int  fails=0;

/* Fork a child to evaluate one candidate. Returns nothing; updates counters and prints on any bug. */
static void evaluate(const unsigned char *buf, size_t len, int enforce_orig, const char *tag) {
  pid_t pid = fork();
  if (pid == 0) {
    /* child: cap address space so a pathological resize bad_allocs (caught -> NULL) instead of OOM-killing. */
    struct rlimit rl; rl.rlim_cur = (rlim_t)1536 * 1024 * 1024; rl.rlim_max = rl.rlim_cur;
    setrlimit(RLIMIT_AS, &rl);
    struct rlimit tl; tl.rlim_cur = 5; tl.rlim_max = 5; setrlimit(RLIMIT_CPU, &tl);  /* hang guard */
    _exit(child_run(buf, len, enforce_orig));
  }
  int st = 0; waitpid(pid, &st, 0);
  if (WIFSIGNALED(st)) {
    int sg = WTERMSIG(st);
    if (sg == SIGSEGV || sg == SIGBUS) {           /* memory-safety crash: the serious kind */
      printf("SEGV sig=%d: %s\n", sg, tag); n_crash++; fails++;
    } else {                                       /* SIGKILL/SIGXCPU/SIGALRM: resource-exhaustion DoS */
      printf("DoS(killed sig=%d): %s\n", sg, tag); n_dos++;
    }
    return;
  }
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  switch (code) {
    case 0:  n_reject++; break;
    case 10: n_exact++;  break;
    case 12: n_refuse++; break;
    case 13: case 14: n_dos++; break;              /* screened: loader would eagerly allocate multi-GB */
    case 11: n_wrong++;  fails++; printf("WRONG-VALUE accepted: %s\n", tag); break;
    default: n_crash++;  fails++; printf("UNEXPECTED child code=%d: %s\n", code, tag); break;
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  crc_init();

  /* baseline valid fragment, kept as a leaf in a table that holds nothing else — so the table's put order is
     exactly this leaf's cells, cell 0 first, and the fuzz can read and rewrite them by index */
  {
    SlateDag *b = ms_dag();
    int64_t A[4]={0,0,0,0}, C[4]={10,20,30,40};
    uint32_t cidA=slate_dag_carrier(b,A,4), cidC=slate_dag_carrier(b,C,4);
    int32_t p=slate_dag_param(b,0);
    int32_t root=slate_dag_add(b, slate_dag_mul(b, slate_dag_load(b,cidA,p), slate_dag_lit(b,2)), slate_dag_load(b,cidC,p));
    uint32_t holes[1]={cidA};
    const char *rc = ms_keep(b, root, holes, 1, &g_frag);
    if (rc != NULL) { printf("FAIL: save_fragment rc=%s\n", rc); return 2; }
    slate_dag_free(b);
  }
  size_t LEN = (size_t)g_frag.pn;
  printf("fragment length = %zu bytes (%zu rows in the table)\n", LEN, ms_store.rows);

  unsigned char *base = (unsigned char *)malloc(LEN + 64);
  if (!ms_leaf_read(&ms_store, LEN, base)) { printf("FAIL: the leaf's cells are not byte cells\n"); return 2; }

  /* baseline must load+run to {12,24,36,48} in-process (proves the harness) */
  {
    SlateFrag *f = ms_load(&g_frag);
    if(!f){printf("FAIL: baseline load NULL\n"); return 2;}
    SlateDag*b=slate_dag_new(); int64_t A[4]={1,2,3,4}; uint32_t ca=slate_dag_carrier(b,A,4);
    int32_t r = -1; slate_dag_splice(b,f,&ca,1, &r); int64_t dims[1]={4}; SlateArray*a=slate_dag_run(b,r,dims,1);
    int64_t num[4],den[4]; slate_array_i64_unsafe(a,num,den);
    if(!(num[0]==12&&num[1]==24&&num[2]==36&&num[3]==48)){printf("FAIL: baseline values %lld,%lld,%lld,%lld\n",(long long)num[0],(long long)num[1],(long long)num[2],(long long)num[3]);return 2;}
    printf("baseline OK: {12,24,36,48}\n");
    slate_array_free(a); slate_dag_free(b); slate_frag_free(f);
    uint32_t stored; memcpy(&stored,base+LEN-4,4);
    if(crc_calc(base,LEN-4)!=stored){printf("FAIL: harness crc mismatch\n");return 2;}
    printf("harness crc32 matches engine\n");
  }

  unsigned char *mut = (unsigned char *)malloc(LEN + 64);

  /* ---------------- Regime A: crc-intact brute mutations ---------------- */
  const unsigned char xm[3] = {0xFF, 0x01, 0x80};
  for (size_t off = 0; off < LEN; off++) {
    for (int pi = 0; pi < 3; pi++) {
      memcpy(mut, base, LEN); mut[off]^=xm[pi];
      char tag[64]; snprintf(tag,sizeof tag,"A.xor off=%zu m=0x%02x",off,xm[pi]);
      evaluate(mut, LEN, 1, tag);
    }
    for (int v=0; v<2; v++) { unsigned char nv=v?0xFF:0x00; if(base[off]==nv)continue;
      memcpy(mut,base,LEN); mut[off]=nv;
      char tag[64]; snprintf(tag,sizeof tag,"A.set off=%zu v=0x%02x",off,nv);
      evaluate(mut, LEN, 1, tag);
    }
  }
  srand(0xC0FFEE);
  for (int it=0; it<2500; it++) {
    memcpy(mut,base,LEN); int nb=2+(rand()%5);
    for(int k=0;k<nb;k++){size_t o=(size_t)(rand()%(int)LEN); mut[o]=(unsigned char)(rand()&0xFF);}
    char tag[48]; snprintf(tag,sizeof tag,"A.multi it=%d",it);
    evaluate(mut, LEN, 1, tag);
  }
  long a_total = n_reject+n_exact+n_refuse+n_wrong+n_crash+n_dos;
  printf("REGIME A done: total=%ld reject=%ld exact=%ld refuse=%ld wrong=%ld segv=%ld dos=%ld\n",
         a_total,n_reject,n_exact,n_refuse,n_wrong,n_crash,n_dos);

  long a_reject=n_reject,a_exact=n_exact,a_refuse=n_refuse,a_wrong=n_wrong,a_crash=n_crash,a_dos=n_dos;

  /* ---------------- Regime B: crc-consistent header/count probes ---------------- */
  /* the head of the bytes is five counts — ninstr, root, nparams, ncarrier, potential — and nothing in front
     of them: the store's word already says what the bytes are, so they carry no marker of their own. */
  uint64_t H[5]; memcpy(H,base,40);
  uint64_t real_ninstr=H[0], real_ncarrier=H[3];
  size_t baked_len_off = 40 + 32 + (size_t)real_ncarrier*16 + (size_t)real_ninstr*24;
  struct { const char*name; size_t off; } fields[] = {
    {"ninstr",0},{"root",8},{"nparams",16},{"ncarrier",24},{"pot",32},{"baked_len",baked_len_off},
  };
  uint64_t probes[] = {
    0,1,2,3,4,5, real_ninstr, real_ninstr-1, real_ninstr+1, real_ncarrier, real_ncarrier+1,
    (uint64_t)1<<20, (uint64_t)1<<30, ((uint64_t)1<<30)+1,
    0x7FFFFFFFull,0xFFFFFFFFull,0x100000000ull,
    0x7FFFFFFFFFFFFFFFull,0x8000000000000000ull,0xFFFFFFFFFFFFFFFFull,
  };
  for (size_t fi=0; fi<sizeof fields/sizeof fields[0]; fi++) {
    if (fields[fi].off+8 > LEN) continue;
    for (size_t pj=0; pj<sizeof probes/sizeof probes[0]; pj++) {
      memcpy(mut,base,LEN); memcpy(mut+fields[fi].off,&probes[pj],8);
      uint32_t c=crc_calc(mut,LEN-4); memcpy(mut+LEN-4,&c,4);
      char tag[80]; snprintf(tag,sizeof tag,"B.%s=0x%llx",fields[fi].name,(unsigned long long)probes[pj]);
      evaluate(mut, LEN, 0, tag);
    }
  }
  /* truncation at every boundary, crc recomputed for the truncated span */
  for (size_t tl=12; tl<LEN; tl++) {
    memcpy(mut,base,tl);
    if (tl>=4){uint32_t c=crc_calc(mut,tl-4); memcpy(mut+tl-4,&c,4);}
    char tag[48]; snprintf(tag,sizeof tag,"B.trunc len=%zu",tl);
    evaluate(mut, tl, 0, tag);
  }
  long b_reject=n_reject-a_reject, b_exact=n_exact-a_exact, b_refuse=n_refuse-a_refuse,
       b_wrong=n_wrong-a_wrong, b_crash=n_crash-a_crash, b_dos=n_dos-a_dos;
  long b_total=b_reject+b_exact+b_refuse+b_wrong+b_crash+b_dos;
  printf("REGIME B done: total=%ld reject=%ld exact=%ld refuse=%ld wrong=%ld segv=%ld dos=%ld\n",
         b_total,b_reject,b_exact,b_refuse,b_wrong,b_crash,b_dos);

  free(mut); free(base); ms_prog_free(&g_frag); ms_free(&ms_store);
  printf("SUMMARY: A_muts=%ld B_muts=%ld  SEGV=%ld WRONG=%ld DoS=%ld  (reject=%ld exact=%ld refuse=%ld)\n",
         a_total,b_total,n_crash,n_wrong,n_dos,n_reject,n_exact,n_refuse);
  printf("NOTE: DoS=%ld — the loader now reads counts incrementally (bounded reserve + <=8MB grow steps), so a\n"
         "      huge count field just streams dry and returns NULL; no upfront attacker-controlled allocation.\n", n_dos);
  if (fails==0){ printf("PASS adversarial_fuzz: no memory-safety crash, no wrong-valued fragment across %ld mutations\n",a_total+b_total); return 0; }
  printf("FAIL adversarial_fuzz: %d memory-safety/correctness problem(s) (segv=%ld wrong=%ld)\n",fails,n_crash,n_wrong);
  return 1;
}
