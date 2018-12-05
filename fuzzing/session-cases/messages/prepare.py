#!/bin/python3

import glob
import os
import sys

def process(input, output):
    for line in input.readlines():
        output.write("{0}\n\n{1}".format(len(line), line[:-1]))

def main():
    directory = os.path.dirname(__file__)
    for path in glob.glob(os.path.join(directory, "*.txt")):
        sys.stderr.write("{0}\n".format(path))
        with open(path, 'r') as input:
            with open(os.path.join(directory, "..", os.path.basename(path)), 'w') as output:
                process(input, output)
    return 0

if __name__ == '__main__':
    sys.exit(main())
