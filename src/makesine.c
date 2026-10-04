// Generate sine lookup table for NCO
#include <stdio.h>
#include <math.h>
#include "misc.h"
#include "nco.h"

int main(int argc,char *argv[]){
  (void)argc;
  (void)argv;

  float NCO_lookup[TAB_SIZE+1];

  for(int i=0; i <= TAB_SIZE; i++)
    NCO_lookup[i] = sinpi(0.5 * (double)i/TAB_SIZE);

  printf("const float NCO_lookup[] = {\n");
  for(int i=0; i <= TAB_SIZE; i++)
    printf("%af,\n",(float)NCO_lookup[i]); // preserve exact bits with hex format
  printf("};\n");
  exit(0);
}
