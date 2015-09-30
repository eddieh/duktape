#
# Basic array read test
#

import time

var = 0
array = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

start = time.clock()

# begin test
for x in range(0, int(1e8)): var = array[7]
# end test

duration = int((time.clock() - start) * 1000)

print duration
