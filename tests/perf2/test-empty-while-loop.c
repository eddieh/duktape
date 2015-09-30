/*
 * Empty while loop test.
 */

#include "stdio.h"
#include "time.h"

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     int i = 1e8L;

     start = clock();

     // begin test
     while (--i) ;
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
