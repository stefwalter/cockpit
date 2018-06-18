#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# This file is part of Cockpit.
#
# Copyright (C) 2017 Slavek Kabrda
#
# Cockpit is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.
#
# Cockpit is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with Cockpit; If not, see <http://www.gnu.org/licenses/>.

# WARNING: As you change this code increment this version number so
# the machine learning model uses a new place to store the model
VERSION = 3

# This code extracts features from log items. In particular it normalizes
# and exracts the log.
#
# TODO: We could weight log lines using TF-IDF, but that would require
# a distance function that could apply that weight between lines. The
# NCD distance we use cannot do that.

import calendar
import json
import re
import sys
import time

import sklearn.feature_extraction.text

# Ignore lines that appear in at least this fraction of logs
IGNORE_THRESHHOLD = 0.07

# Choose only one out of every N tracked items. These have
# already been manually "clustered" elsewhere, and we only need
# some cluster seeds
TRACKER_SPARSE = 100

NUMBERS = (
    # 512 bit hashes
    ("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", re.compile('[0-9a-f]{128}')),
    ("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", re.compile('[0-9A-F]{128}')),

    # 256 bit hashes
    ("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", re.compile('/[0-9a-f]{64}')),
    ("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", re.compile('/[0-9A-F]{64}')),

    # 160 bit hashes
    ("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", re.compile('[0-9a-f]{40}')),
    ("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", re.compile('[0-9A-F]{40}')),

    # 128 bit hashes
    ("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx", re.compile('[0-9a-f]{32}')),
    ("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", re.compile('[0-9A-F]{32}')),

    # GUIDs
    ('xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx',
        re.compile('[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}')),

    # Digits
    ('000', re.compile('\d+'))
)

# Various features extracted
FEATURE_LOG = 0                # string: The normalized and collapsed log extracted
FEATURE_ITEMS = 1              # [ number, number ]: Unique indexes of the items
FEATURE_WHEN = 2               # [ start, end ]: The time frame since epoch at which test was run
FEATURE_NAME = 3               # string: The name of the test run
FEATURE_CONTEXT = 4            # string: The context in which the test is run
FEATURE_TRACKER = 5            # string: A tracker issue for this

TOTAL = { }

# Return already tokenized data
def noop(value):
    return value

# Select which items we want to operate on.
#
# Because we have so many tracked failures, we need to only bring
# some of those into our clustering algorithm. We can assume that
# these are already clusters
tracked = { }
def select(item):
    if item.get("status") != "failure":
        return False
    tracker = item.get("tracker")
    if not tracker:
        return True
    count = tracked[tracker] = tracked.get(tracker, 0) + 1
    return count % TRACKER_SPARSE == 0 # Only every Nth for tracked failures

# The actual feature extractor. Currently only extracts a
# normalized log from each item. By using fit() you can train
# the extractor to ignore frequently found lines.
class Extractor():
    def __init__(self, verbose=False):
        self.extract = sklearn.feature_extraction.text.CountVectorizer(
            analyzer='word',
            tokenizer=noop,
            lowercase=False,
            max_df=IGNORE_THRESHHOLD)
        self.verbose = verbose

    @staticmethod
    def tokenize(item):
        result = [ ]
        value = item["log"] or ""
        for line in value.replace('\r\n', '\n').replace('\r', '\n').split('\n'):
            line = line.strip()
            for (substitute, pattern) in NUMBERS:
                line = pattern.sub(substitute, line)
            result.append(line)
        return result

    def fit(self, items, tokenized=None):
        tokenized = tokenized or map(Extractor.tokenize, items)
        self.extract.fit(tokenized)

    def transform(self, items, tokenized=None):
        tokenized = list(tokenized or map(Extractor.tokenize, items))
        seen = { }
        for index, item in enumerate(items):
            if not select(item):
                continue
            lines = tokenized[index]
            filtered = filter(lambda line: line not in self.extract.stop_words_, lines)
            log = "\n".join(filtered)

            try:
                timestamp = calendar.timegm(time.strptime(item.get("date", ""), "%Y-%m-%dT%H:%M:%SZ"))
            except ValueError:
                timestamp = -1

            context = item.get("context", "")
            tracker = item.get("tracker", "")
            name = item.get("name", "")

            # For identifying duplicates
            key = "{}\n{}\n{}\n{}".format(context, tracker, name, log)
            if not key in seen:
                seen[key] = [
                    log,                  # FEATURE_LOG
                    [ ],                  # FEATURE_ITEMS
                    [ ],                  # FEATURE_WHEN
                    [ ],                  # FEATURE_MERGED
                    name,                 # FEATURE_NAME
                    context,              # FEATURE_CONTEXT
                    tracker,              # FEATURE_TRACKER
                ]
            seen[key][FEATURE_ITEMS].append(index)
            seen[key][FEATURE_WHEN].append(timestamp)

            merged = item.get("merged")
            if merged is not None:
                seen[key][FEATURE_MERGED].append(merged and 1 or 0)

        sys.stderr.write("TOTAL: {}\n".format(len(seen)))

        results = [ ]
        for key, features in seen.items():
            features[FEATURE_ITEMS] = tuple(features[FEATURE_ITEMS])
            features[FEATURE_WHEN] = tuple(features[FEATURE_WHEN])
            results.append(tuple(features))
        return results

    def fit_transform(self, items):
        tokenized = list(map(Extractor.tokenize, items))
        self.fit(items, tokenized)
        return self.transform(items, tokenized)

    def stop_tokens(self):
        return self.extract.stop_words_

# This is a helpful debugger to help diagnose data, and figure out if we're
# getting the above threshold and regular expressions right
if __name__ == '__main__':
    import data
    import argparse

    parser = argparse.ArgumentParser(description="Look for noise lines in input jsonl")
    parser.add_argument("--only", action="append", help="Only analyze these statuses")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print verbose progress output")
    parser.add_argument("filename", help="The filename in JSONL gzip format")
    opts = parser.parse_args()

    # The kind of statuses to inlcude
    if not opts.only:
        only = None
    else:
        only = lambda item: item.get("status") in opts.only

    # Load the actual data
    items = data.load(opts.filename, only=only, verbose=opts.verbose)

    # Print out all lines we think are stop lines in the data
    extract = Extractor()
    extract.fit(items)
    for stop in extract.stop_tokens():
        print(stop)
