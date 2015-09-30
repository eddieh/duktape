#
# Build string with concatenation.
#

import time

start = time.clock()

# begin test
for i in xrange(int(5e3)):
    t = ''
    for j in xrange(int(1e4)):
        t += 'x'
# end test

duration = int((time.clock() - start) * 1000)

print duration
