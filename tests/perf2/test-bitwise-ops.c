/*
 * Simple bitwise operation test.
 */

#include "stdio.h"
#include "time.h"

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     int x, y, z;

     x = 0xdeadbeef;
     y = 0xf;

     start = clock();

     // begin test
     for (int i = 0; i < 1e7; i++) {
          z = x & y;
          z = x | y;
          z = x ^ y;
          z = x << y;
          z = x >> y;
          z = (unsigned)x >> (unsigned)y;
          z = ~x;
     }
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
