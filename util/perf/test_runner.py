#
# This runs all the performances tests and logs the output
#

from datetime import datetime
import argparse
import json
import os
import pipes
import re
import shlex
import subprocess

perf2_path = 'tests/perf2'

# list of extentions we want to run
extensions = ['c', 'js', 'py']

duktape = '../../duk'
jsc = '/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Resources/jsc'
node = 'node'
python = 'python'

# extension to executable map
executables = {
    'js': [duktape, jsc, node],
    'py': [python]
}

# files to skip
skip = []

save_results_path = 'results-YYYY-MM-DD-HHMM.json'
save_results_dir = '../../util/perf/results'

test_data = []
last_base = ''

# assume we're running from the base of the project
os.chdir(perf2_path)

# TODO: option to only run JavaScript tests
# TODO: option to run C tests

# TODO: option to skip excessively slow tests
# these are really slow at the moment
skip.append('test-string-plain-concat.c')
skip.append('test-string-plain-concat.js')
skip.append('test-string-plain-concat.py')

parser = argparse.ArgumentParser(description='Run the performance test suite.')
parser.add_argument('--save-results', dest='save_path', nargs='?', const=True, default=False,
                    help='save the results to a file (default results-YYYY-MM-DD-HHMM.json)')
parser.add_argument('--only-duktape', action='store_true')
parser.add_argument('--iterations', type=int, default=5)
args = parser.parse_args()

if args.only_duktape:
    executables = { 'js': [duktape] }

# number of iterations to run and average
iterations = args.iterations

# get the version of each exe
vers = {}

vargs = shlex.split('/usr/libexec/PlistBuddy -c "Print :CFBundleVersion" /System/Library/Frameworks/JavaScriptCore.framework/Resources/Info.plist')
output = subprocess.check_output(vargs)
vers['jsc'] = output.strip()

vargs = shlex.split('git rev-parse HEAD')
output = subprocess.check_output(vargs)
vers['duk'] = output.strip()[:8]

# capture stderr and stdout because python
output = subprocess.check_output(['python', '--version'], stderr=subprocess.STDOUT)
vers['python'] = output.strip()[7:]

output = subprocess.check_output(['node', '--version'])
vers['node'] = output.strip()[1:]

for t in os.listdir('.'):
    if t in skip: continue

    base, ext = os.path.splitext(t)
    ext = ext.strip('.')

    if not ext in executables: continue

    test_runs = {}

    print 'Running: ' + t
    for exe in executables[ext]:
        timesum = 0
        avg = 0
        exebase = os.path.basename(exe)
        for x in xrange(iterations):
            print '[' + str(x + 1) + '] ' + exebase + ' ' + t,
            output = subprocess.check_output([exe, t])
            output = re.search('\d+$', output).group(0)
            # TODO: handle errors
            timesum += int(output)
            print output
        avg = timesum / iterations
        print 'Average: ' + str(avg) + '\n'

        test_runs[exebase + ' ' + vers[exebase]] = avg

    if not last_base == base:
        last_base = base
        test_data.append({ 'name': base, 'runs': test_runs })
    else:
        test_data[-1]['runs'].update(test_runs)

# save the results
if args.save_path:
    ext = 'js'

    if type(args.save_path) is bool:
        save_results_path = datetime.today().strftime('results-%Y-%m-%d-%H%M.' + ext)
    else:
        save_results_path = args.save_path

    save_results_path = os.path.join(save_results_dir, save_results_path)

    # TODO: save as *.js and optionally as *.json
    if ext == 'js':
        file_prefix = 'var data = '
    else:
        file_prefix = ''

    with open(save_results_path, 'w') as save_file:
        save_file.write(file_prefix + json.dumps(test_data))
