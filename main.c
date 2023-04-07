#include "mpool.h"
#include <stdio.h>

#define KB256 (65536 << 4)
int main() {
  const char arr[KB256];
  bool is_good = pool_init((void *)arr, KB256);
  if (is_good) {
    printf("Hello World!");
  } else {
    goto final;
  }
final:
  return 0;
}