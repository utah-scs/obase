"""One implementation of process, swap, CPU, PEBS, and IMC collection."""
from contextlib import ExitStack
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import threading
import time

from .common import run, stop_process, write
from .parsers import CPU_EVENTS, UNCORE_EVENTS, memory_samples, perf_counts, uncore_metrics


def counter_delta(before, after, key):
    start, end = before.get(key), after.get(key)
    return end - start if start is not None and end is not None and end >= start else None


def host_snapshot():
    vm = dict(line.split() for line in Path('/proc/vmstat').read_text().splitlines())
    return dict(monotonic_s=time.monotonic(), host_pswpin_pages=int(vm['pswpin']),
                host_pswpout_pages=int(vm['pswpout']))


def snapshot(pid, cgroup=None):
    item = host_snapshot()
    root = Path('/proc') / str(pid)
    try:
        # Field offsets are relative to state (field 3); comm may contain ')'.
        stat = (root / 'stat').read_text().rsplit(')', 1)[1].split()
        item.update(cpu_ticks=int(stat[11]) + int(stat[12]), minor_faults=int(stat[7]),
                    major_faults=int(stat[9]), starttime_ticks=int(stat[19]))
        item.update({m[1]: int(m[2]) for m in re.finditer(
            r'^(Rss|Pss|Swap|Anonymous):\s+(\d+)', (root / 'smaps_rollup').read_text(), re.M)})
        item['process_io'] = {k: int(v) for k, v in
                              (line.split(':', 1) for line in (root / 'io').read_text().splitlines())}
    except (OSError, ValueError, IndexError) as exc:
        item['process_error'] = repr(exc)
    if cgroup:
        item['cgroup'] = {}
        for name in ('memory.current', 'memory.swap.current', 'memory.events',
                     'memory.stat', 'memory.pressure', 'cpu.stat'):
            try:
                item['cgroup'][name] = (Path(cgroup) / name).read_text().strip()
            except OSError as exc:
                item.setdefault('cgroup_errors', {})[name] = repr(exc)
    return item


def summarize_samples(samples):
    if len(samples) < 2:
        return {'process_sample_status': 'unavailable', 'swap_status': 'unavailable'}
    before, after = samples[0], samples[-1]
    swapin = counter_delta(before, after, 'host_pswpin_pages')
    swapout = counter_delta(before, after, 'host_pswpout_pages')
    valid = (not any('process_error' in s for s in samples)
             and before.get('starttime_ticks') == after.get('starttime_ticks'))
    ticks = counter_delta(before, after, 'cpu_ticks') if valid else None
    rss = [s['Rss'] for s in samples if 'Rss' in s]
    result = dict(process_sample_status='ok' if valid else 'incomplete',
                  cgroup_sample_status='unavailable' if not any('cgroup' in s for s in samples) else
                  'incomplete' if any('cgroup_errors' in s for s in samples) else 'ok',
                  swap_status='ok' if swapin is not None and swapout is not None else 'counter_reset',
                  swapin_pages=swapin, swapout_pages=swapout,
                  swap_window_seconds=after['monotonic_s'] - before['monotonic_s'],
                  minor_faults=counter_delta(before, after, 'minor_faults') if valid else None,
                  major_faults=counter_delta(before, after, 'major_faults') if valid else None,
                  server_cpu_seconds=ticks / os.sysconf('SC_CLK_TCK') if ticks is not None else None,
                  rss_peak_kib=max(rss) if rss else None, rss_end_kib=after.get('Rss'),
                  swap_end_kib=after.get('Swap'),
                  cgroup_memory_events=after.get('cgroup', {}).get('memory.events'))
    # Time-weighted mean avoids overweighting short first/last sampling intervals.
    elapsed = after['monotonic_s'] - before['monotonic_s']
    result['rss_mean_kib'] = (sum((b['monotonic_s'] - a['monotonic_s']) * (a['Rss'] + b['Rss']) / 2
                                for a, b in zip(samples, samples[1:])) / elapsed
                              if len(rss) == len(samples) and elapsed > 0 else None)
    return result


class ProcessSampler:
    def __init__(self, pid, folder, cgroup=None, interval=1):
        if interval <= 0:
            raise ValueError('Sampling interval must be positive')
        self.pid, self.folder, self.cgroup, self.interval = pid, Path(folder), cgroup, interval
        self.samples, self.errors, self.result = [], [], {}
        self.done = threading.Event()

    def take(self):
        item = snapshot(self.pid, self.cgroup)
        self.samples.append(item)
        self.stream.write(json.dumps(item) + '\n')

    def worker(self):
        while not self.done.wait(self.interval):
            try:
                self.take()
            except Exception as exc:
                self.errors.append(repr(exc))
                return

    def __enter__(self):
        self.stream = (self.folder / 'replay-memory.jsonl').open('w', buffering=1)
        try:
            self.take()
            self.thread = threading.Thread(target=self.worker)
            self.thread.start()
        except BaseException:
            self.stream.close()
            raise
        return self

    def __exit__(self, *exc):
        self.done.set()
        self.thread.join()
        try:
            self.take()  # Summary is computed only after this final boundary.
        except Exception as error:
            self.errors.append(repr(error))
        finally:
            self.stream.close()
        self.result.update(summarize_samples(self.samples))
        if self.errors:
            self.result.update(process_sample_status='incomplete', swap_status='incomplete',
                               swapin_pages=None, swapout_pages=None, major_faults=None, minor_faults=None)
        write(self.folder / 'replay-memory.jsonl.status.json',
              dict(samples=len(self.samples), errors=self.errors, **self.result))
        write(self.folder / 'replay-swap.json', dict(before=self.samples[0], after=self.samples[-1],
              page_size=os.sysconf('SC_PAGE_SIZE'), status=self.result['swap_status'],
              scope='host-wide base pages, including readahead',
              window='replay invocation including client startup/shutdown',
              swaps=Path('/proc/swaps').read_text(),
              zswap_enabled=Path('/sys/module/zswap/parameters/enabled').read_text().strip()
              if Path('/sys/module/zswap/parameters/enabled').exists() else None))


def check_uncore_platform():
    info = {k.strip(): v.strip() for k, v in
            (line.split(':', 1) for line in Path('/proc/cpuinfo').read_text().split('\n\n')[0].splitlines()
             if ':' in line)}
    if (info.get('vendor_id') != 'GenuineIntel' or info.get('cpu family') != '6'
            or info.get('model') != '85' or int(info.get('stepping', '0')) < 5):
        raise RuntimeError('Built-in IMC encodings require Cascade Lake: Intel family 6/model 85, stepping >=5')


class PerfCollector:
    def __init__(self, pid, folder, profile, sudo=False, sample_period=4000, ldlat=30):
        if profile not in ('cpu', 'samples', 'uncore'):
            raise ValueError('Unknown perf profile: ' + profile)
        self.pid, self.profile, self.sudo = pid, profile, sudo
        self.sample_period, self.ldlat = sample_period, ldlat
        self.prefix = Path(folder) / ('replay-' + profile)
        self.control, self.ack = self.path('control.fifo'), self.path('ack.fifo')
        self.raw = self.path('mem.data' if profile == 'samples' else 'perf.csv')
        self.result, self.fds, self.created, self.process = {}, [], [], None
        self.metadata = dict(profile=profile, pid=None if profile == 'uncore' else pid,
                             status='starting', scope='host-wide, all sockets' if profile == 'uncore'
                             else 'process threads, userspace', clock='monotonic seconds',
                             window='replay invocation; nested collectors include differing setup/teardown overhead')

    def path(self, suffix):
        return Path(str(self.prefix) + '-' + suffix)

    def command(self, message):
        if self.process.poll() is not None:
            raise RuntimeError(f'Perf exited; inspect {self.path("perf.log")}')
        os.write(self.fds[0], (message + '\n').encode())
        if not select.select([self.fds[1]], [], [], 5)[0]:
            raise RuntimeError(f'Perf {message} acknowledgement timed out: {self.prefix}')
        if os.read(self.fds[1], 128).strip(b'\x00\n') != b'ack':
            raise RuntimeError(f'Invalid perf acknowledgement: {self.prefix}')
        self.metadata[message + '_ack_s'] = time.monotonic()

    def __enter__(self):
        try:
            if self.profile == 'uncore':
                check_uncore_platform()
            for path in (self.control, self.ack):
                os.mkfifo(path, 0o600)
                self.created.append(path)
                self.fds.append(os.open(path, os.O_RDWR | os.O_NONBLOCK))
            prefix = ['sudo', '-n'] if self.sudo else []
            if self.profile == 'samples':
                cmd = prefix + ['perf', 'mem', '-t', 'load', 'record', '--all-user', '--ldlat', str(self.ldlat),
                                '--count', str(self.sample_period), '--output', str(self.raw),
                                '--pid', str(self.pid), '--delay=-1']
                self.metadata.update(sample_period=self.sample_period, ldlat=self.ldlat)
            else:
                events = UNCORE_EVENTS if self.profile == 'uncore' else CPU_EVENTS
                cmd = prefix + ['perf', 'stat', '-x', ';', '--no-big-num', '-o', str(self.raw),
                                '-e', '{' + ','.join(events.values()) + '}', '-D', '-1']
                cmd += ['-a'] if self.profile == 'uncore' else ['-p', str(self.pid)]
                self.metadata.update(events=events, minimum_running_pct=99)
            cmd += ['--control', f'fifo:{self.control},{self.ack}']
            self.metadata['command'] = cmd
            with self.path('perf.log').open('w') as log:
                self.process = subprocess.Popen(cmd, stdout=log, stderr=log,
                                                env=dict(os.environ, LC_ALL='C'), start_new_session=True)
            self.command('enable')
            self.metadata['status'] = 'running'
            return self
        except BaseException:
            self.finish(aborted=True)
            raise

    def finish(self, aborted=False):
        errors = []
        try:
            if self.metadata['status'] == 'running':
                self.command('disable')
        except Exception as exc:
            errors.append(repr(exc))
        try:
            if self.process is not None:
                stop_process(self.process, self.sudo)
                self.metadata['returncode'] = self.process.returncode
                if self.process.returncode not in (0, -signal.SIGINT, 128 + signal.SIGINT):
                    raise RuntimeError(f'Perf exited with {self.process.returncode}')
            seconds = self.metadata.get('disable_ack_s', 0) - self.metadata.get('enable_ack_s', 0)
            if seconds <= 0:
                raise RuntimeError('Perf window did not finish')
            self.decode(seconds)
        except Exception as exc:
            errors.append(repr(exc))
        finally:
            for fd in self.fds:
                os.close(fd)
            for path in self.created:
                path.unlink(missing_ok=True)
            self.metadata.update(status='failed' if errors else 'aborted' if aborted else 'complete',
                                 errors=errors, metrics=self.result)
            write(self.path('metadata.json'), self.metadata)
        return errors

    def __exit__(self, kind, value, tb):
        errors = self.finish(aborted=kind is not None)
        if errors and kind is None:
            raise RuntimeError(f'Perf collection failed: {self.prefix}: {errors}')

    def decode(self, seconds):
        if self.profile == 'samples':
            decoded = self.path('mem.txt')
            cmd = (['sudo', '-n'] if self.sudo else []) + [
                'perf', 'script', '-f', '-i', str(self.raw), '--show-lost-events',
                '-F', 'time,addr,data_src,weight']
            self.metadata['decode_command'] = cmd
            with decoded.open('w') as output, self.path('mem-decode.log').open('w') as log:
                subprocess.run(cmd, stdout=output, stderr=log, check=True, timeout=120,
                               env=dict(os.environ, LC_ALL='C'))
            self.result.update(memory_samples(decoded), memory_sample_window_seconds=seconds)
            logs = self.path('perf.log').read_text() + self.path('mem-decode.log').read_text()
            if re.search(r'\b(lost|throttl\w*)\b', logs, re.I):
                self.result['memory_sample_status'] = 'loss_or_throttling_reported'
        elif self.profile == 'uncore':
            self.result.update(uncore_metrics(self.raw, seconds))
        else:
            counts, coverage, status = perf_counts(self.raw, CPU_EVENTS)
            self.result.update(counts, cpu_perf_status=status, cpu_perf_window_seconds=seconds,
                               ipc=counts['instructions'] / counts['cycles']
                               if status == 'ok' and counts['cycles'] else None)
            self.result.update({k + '_running_pct': v for k, v in coverage.items()})


def capture(pid, folder, argv, *, cgroup=None, interval=1, profiles=('cpu', 'samples'),
            sudo=False, sample_period=4000, ldlat=30, timeout=7200, cancel=None):
    folder = Path(folder)
    result, collectors = {'capture_status': 'running'}, []
    sampler = ProcessSampler(pid, folder, cgroup, interval)
    try:
        with ExitStack() as stack:
            for profile in profiles:
                collector = PerfCollector(pid, folder, profile, sudo, sample_period, ldlat)
                collectors.append(collector)
                stack.enter_context(collector)
            stack.enter_context(sampler)  # Bound process and host counters closest to the command.
            run(argv, folder / 'replay.log', timeout, cancel)
        result['capture_status'] = 'complete'
    except BaseException as exc:
        result.update(capture_status='failed', error=repr(exc))
        raise
    finally:
        result.update(sampler.result)
        for collector in collectors:
            result.update(collector.result)
        write(folder / 'metrics.json', result)
    return result
