/*
 * Basic property read performance.
 */

#include "stdio.h"
#include "time.h"

typedef struct obj_t {
     int xxx1;
     int xxx2;
     int xxx3;
     int xxx4;
     int foo;
} obj_t;

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     obj_t obj = {
          .xxx1 = 1,
          .xxx2 = 2,
          .xxx3 = 3,
          .xxx4 = 4,
          .foo = 123
     };
     int ign;

     start = clock();

     // begin test
     for (int i = 0; i < 1e8; i++) {
          ign = obj.foo;
     }
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
