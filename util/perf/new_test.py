#
# Create a new performance test from the templates.
#
# The tests and templates are in the tests/perf2 directory.
#

import argparse
import os
import re

perf2_path = 'tests/perf2'
template_dir = '../../util/perf/templates'
template_basename = 'test-template'
template_extensions = ['c', 'js', 'py']
test_name = ''
test_description = ''

# assume we're running from the base of the project
os.chdir(perf2_path)

# read command line args
parser = argparse.ArgumentParser(description='Create a new performance test.')
parser.add_argument('--name')
parser.add_argument('--description')
args = parser.parse_args()

test_name = args.name or raw_input('Name: ')
if not test_name:
    print('Performance tests require a name.')
    exit(1)

test_description = args.description or raw_input('Description: ')
if not test_description:
    print('Performance tests require a description.')
    exit(1)

# make test basename from test_name (lower case, prefixed with 'test-',
# hyphenated)
test_basename = test_name.strip().lower()

if (test_basename.find('test ') != 0 and
    test_basename.find('test-') != 0 and
    test_basename.find('test_') != 0):
    test_basename = 'test-' + test_basename

test_basename = re.sub(r'[^a-zA-Z0-9]', '-', test_basename)
test_basename = re.sub(r'\-+', '-', test_basename)
test_basename = test_basename.strip('-')

for ext in template_extensions:
    test_path = test_basename + '.' + ext

    # complain about already existing test and bail
    if os.path.exists(test_path):
        print('Performance test ' + test_name + ' already exists.')
        exit(1)

    template_path = template_basename + '.' + ext
    template_path = os.path.join(template_dir, template_path)
    with open(template_path, 'r') as template_file:
        template_text = template_file.read()

        # replace the macros in the template text
        test_text = template_text.replace('{{TEST_DESCRIPTION}}', test_description)

        # write the file with test path
        with open(test_path, 'w') as test_file:
            test_file.write(test_text)
