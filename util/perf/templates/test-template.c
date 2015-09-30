/*
 * {{TEST_DESCRIPTION}}
 */

#include "stdio.h"
#include "time.h"

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     // setup test

     start = clock();

     // begin test
     // ...
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
