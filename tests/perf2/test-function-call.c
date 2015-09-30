/*
 * Test basic function call.
 */

#include "stdio.h"
#include "time.h"

void f () { return; }

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     start = clock();

     // begin test
     for (int i = 0; i < 1e8; i++) f();
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
