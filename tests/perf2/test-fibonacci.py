#
# Fibonacci test, exercises recursion.
#

import time

def fibonacci(n):
    if n <= 1: return n
    return fibonacci(n - 2) + fibonacci(n - 1)

start = time.clock()

# begin test
fibonacci(35)
# end test

duration = int((time.clock() - start) * 1000)

print duration
