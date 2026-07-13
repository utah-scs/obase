#!/usr/bin/env python3
#
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This software may be used and distributed according to the terms of the
# GNU General Public License version 2.

import argparse
import datetime
import math
import time
import sys
import os

def h(x):
    """Translates a number of bytes to a human-readable string."""
    order = 0
    suffix = ['', 'k', 'M', 'G', 'T']
    max_order = len(suffix) - 1
    while abs(x) > 1024 and order < max_order:
        x /= 1024.0
        order += 1
    return '%.2f%s' % (x, suffix[order])


def log(string):
    """Logs timestamped information to stdout."""
    ts = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(ts + ' ' + string, flush=True)


class Cgroup(object):
    def __init__(self, path):
        self.path = path
        self.pressure()  # Ensure psi is available in Linux 4.20+

    # Parsing the memory pressure file format:
    #
    # some avg10=0.00 avg60=0.00 avg300=0.00 total=0
    # full avg10=0.00 avg60=0.00 avg300=0.00 total=0

    def pressure(self):
        return float(self.readlines("memory.pressure")[0].split()[1].split('=')[1])

    def total(self):
        return int(self.readlines("memory.pressure")[0].split()[4].split('=')[1])

    # Memory management interface

    def reclaim(self, amount):
        """Request kernel to reclaim the specified amount of memory."""
        amount = int(amount)
        if amount > 0:
            self.write("memory.reclaim", str(amount))
            return amount
        return 0

    def read_current(self):
        """Read current memory usage of the cgroup."""
        return int(self.read("memory.current"))

    # Cgroupfs IO

    def read(self, filename):
        with open(os.path.join(self.path, filename)) as f:
            return f.read()

    def readlines(self, filename):
        with open(os.path.join(self.path, filename)) as f:
            return f.readlines()

    def write(self, filename, string):
        with open(os.path.join(self.path, filename), 'w') as f:
            f.write(string)


class Senpai(object):
    def __init__(self, conf):
        self.conf = conf
        log('Configuration:')
        for key, val in vars(conf).items():
            log(f'  {key} = {val}')

        self.cgroup = Cgroup(self.conf.cgpath)

    def run(self):
        while True:
            time.sleep(self.conf.interval)
            self.tick()

    def tick(self):
        # Get current memory pressure
        psi_some = self.cgroup.pressure()
        
        # Get current memory usage
        current_mem = self.cgroup.read_current()
        
        # Calculate memory to reclaim using TMO formula
        reclaim_factor = max(0, 1 - (psi_some / self.conf.psi_threshold))
        reclaim_mem = int(current_mem * self.conf.reclaim_ratio * reclaim_factor)
        
        # Apply reclaim limit (max 1% of total workload size per reclaim period)
        max_reclaim = int(current_mem * self.conf.max_reclaim_percent)
        reclaim_mem = min(reclaim_mem, max_reclaim)
        
        # Request memory reclaim
        reclaimed = self.cgroup.reclaim(reclaim_mem)
        
        log(f'current_mem={h(current_mem)} psi_some={psi_some:.6f} '
            f'psi_threshold={self.conf.psi_threshold:.6f} reclaim_factor={reclaim_factor:.4f} '
            f'reclaimed={h(reclaimed)}')


parser = argparse.ArgumentParser(description="""
Senpai continuously engages the kernel's reclaim algorithm, using PSI metrics
to determine how much memory can be offloaded.

For each cgroup, Senpai calculates the amount of memory to reclaim as:
reclaim_mem = current_mem x reclaim_ratio x max(0, 1 - PSIsome/PSIthreshold)

No memory is reclaimed when PSIsome is above PSIthreshold. Otherwise, Senpai 
asks the kernel to reclaim reclaim_mem from the cgroup. As PSIsome approaches
PSIthreshold, Senpai gradually reclaims less memory to maintain mild pressure.
""",
formatter_class=argparse.RawDescriptionHelpFormatter)

parser.add_argument('cgpath', type=str)
parser.add_argument('--interval', type=int, default=6,
                    help='Seconds between reclaim attempts (default: 6)')
parser.add_argument('--reclaim-ratio', type=float, default=0.0005,
                    help='Base ratio of memory to reclaim (default: 0.0005)')
parser.add_argument('--psi-threshold', type=float, default=0.001,
                    help='PSI pressure threshold (default: 0.001 or 0.1%)')
parser.add_argument('--max-reclaim-percent', type=float, default=0.01,
                    help='Maximum percentage of memory to reclaim per period (default: 0.01 or 1%)')

conf = parser.parse_args()
senpai = Senpai(conf)
senpai.run()