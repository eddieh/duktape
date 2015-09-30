#
# Simple bitwise operation test.
#

import time

x = 0xdeadbeef
y = 0xf

start = time.clock()

# begin test
for i in range(0, int(1e7)):
    z = x & y
    z = x | y
    z = x ^ y
    z = x << 2
    z = x >> y
    z = x >> y
    z = ~x
# end test

duration = int((time.clock() - start) * 1000)

print duration
