/*
 * Fibonacci test, exercises recursion.
 */

#include "stdio.h"
#include "time.h"

int fibonacci (int n) {
     return n <= 1 ? n : fibonacci(n - 2) + fibonacci(n - 1);
}

int main(int argc, char **argv) {
     int msec;
     clock_t start, duration;

     start = clock();

     // begin test
     fibonacci(35);
     // end test

     duration = clock() - start;
     msec = duration * 1000 / CLOCKS_PER_SEC;

     printf("%d\n", msec);

     return 0;
}
