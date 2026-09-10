#include "multicast.h"

int Verbose = 0;

int main(void)
{
    return wait_for_lan() ? 0 : 1;
}
