/*
 *  Empty for loop performance alternate 2
 */

if (typeof window == 'undefined') window = {}
if (!window.performance) window.performance = {}
window.performance.now = window.performance.now || Date.now

if (typeof print != 'undefined') log = print
else log = console.log

var start, duration

start = window.performance.now()

// begin test
for (var i = 0; i < 1e8; i++) {}
// end test

duration = window.performance.now() - start

log(duration)
