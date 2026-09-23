/* Diagnostic: replay one iteration of the fuzzer's multi-byte mutation stream and watch the load. A program
 * is a leaf, so the fuzz unit is the store's rows: each cell of the leaf is one byte of the program, and a
 * mutation is the store handing back a different byte. Prints what was changed, the head of the mutated
 * bytes, and how long the load took — a load that hangs or explodes on a hostile store is what this is for.
 *
 * SPDX-License-Identifier: MIT OR Apache-2.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/resource.h>
#include <time.h>
#include "slate/slate.h"
#include "../memstore.h"

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }

int main(int argc, char**argv){
  setvbuf(stdout,NULL,_IONBF,0);
  int target = argc>1 ? atoi(argv[1]) : 3086;
  struct rlimit rl; rl.rlim_cur=(rlim_t)900*1024*1024; rl.rlim_max=rl.rlim_cur; setrlimit(RLIMIT_AS,&rl);
  SlateDag *b=ms_dag();
  int64_t A[4]={0,0,0,0}, C[4]={10,20,30,40};
  uint32_t cidA=slate_dag_carrier(b,A,4), cidC=slate_dag_carrier(b,C,4);
  int32_t p=slate_dag_param(b,0);
  int32_t root=slate_dag_add(b, slate_dag_mul(b, slate_dag_load(b,cidA,p), slate_dag_lit(b,2)), slate_dag_load(b,cidC,p));
  MsProg frag={0}; uint32_t holes[1]={cidA}; ms_keep(b,root,holes,1,&frag); slate_dag_free(b);
  size_t LEN=(size_t)frag.pn;
  unsigned char *base=(unsigned char*)malloc(LEN);
  if(!ms_leaf_read(&ms_store,LEN,base)){ printf("the leaf's cells are not byte cells\n"); return 2; }
  unsigned char *mut=(unsigned char*)malloc(LEN);

  /* replay the exact RNG stream of the fuzzer's multi-byte loop up to `target` */
  srand(0xC0FFEE);
  int nb=0; size_t offs[16]; unsigned char vals[16];
  for(int it=0; it<=target; it++){
    memcpy(mut,base,LEN);
    nb=2+(rand()%5);
    for(int k=0;k<nb;k++){ size_t o=(size_t)(rand()%(int)LEN); unsigned char v=(unsigned char)(rand()&0xFF); mut[o]=v; offs[k]=o; vals[k]=v; }
    if(it<target) continue;
  }
  printf("target it=%d LEN=%zu nb=%d changes:", target, LEN, nb);
  for(int k=0;k<nb;k++) printf(" [%zu]=0x%02x", offs[k], vals[k]);
  printf("\n");
  uint64_t H[5]; memcpy(H,mut,40);
  printf("mut head: ninstr=%llu root=%llu nparams=%llu ncarrier=%llu pot=%llu\n",
    (unsigned long long)H[0],(unsigned long long)H[1],(unsigned long long)H[2],
    (unsigned long long)H[3],(unsigned long long)H[4]);

  ms_leaf_write(&ms_store, mut, LEN);       /* the store now hands these bytes back for the leaf's cells */
  alarm(15);
  double t0=now();
  SlateFrag *f=ms_load(&frag);
  double t1=now();
  printf("LOAD took %.3f s -> %s\n", t1-t0, f?"loaded":"NULL");
  if(f){
    uint32_t np=0,nh=0; slate_frag_iface(f,&np,NULL,&nh,NULL);
    printf("  nparams=%u nholes=%u\n", np, nh);
    slate_frag_free(f);
  }
  free(mut); free(base); ms_prog_free(&frag); ms_free(&ms_store);
  return 0;
}
