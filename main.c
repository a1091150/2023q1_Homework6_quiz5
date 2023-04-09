#include <stdio.h>
#include <stdlib.h>
#include "mpool.h"

#define KB256 (32 << 4)
int main()
{
    char *arr = malloc(sizeof(char) * KB256);
    bool is_good = pool_init((void *) arr, KB256);
    int *a1 = pool_malloc(sizeof(int) * 8);
    int *a2 = pool_malloc(sizeof(int) * 8);
    pool_free(a1);
    pool_free(a2);
    return 0;
}