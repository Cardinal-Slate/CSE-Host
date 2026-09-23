/* Targeted probe: does slate_frag_load allocate an attacker-controlled count ahead of the bytes it actually
 * has? A program is a leaf named by its word and nothing else, so there is no count outside the bytes to
 * claim four billion of: the load walks the store, cell 0, cell 1, …, and stops at the first cell nobody has,
 * which means a store that wants it to allocate a lot must actually hold a lot — amplification 1:1, by
 * construction. The counts an attacker still reaches are the ones in the head of the bytes themselves, which
 * only a lying store can move, and those are caught by the crc over the body — the whole reason the bytes
 * still carry one now that no file does.
 *
 * Probed here, with peak RSS measured either side: the leaf as kept (control), a lying store that inflates a
 * count inside the bytes, and a store that lost a cell so the walk stops early. A small leaf driving a large
 * resident set would be a memory-amplification DoS in the decoder. */
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
  size_t LEN=ms_leaf_len(&ms_store);    /* what a walk finds: nothing else says how long the leaf is */
  printf("fragment length = %zu bytes, kept as %zu rows (the records are the count)\n", LEN, ms_store.rows);

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

  /* attack 1 is gone: there is no count beside the word to claim. What used to be "the name says four
     billion bytes" is now "the store would have to hold four billion cells", which costs the attacker exactly
     what it costs the reader. What is left is the store losing a cell: the walk stops there and the parser
     must refuse the short program rather than run it. */
  { ms_leaf_stop_after(&ms_store, LEN/2);
    SlateFrag*f=ms_load(&frag); printf("attack (walk stops at %zu of %zu) load -> %s\n", LEN/2, LEN, f?"LOADED(bug)":"null(rejected)"); if(f) slate_frag_free(f);
    ms_leaf_stop_after(&ms_store, LEN); }

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
  printf("AMPLIFICATION: ~%.0fx  (resident bytes per leaf byte the store actually holds)\n", amp);
  if (after-before > 100*1000*1000) printf("VERDICT: memory-amplification CONFIRMED (>100MB from a %zu-byte leaf)\n", LEN);
  else printf("VERDICT: no significant amplification\n");
  free(base); ms_prog_free(&frag); ms_free(&ms_store);
  return 0;
}
