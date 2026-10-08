#!/usr/bin/env python3
"""Compare compiler-uninstrumented baseline and OBASE Hinted on all structures.

Example (from repo root):
  python3 tools/run_obase_throughput.py --out data/figure9/run1
  tail -F data/figure9/run1/progress.log

This extends the paper's baseline/OBASE Hinted comparison across ten DS.
The baseline retains Guide pointers; mode raw uses actual C++ pointers and
does not link the OBASE runtime (Harris, Pugh, CHM, and Masstree only).
Defaults: YCSB scattered C, 1M records x 10 fields x 1024 bytes, 6 threads,
120s OC interval, 1%/minute target, Ct +/-1 within [1,32]. Run serially.
Each mode has a fresh server/load. OBASE must complete two consecutive
pageout rounds without hitting either migration cap before measurement.
Replicates are measurement windows on the same loaded server, not independent
fresh-load trials. Logs and a manifest preserve this distinction.
"""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
STRUCTURES = ('ht_harris ht_pugh ht_chm sl_seq sl_fraser sl_hierlihy '
              'bpt_seq bpt_occ bpt_mass trie_art').split()
RAW_STRUCTURES = ['ht_harris', 'ht_pugh', 'ht_chm', 'bpt_mass']
ERRORS = re.compile(r'\[error\]|\[critical\]|Reference count\b|invalid heap|'
                    r'Failed to allocate|purge failed|madvise.*failed', re.I)


def ycsb_metrics(path, operation, expected=None):
    text = path.read_text(errors='replace')
    values = {}
    for line in text.splitlines():
        fields = line.split(', ')
        if len(fields) == 3 and fields[0].startswith('['):
            values[fields[0].strip('[]'), fields[1]] = float(fields[2])
    count = values.get((operation, 'Operations'), 0)
    ok = values.get((operation, 'Return=OK'), 0)
    bad = [(key, value) for key, value in values.items()
           if key[1].startswith('Return=') and key[1] != 'Return=OK' and value]
    if (count <= 0 or count != ok or bad or
            (expected is not None and count != expected) or
            re.search(r'Exception|ERROR|failed', text)):
        raise RuntimeError(f'YCSB validation failed: {path}; count={count}, OK={ok}, errors={bad}')
    return {
        'ops_sec': values['OVERALL', 'Throughput(ops/sec)'],
        'runtime_ms': values['OVERALL', 'RunTime(ms)'],
        'operations': int(count),
        'avg_latency_us': values[operation, 'AverageLatency(us)'],
        'p99_latency_us': values[operation, '99thPercentileLatency(us)'],
    }


def converged(text):
    # A pageout log precedes completion of the entire migration round.
    rounds = re.findall(r'Migration active phase started(.*?)Migration phase ended',
                        text, re.S)
    return len(rounds) >= 2 and all(
        'Target promotion rate reached: Paging out' in part and
        'cap (' not in part for part in rounds[-2:])


class Experiment:
    def __init__(self, args):
        self.args = args
        self.out = args.out.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        self.progress = (self.out / 'progress.log').open('w', buffering=1)
        self.server = None
        self.child = None
        self.server_log = None
        self.env = dict(os.environ, CREST_SCAN_INTERVAL_S='120')
        self.env['LD_LIBRARY_PATH'] = str(args.jemalloc / 'lib') + ':' + os.environ.get('LD_LIBRARY_PATH', '')
        self.rows = []
        self.cp = self.classpath()
        manifest = vars(args).copy()
        manifest.update(
            source_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
            controller_sha256=hashlib.sha256((ROOT / 'crest/runtime/ObjectCollector.cc').read_bytes()).hexdigest(),
            runner_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            measurement_repeats=args.repeats,
            controller={'scan_interval_seconds': 120, 'target_per_minute': 0.01,
                        'ct_initial': 3, 'ct_step': 1, 'ct_min': 1, 'ct_max': 32},
            baseline='Compiler instrumentation disabled; Guide pointers retained; OC off',
            raw='Actual C++ pointers; jemalloc; no Guide hooks, SODA, SAMA, TAG, or Object Collector linked; load then read only',
            obase='Compiler instrumentation enabled; tracking, migration, proactive pageout on',
            transport='UNIX /tmp/server.sock',
            repeats='Successive measurement windows on one fresh load per DS/mode',
            paper='https://www.usenix.org/system/files/osdi26-banakar.pdf',
            paper_scope='Extension of baseline versus OBASE Hinted to all DS; no other backends',
            host=os.uname(), cpus_allowed=sorted(os.sched_getaffinity(0)),
            started_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
        (self.out / 'manifest.json').write_text(json.dumps(manifest, default=str, indent=2) + '\n')
        (self.out / 'source.patch').write_bytes(subprocess.check_output(['git', 'diff'], cwd=ROOT))
        for command, name in [(['lscpu'], 'cpu.txt'), (['free', '-h'], 'memory.txt'),
                              (['swapon', '--show'], 'swap.txt'), (['java', '-version'], 'java.txt')]:
            with (self.out / name).open('w') as log:
                subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

    def say(self, text):
        message = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()) + ' ' + text
        print(message, flush=True)
        self.progress.write(message + '\n')

    def classpath(self):
        # Use built source jars plus dependencies from the existing distribution;
        # avoids Maven/network activity during timed experiments.
        libs = self.out / 'ycsb-libs'
        libs.mkdir()
        archives = list((ROOT / 'YCSB/crest/target').glob('ycsb-crest-binding-*.tar.gz'))
        if len(archives) != 1:
            raise RuntimeError('Build YCSB first; expected one crest distribution archive')
        with tarfile.open(archives[0]) as archive:
            for member in archive.getmembers():
                name = Path(member.name)
                if member.isfile() and name.parent.name == 'lib' and name.suffix == '.jar':
                    if not name.name.startswith(('core-', 'crest-binding-')):
                        (libs / name.name).write_bytes(archive.extractfile(member).read())
        jars = []
        for module, pattern in [('core', 'core-*.jar'), ('crest', 'crest-binding-*.jar')]:
            found = list((ROOT / 'YCSB' / module / 'target').glob(pattern))
            if len(found) != 1:
                raise RuntimeError(f'Expected one built {module} jar')
            jars.extend(found)
        jars.extend(sorted(libs.glob('*.jar')))
        return ':'.join(map(str, jars))

    def run(self, command, log, timeout):
        command = list(map(str, command))
        self.say('$ ' + shlex.join(command) + ' > ' + str(log) + ' 2>&1')
        with log.open('w') as handle:
            self.child = subprocess.Popen(command, cwd=ROOT / 'crest', env=self.env,
                                          stdout=handle, stderr=subprocess.STDOUT)
            try:
                code = self.child.wait(timeout=timeout)
            except BaseException:
                self.stop(self.child)
                raise
            finally:
                self.child = None
        if code:
            raise RuntimeError(f'Command exited {code}: {log}')

    @staticmethod
    def stop(process):
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    @staticmethod
    def command(text):
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(60)
            sock.connect('/tmp/server.sock')
            sock.sendall(text.encode() + b'\n')
            data = b''
            while not data.endswith(b'\n'):
                piece = sock.recv(4096)
                if not piece:
                    raise RuntimeError('Server disconnected')
                data += piece
            return data.decode().strip()

    def start(self, binary, folder, ds, mode):
        # The server unlinks the global socket at startup: never steal a live one.
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
            sock.settimeout(2)
            try:
                sock.connect('/tmp/server.sock')
            except (FileNotFoundError, ConnectionRefusedError):
                pass
            else:
                raise RuntimeError('Existing server is listening; stop it before running this experiment')
        cmd = ['taskset', '-c', self.args.server_cpus, str(binary), '127.0.0.1', '6363', '1', str(self.args.threads)]
        self.server_log = folder / 'server.log'
        self.say('$ CREST_SCAN_INTERVAL_S=120 ' + shlex.join(cmd) + ' > ' + str(self.server_log) + ' 2>&1')
        with self.server_log.open('w') as log:
            self.server = subprocess.Popen(cmd, cwd=ROOT / 'crest', env=self.env,
                                           stdout=log, stderr=subprocess.STDOUT)
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                raise RuntimeError(f'Server exited: {self.server_log}')
            try:
                if self.command('GET benchmark-connectivity-check') == 'ERR':
                    break
            except (FileNotFoundError, ConnectionRefusedError, socket.timeout):
                time.sleep(0.2)
        else:
            raise RuntimeError('Server startup timed out')
        if f'Data structure: {ds}' not in self.server_log.read_text():
            raise RuntimeError('Server structure does not match requested DS')
        if mode == 'raw':
            if 'Pointer mode: raw (OBASE runtime not linked)' not in self.server_log.read_text():
                raise RuntimeError('Server is not a raw-pointer build')
            if self.command('obase decay') != 'ERR OBASE disabled in raw-pointer build':
                raise RuntimeError('Raw-pointer server unexpectedly permits OBASE')
        self.say(f'Server PID {self.server.pid}; watch: tail -F {self.server_log}')

    def ycsb(self, folder, phase, seconds=None):
        load = phase == 'load'
        props = dict(recordcount=self.args.records, operationcount=self.args.records if load else 2000000000,
                     fieldcount=10, fieldlength=self.args.fieldlength, requestdistribution='zipfian',
                     readallfields=str(self.args.read_all_fields).lower())
        if seconds:
            props['maxexecutiontime'] = seconds
        command = ['taskset', '-c', self.args.client_cpus, 'java', '-Xmx2g', '-cp', self.cp,
                   'site.ycsb.Client', '-db', 'site.ycsb.db.CrestClient', '-s',
                   '-P', ROOT / 'YCSB/workloads' / ('workloadScatter_a' if load else 'workloadScatter_c'),
                   '-threads', str(self.args.threads), '-load' if load else '-t']
        for key, value in props.items():
            command += ['-p', f'{key}={value}']
        log = folder / (phase + '.log')
        self.run(command, log, self.args.load_timeout if load else seconds + 120)
        self.check_server()
        return ycsb_metrics(log, 'INSERT' if load else 'READ', self.args.records if load else None)

    def check_server(self):
        if self.server.poll() is not None:
            raise RuntimeError('Server died during benchmark')
        text = self.server_log.read_text(errors='replace')
        if ERRORS.search(text):
            raise RuntimeError(f'Server logged a runtime error: {self.server_log}')
        return text

    def rss(self):
        status = Path(f'/proc/{self.server.pid}/status').read_text()
        return {key: int(re.search(rf'^{key}:\s+(\d+)', status, re.M).group(1))
                for key in ['VmRSS', 'VmSwap']}

    def execute(self):
        args = self.args
        for ds in args.ds:
            for mode in args.modes:
                folder = self.out / ds / mode
                folder.mkdir(parents=True)
                build = ROOT / 'crest' / ('bin-figure9-' + mode)
                if mode == 'raw':
                    build /= ds
                self.run(['make', 'all', f'DS={ds}', f'BIN_DIR={build}',
                          f'ENABLE_LLVM_PASS={int(mode == "obase")}',
                          f'RAW_POINTERS={int(mode == "raw")}',
                          f'JEMALLOC_DIR={args.jemalloc}', '-j8'], folder / 'build.log', 900)
                self.say('Server binary SHA256: ' + hashlib.sha256((build / 'crest-server').read_bytes()).hexdigest())
                try:
                    self.start(build / 'crest-server', folder, ds, mode)
                    self.ycsb(folder, 'load')
                    before = self.rss()
                    self.say(f'{ds}/{mode}: load validated; RSS={before["VmRSS"] / 1048576:.2f} GiB')
                    warmup_start = time.monotonic()
                    if mode == 'obase':
                        for text in ['obase decay', 'obase migrate']:
                            self.say('$ crest-cli 127.0.0.1 6363 ' + shlex.quote(text))
                            if self.command(text) != 'OK':
                                raise RuntimeError(f'Failed to enable {text}')
                        config = 'scan interval=120s, target promotion rate=1.00% per minute, Ct initial=3, step=+1/-1, bounds=[1, 32]'
                        if config not in self.check_server():
                            raise RuntimeError('Effective controller settings do not match the paper')
                        warmup = 0
                        while time.monotonic() - warmup_start < args.warmup_max:
                            warmup += 1
                            self.ycsb(folder, f'warmup-{warmup:02d}', 120)
                            text = self.check_server()
                            self.say(f'{ds}/{mode}: warm-up {warmup}; pageouts={text.count("Target promotion rate reached: Paging out")}')
                            if converged(text):
                                break
                        else:
                            raise RuntimeError('OBASE did not converge before timeout; no throughput result accepted')
                    else:
                        self.ycsb(folder, 'warmup', 120)
                    elapsed = time.monotonic() - warmup_start
                    self.say(f'{ds}/{mode}: warm-up complete after {elapsed:.0f}s; measuring')
                    for rep in range(1, args.repeats + 1):
                        stats = self.ycsb(folder, f'measure-{rep:02d}', args.seconds)
                        after = self.rss()
                        row = dict(ds=ds, mode=mode, rep=rep, threads=args.threads,
                                   records=args.records, fieldlength=args.fieldlength,
                                   readallfields=args.read_all_fields, warmup_seconds=round(elapsed, 2),
                                   post_load_rss_kib=before['VmRSS'], rss_kib=after['VmRSS'],
                                   swap_kib=after['VmSwap'], **stats)
                        self.rows.append(row)
                        with (self.out / 'throughput.csv').open('w', newline='') as handle:
                            writer = csv.DictWriter(handle, fieldnames=list(row))
                            writer.writeheader()
                            writer.writerows(self.rows)
                        self.say(f'RESULT {ds}/{mode} rep={rep}: {stats["ops_sec"]:.1f} ops/s, RSS={after["VmRSS"] / 1048576:.2f} GiB')
                finally:
                    self.stop(self.server)
                    self.server = None
        self.say('COMPLETE: ' + str(self.out / 'throughput.csv'))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', type=Path, required=True, help='New output directory; existing results are never overwritten')
    parser.add_argument('--ds', nargs='+', choices=STRUCTURES, default=STRUCTURES)
    parser.add_argument('--modes', nargs='+', choices=['baseline', 'obase', 'raw'], default=['baseline', 'obase'])
    parser.add_argument('--records', type=int, default=1000000)
    parser.add_argument('--fieldlength', type=int, default=1024)
    parser.add_argument('--read-all-fields', action='store_true', help='Read all ten fields per YCSB operation')
    parser.add_argument('--threads', type=int, default=6)
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--warmup-max', type=int, default=3600)
    parser.add_argument('--load-timeout', type=int, default=3600)
    parser.add_argument('--server-cpus', default='0,2,4,6,8,10,12')
    parser.add_argument('--client-cpus', default='14,16,18,20,22,24')
    parser.add_argument('--jemalloc', type=Path, default=Path.home() / 'jemalloc')
    args = parser.parse_args()
    if 'raw' in args.modes and any(ds not in RAW_STRUCTURES for ds in args.ds):
        parser.error('Raw-pointer mode supports only: ' + ' '.join(RAW_STRUCTURES))
    if min(args.records, args.fieldlength, args.threads, args.seconds, args.repeats, args.warmup_max, args.load_timeout) <= 0:
        parser.error('Counts and durations must be positive')
    # Stop only this runner's own processes when interrupted.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    experiment = Experiment(args)
    try:
        experiment.execute()
    except BaseException as error:
        experiment.say(f'STOPPED: {error}')
        experiment.stop(experiment.child)
        experiment.stop(experiment.server)
        raise


if __name__ == '__main__':
    main()
