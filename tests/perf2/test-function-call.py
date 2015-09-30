#
# Test basic function call.
#

import time

def f(): return

start = time.clock()

# begin test
for i in range(0, int(1e8)): f()
# end test

duration = int((time.clock() - start) * 1000)

print duration
