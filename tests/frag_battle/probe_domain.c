/* probe: does a plain constant build (no fragment) also report reading.domain==Q for 42? */
#include <stdio.h>
#include "slate/slate.h"
/* the text of a reading entry, for printing (a utf8 entry's bytes are a C string) */
static const char *ent(const slate_entry *e, int32_t n, const char *name) {
  const slate_entry *f = slate_entry_find(e, n, name); return f ? (const char *)f->bytes : "-";
}
int main(void){
  setvbuf(stdout,NULL,_IONBF,0);
  /* direct 6*7 */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_mul(b,slate_dag_lit(b,6),slate_dag_lit(b,7));
    int64_t d[1]={1}; SlateArray*a=slate_dag_run(b,r,d,1);
    const slate_entry *re; int32_t rn; slate_array_receipt(a,&re,&rn);
    printf("direct 6*7=42       : domain=%s path=%s mode=%s\n",ent(re,rn,"domain"),ent(re,rn,"path"),ent(re,rn,"mode"));
    slate_array_free(a); slate_dag_free(b); }
  /* direct literal 42 */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_lit(b,42);
    int64_t d[1]={1}; SlateArray*a=slate_dag_run(b,r,d,1);
    const slate_entry *re; int32_t rn; slate_array_receipt(a,&re,&rn);
    printf("direct lit(42)      : domain=%s path=%s mode=%s\n",ent(re,rn,"domain"),ent(re,rn,"path"),ent(re,rn,"mode"));
    slate_array_free(a); slate_dag_free(b); }
  /* direct add 40+2 */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_add(b,slate_dag_lit(b,40),slate_dag_lit(b,2));
    int64_t d[1]={1}; SlateArray*a=slate_dag_run(b,r,d,1);
    const slate_entry *re; int32_t rn; slate_array_receipt(a,&re,&rn);
    printf("direct 40+2         : domain=%s path=%s mode=%s\n",ent(re,rn,"domain"),ent(re,rn,"path"),ent(re,rn,"mode"));
    slate_array_free(a); slate_dag_free(b); }
  /* param-based build (has a real grid coord) */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_add(b,slate_dag_param(b,0),slate_dag_lit(b,10));
    int64_t d[1]={3}; SlateArray*a=slate_dag_run(b,r,d,1);
    const slate_entry *re; int32_t rn; slate_array_receipt(a,&re,&rn);
    printf("param p0+10 grid3   : domain=%s path=%s mode=%s\n",ent(re,rn,"domain"),ent(re,rn,"path"),ent(re,rn,"mode"));
    slate_array_free(a); slate_dag_free(b); }
  return 0;
}
