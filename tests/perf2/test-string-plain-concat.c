/*
 * Build string with concatenation.
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "time.h"

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     char *t = malloc(50000000);

     start = clock();

     // begin test
     for (int i = 0; i < 5000; i++) {
          for (int j = 0; j < 10000; j++) {
               t[(i * 10000) + j] = 'x';
          }
     }
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
