#
# Empty while loop test
#

import time

count = 1e8

start = time.clock()

# begin test
while count > 0: count -= 1
# end test

duration = int((time.clock() - start) * 1000)

print duration
