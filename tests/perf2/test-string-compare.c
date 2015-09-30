/*
 * Test string comparison.
 */

#include "stdio.h"
#include "stdlib.h"
#include "time.h"

#define FILL(x, y) malloc(x); for (int i = 0; i < x; i++) y[i] = 'x';

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     char *a = FILL(0, a);
     char *b = FILL(1, b);
     char *c = FILL(16, c);
     char *d = FILL(256, d);
     char *e = FILL(4096, e);
     char *f = FILL(65536, f);
     char *g = FILL(1048576, g);

     start = clock();

     // begin test
     for (int i = 0; i < 1e7; i++) {
          // pointer compare or string compare?
     }
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
