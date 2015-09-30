/*
 * Basic array read test
 */

#include "stdio.h"
#include "time.h"

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     int var;
     int a[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

     start = clock();

     // begin test
     for (int i = 0; i < 1e8L; i++) var = a[7];
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
