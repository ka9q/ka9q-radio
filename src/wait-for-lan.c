#include "multicast.h"
#include <stdlib.h>

int Verbose = 0;
int main(int argc,char *argv[]){
  int ntries = 30;
  if(argc > 1)
    ntries = strtol(argv[1],NULL,10);
  return wait_for_lan(ntries) ? 0 : 1;
}
