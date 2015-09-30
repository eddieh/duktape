#
# Test empty for loop
#

import time

start = time.clock()

# begin test
for x in range(0, int(1e8)): pass
# end test

duration = int((time.clock() - start) * 1000)

print duration
