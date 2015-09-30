#
# Obligatory test.
#

import time

start = time.clock()

# begin test
print 'Hello, world!'
# end test

duration = int((time.clock() - start) * 1000)

print duration
