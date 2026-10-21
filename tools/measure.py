#!/usr/bin/env python3
"""Unified CrestDB trace measurement, reporting, and plotting command."""
import argparse
import json
from pathlib import Path
import signal
import sys

from measurements.collect import capture
from measurements.common import read, write
from measurements.report import summarize
from measurements.sweep import config, sweep


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='action', required=True)
    record = commands.add_parser('capture', help='Measure an existing server during a foreground client command')
    record.add_argument('--pid', type=int, required=True)
    record.add_argument('--out', type=Path, required=True)
    record.add_argument('--cgroup', type=Path)
    record.add_argument('--profiles', default='cpu,samples', help='Comma-separated cpu,samples,uncore; use none for proc-only')
    record.add_argument('--sudo', action='store_true', help='Run perf with sudo -n')
    record.add_argument('--interval', type=float, default=1)
    record.add_argument('--sample-period', type=int, default=4000)
    record.add_argument('--ldlat', type=int, default=30)
    record.add_argument('--timeout', type=float, default=7200)
    record.add_argument('command', nargs=argparse.REMAINDER)
    run = commands.add_parser('sweep', help='Run baseline/OBASE across configured DRAM limits; stop on failure')
    run.add_argument('--config', type=Path, required=True)
    run.add_argument('--out', type=Path, required=True)
    for name in ('summarize', 'plot'):
        command = commands.add_parser(name, help='Read new or archived results; write a separate analysis directory')
        command.add_argument('run', type=Path)
        command.add_argument('--out', type=Path, required=True)
    status = commands.add_parser('status', help='Show a detached sweep and its latest recorded process sample')
    status.add_argument('run', type=Path)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    if args.action == 'capture':
        argv = args.command[1:] if args.command[:1] == ['--'] else args.command
        if not argv:
            parser.error('capture requires -- CLIENT [ARGS...]')
        profiles = [] if args.profiles == 'none' else args.profiles.split(',')
        if len(profiles) != len(set(profiles)) or set(profiles) - {'cpu', 'samples', 'uncore'}:
            parser.error('Invalid or duplicate perf profiles')
        if min(args.pid, args.interval, args.sample_period, args.ldlat, args.timeout) <= 0:
            parser.error('PID, intervals, thresholds, and timeout must be positive')
        if not Path(f'/proc/{args.pid}').is_dir():
            parser.error('Target PID does not exist')
        args.out = args.out.resolve()
        args.out.mkdir(parents=True, exist_ok=False)
        write(args.out / 'capture.json', dict(pid=args.pid, command=argv, profiles=profiles,
              interval=args.interval, sample_period=args.sample_period, ldlat=args.ldlat,
              cgroup=str(args.cgroup) if args.cgroup else None))
        result = capture(args.pid, args.out, argv, cgroup=args.cgroup, profiles=profiles,
                         interval=args.interval, sudo=args.sudo, sample_period=args.sample_period,
                         ldlat=args.ldlat, timeout=args.timeout)
        print(json.dumps(result, indent=2))
    elif args.action == 'sweep':
        sweep(config(args.config), args.out)
    elif args.action == 'summarize':
        summarize(args.run, args.out)
    elif args.action == 'plot':
        from measurements.plot import plot
        plot(args.run, args.out)
    else:
        info = read(args.run / 'status.json')
        pid = info.get('pid')
        info['runner_pid_exists'] = bool(pid and Path(f'/proc/{pid}').exists())
        if info.get('configuration'):
            folder = args.run / info['configuration']
            path = folder / 'replay-memory.jsonl'
            if path.exists():
                # Read only the tail, and tolerate an in-progress final line.
                with path.open('rb') as stream:
                    stream.seek(max(0, path.stat().st_size - 65536))
                    lines = stream.read().splitlines()
                for line in reversed(lines):
                    try:
                        info['last_sample'] = json.loads(line)
                        break
                    except json.JSONDecodeError:
                        pass
        print(json.dumps(info, indent=2))


if __name__ == '__main__':
    main()
