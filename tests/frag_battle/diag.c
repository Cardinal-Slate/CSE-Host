/* Reproduce a specific flagged multi-byte mutation (RNG is deterministic from the seed) and isolate where the
 * time goes: load alone, then splice, then run. Prints the mutated header fields and changed offsets. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include <time.h>
#include "slate/array.h"
#include "slate/stream.h"

typedef struct { unsigned char *buf; size_t len, cap; } MemSink;
static uint64_t mem_sink(const void *bytes, uint64_t n, void *user){MemSink*m=(MemSink*)user; if(m->len+n>m->cap){m->cap=(m->len+n)*2+64;m->buf=(unsigned char*)realloc(m->buf,m->cap);} memcpy(m->buf+m->len,bytes,(size_t)n); m->len+=(size_t)n; return n;}
typedef struct { const unsigned char*buf; size_t len,pos; } MemSrc;
static uint64_t mem_src(void*bytes,uint64_t n,void*user){MemSrc*s=(MemSrc*)user; if(s->pos+n>s->len)return 0; memcpy(bytes,s->buf+s->pos,(size_t)n); s->pos+=(size_t)n; return n;}

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

int main(int argc, char**argv){
  setvbuf(stdout,NULL,_IONBF,0);
  int target = argc>1 ? atoi(argv[1]) : 3086;
  struct rlimit rl; rl.rlim_cur=(rlim_t)900*1024*1024; rl.rlim_max=rl.rlim_cur; setrlimit(RLIMIT_AS,&rl);
  SlateDag *b=slate_dag_new();
  int64_t A[4]={0,0,0,0}, C[4]={10,20,30,40};
  uint32_t cidA=slate_dag_carrier(b,A,4), cidC=slate_dag_carrier(b,C,4);
  int32_t p=slate_dag_param(b,0);
  int32_t root=slate_dag_add(b, slate_dag_mul(b, slate_dag_load(b,cidA,p), slate_dag_lit(b,2)), slate_dag_load(b,cidC,p));
  MemSink frag={0}; uint32_t holes[1]={cidA}; slate_dag_save_fragment(b,root,holes,1,mem_sink,&frag); slate_dag_free(b);
  size_t LEN=frag.len;
  unsigned char *mut=(unsigned char*)malloc(LEN);

  /* replay the exact RNG stream of the fuzzer's multi-byte loop up to `target` */
  srand(0xC0FFEE);
  int nb=0; size_t offs[16]; unsigned char vals[16];
  for(int it=0; it<=target; it++){
    memcpy(mut,frag.buf,LEN);
    nb=2+(rand()%5);
    for(int k=0;k<nb;k++){ size_t o=(size_t)(rand()%(int)LEN); unsigned char v=(unsigned char)(rand()&0xFF); mut[o]=v; offs[k]=o; vals[k]=v; }
    if(it<target) continue;
  }
  printf("target it=%d LEN=%zu nb=%d changes:", target, LEN, nb);
  for(int k=0;k<nb;k++) printf(" [%zu]=0x%02x", offs[k], vals[k]);
  printf("\n");
  uint64_t H[8]; memcpy(H,mut,64);
  printf("mut header: magic=0x%llx ver=%llu flags=%llu ninstr=%llu root=%llu nparams=%llu ncarrier=%llu pot=%llu\n",
    (unsigned long long)H[0],(unsigned long long)H[1],(unsigned long long)H[2],(unsigned long long)H[3],
    (unsigned long long)H[4],(unsigned long long)H[5],(unsigned long long)H[6],(unsigned long long)H[7]);

  alarm(15);
  double t0=now();
  MemSrc s={mut,LEN,0};
  SlateFrag *f=slate_frag_load(mem_src,&s);
  double t1=now();
  printf("LOAD took %.3f s -> %s\n", t1-t0, f?"loaded":"NULL");
  if(f){
    uint32_t np=0,nh=0; slate_frag_iface(f,&np,NULL,&nh,NULL);
    printf("  nparams=%u nholes=%u\n", np, nh);
    slate_frag_free(f);
  }
  free(mut); free(frag.buf);
  return 0;
}
