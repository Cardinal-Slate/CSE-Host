/* probe: does a plain constant build (no fragment) also report reading.domain==Q for 42? */
#include <stdio.h>
#include "slate/array.h"
int main(void){
  setvbuf(stdout,NULL,_IONBF,0);
  /* direct 6*7 */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_mul(b,slate_dag_lit(b,6),slate_dag_lit(b,7));
    int64_t d[1]={1}; SlateArray*a=slate_dag_run(b,r,d,1);
    slate_reading rd; slate_array_receipt(a,&rd);
    printf("direct 6*7=42       : domain=%d path=%d mode=%d\n",rd.domain,rd.path,rd.mode);
    slate_array_free(a); slate_dag_free(b); }
  /* direct literal 42 */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_lit(b,42);
    int64_t d[1]={1}; SlateArray*a=slate_dag_run(b,r,d,1);
    slate_reading rd; slate_array_receipt(a,&rd);
    printf("direct lit(42)      : domain=%d path=%d mode=%d\n",rd.domain,rd.path,rd.mode);
    slate_array_free(a); slate_dag_free(b); }
  /* direct add 40+2 */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_add(b,slate_dag_lit(b,40),slate_dag_lit(b,2));
    int64_t d[1]={1}; SlateArray*a=slate_dag_run(b,r,d,1);
    slate_reading rd; slate_array_receipt(a,&rd);
    printf("direct 40+2         : domain=%d path=%d mode=%d\n",rd.domain,rd.path,rd.mode);
    slate_array_free(a); slate_dag_free(b); }
  /* param-based build (has a real grid coord) */
  { SlateDag*b=slate_dag_new();
    int32_t r=slate_dag_add(b,slate_dag_param(b,0),slate_dag_lit(b,10));
    int64_t d[1]={3}; SlateArray*a=slate_dag_run(b,r,d,1);
    slate_reading rd; slate_array_receipt(a,&rd);
    printf("param p0+10 grid3   : domain=%d path=%d mode=%d\n",rd.domain,rd.path,rd.mode);
    slate_array_free(a); slate_dag_free(b); }
  return 0;
}
