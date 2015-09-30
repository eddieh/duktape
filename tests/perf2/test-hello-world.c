/*
 * Obligatory test.
 */

#include "stdio.h"
#include "time.h"

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     start = clock();

     // begin test
     printf("Hello, world!\n");
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
