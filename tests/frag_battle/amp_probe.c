/* Targeted probe: does slate_frag_load allocate an attacker-controlled count ahead of the bytes it actually
 * has? A program is a leaf now, so the counts an attacker reaches are (1) the byte count in the name — how
 * many cells the load asks the store for — and (2) the counts in the head of those bytes, which only a lying
 * store can move. Both are probed here, and peak RSS is measured either side: a tiny name driving a large
 * resident set would be a memory-amplification DoS in the decoder.
 *
 * (1) is the new one: the count is a plain argument, so it costs nothing to claim four billion bytes. The
 * load must give that up on the first cell the store does not hold. (2) is caught by the crc over the body,
 * which is the whole reason the bytes still carry one now that no file does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/resource.h>
#include "slate/slate.h"
#include "../memstore.h"

static uint32_t T[256];
static void crc_init(void){ for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c>>1)^(0xEDB88320u&(uint32_t)(-(int32_t)(c&1)));T[i]=c;} }
static uint32_t crc_calc(const uint8_t*p,size_t n){uint32_t c=0xFFFFFFFFu;for(size_t i=0;i<n;i++)c=T[(c^p[i])&0xFF]^(c>>8);return c^0xFFFFFFFFu;}

static long maxrss_bytes(void){ struct rusage r; getrusage(RUSAGE_SELF,&r); return (long)r.ru_maxrss; /* Darwin: bytes */ }

int main(void){
  crc_init();
  SlateDag *b = ms_dag();
  int64_t A[4]={0,0,0,0}, C[4]={10,20,30,40};
  uint32_t cidA=slate_dag_carrier(b,A,4), cidC=slate_dag_carrier(b,C,4);
  int32_t p=slate_dag_param(b,0);
  int32_t root=slate_dag_add(b, slate_dag_mul(b, slate_dag_load(b,cidA,p), slate_dag_lit(b,2)), slate_dag_load(b,cidC,p));
  MsProg frag={0}; uint32_t holes[1]={cidA};
  ms_keep(b, root, holes, 1, &frag);
  slate_dag_free(b);
  size_t LEN=(size_t)frag.pn;
  printf("fragment length = %zu bytes, kept as %zu rows\n", LEN, ms_store.rows);

  unsigned char *base=(unsigned char*)malloc(LEN);
  if(!ms_leaf_read(&ms_store,LEN,base)){ printf("FAIL: the leaf's cells are not byte cells\n"); return 2; }
  uint64_t H[5]; memcpy(H,base,40);
  uint64_t ninstr=H[0], ncarrier=H[3];
  size_t baked_len_off = 40 + 32 + (size_t)ncarrier*16 + (size_t)ninstr*24;
  printf("ninstr=%llu ncarrier=%llu baked_len_off=%zu (leaf has %zu)\n",
         (unsigned long long)ninstr,(unsigned long long)ncarrier,baked_len_off,LEN);

  long before = maxrss_bytes();
  printf("maxrss before load: %ld bytes\n", before);

  /* control: the leaf as it was kept */
  { SlateFrag*f=ms_load(&frag); printf("control load -> %s\n", f?"loaded":"null"); if(f) slate_frag_free(f); }
  printf("maxrss after control: %ld bytes\n", maxrss_bytes());

  /* attack 1: the name claims four billion bytes. Nothing was kept under those cells, so the load must give
     up on the first one it asks for. */
  { MsProg big = frag; big.pn = 0xFFFFFFFFull;
    SlateFrag*f=ms_load(&big); printf("attack (count=2^32-1) load -> %s\n", f?"loaded":"null(rejected)"); if(f) slate_frag_free(f); }

  /* attack 2: a lying store — the baked-data length word says 50,000,000 int64 cells (= 400MB), the crc fixed
     so integrity would pass if the crc were all that stood between the count and the allocator. */
  if (baked_len_off + 8 <= LEN) {
    unsigned char *mut=(unsigned char*)malloc(LEN);
    memcpy(mut,base,LEN);
    uint64_t big=50000000ull; memcpy(mut+baked_len_off,&big,8);
    uint32_t c=crc_calc(mut,LEN-4); memcpy(mut+LEN-4,&c,4);
    printf("attack: baked_len=%llu (=> %.0f MB int64), the leaf still %zu bytes\n",
           (unsigned long long)big, big*8.0/1e6, LEN);
    ms_leaf_write(&ms_store, mut, LEN);
    SlateFrag*f=ms_load(&frag); printf("attack (lying store) load -> %s\n", f?"loaded":"null(rejected)"); if(f) slate_frag_free(f);
    free(mut);
  } else {
    printf("attack: the baked-data length word is past this leaf; count probe only\n");
  }

  long after = maxrss_bytes();
  printf("maxrss after attack: %ld bytes  (delta = %ld bytes = %.1f MB)\n", after, after-before, (after-before)/1e6);
  double amp = (double)(after-before)/(double)LEN;
  printf("AMPLIFICATION: ~%.0fx  (resident bytes per leaf byte)\n", amp);
  if (after-before > 100*1000*1000) printf("VERDICT: memory-amplification CONFIRMED (>100MB from a %zu-byte leaf)\n", LEN);
  else printf("VERDICT: no significant amplification\n");
  free(base); ms_prog_free(&frag); ms_free(&ms_store);
  return 0;
}
