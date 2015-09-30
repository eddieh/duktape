#
# Basic array write test
#

import time

a = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

start = time.clock()

# begin test
for i in range(0, int(1e8)): a[7] = 256
# end test

duration = int((time.clock() - start) * 1000)

print duration
