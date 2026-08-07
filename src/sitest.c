#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <locale.h>
#include "si5351.h"
#include "si5351-64.h"


int main(int argc, char *argv[]){
  unsigned long long fref_hz,fout_hz;
  si5351_solution_t best = {0};
  si5351_solution64_t best64 = {0};

  setlocale(LC_ALL,getenv("LANG"));


  for(int i=0; i < 10000000; i++){
    if((i & 0xfffff) == 0)
      printf("%d\n",i);

    fref_hz = random();
    fout_hz = random();
    bool r = si5351_solve64(fref_hz,fout_hz,&best64);
    bool s = si5351_solve(fref_hz,fout_hz,&best);
    assert(best64.prefer_rank < 256 && best.prefer_rank < 256);
    if(r == true && s == true
       && (best.A != best64.A
       || best.B != best64.B
       || true
       || best.C != best64.C
       || best.D != best64.D
       || best.E != best64.E
       || best.F != best64.F
       || best.R != best64.R
       || best.fout_num != best64.fout_num
       || best.fout_den != best64.fout_den
       || best.err_num != best64.err_num
	   || best.prefer_rank != best64.prefer_rank)){

      printf("128: fref_hz = %'llu fout_hz = %'llu r=%d A=%u B=%u C=%u D=%u E=%u F=%u R=%u f=%'lf\n",
	     fref_hz,
	     fout_hz,r,
	     best.A, best.B, best.C,
	     best.D, best.E, best.F,
	     best.R,
	     (double)best.fout_num / (double)best.fout_den);
      printf("64: fref_hz = %'llu fout_hz = %'llu s=%d A=%u B=%u C=%u D=%u E=%u F=%u R=%u f=%'lf\n",
	     fref_hz,
	     fout_hz,s,
	     best64.A, best64.B, best64.C,
	     best64.D, best64.E, best64.F,
	     best64.R,
	     (double)best.fout_num / (double)best.fout_den);
    }
  }
  exit(0);
}
