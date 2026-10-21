"""Configurable, serial baseline/OBASE sweeps with a fresh capped server per case."""
from contextlib import contextmanager
from datetime import datetime, timezone
import getpass
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time
import traceback
import uuid

from .collect import capture
from .common import digest, read, run, write
from .integrity import watch

ROOT = Path(__file__).resolve().parents[2]
ERRORS = re.compile(r'\[error\]|\[critical\]|invalid heap|Failed to allocate|purge failed|'
                    r'RECLAMATION SAFETY FAILURE|SODA:|saturated|AddressSanitizer|runtime error:', re.I)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def config(path):
    data = read(path)
    required = {'structure', 'binaries', 'load_trace', 'replay_trace', 'expected_preload_keys',
                'replay_rows', 'expected_counts', 'memory_node', 'server_cpus', 'client_cpus'}
    defaults = dict(dram_gib_order=[2, 4, 8], threads=6, scan_seconds=120, memory_swap_max_bytes=96 * 2**30,
                    profiles=['cpu', 'samples', 'uncore'], sample_period=4000, ldlat=30, interval=1,
                    integrity_interval=5, perf_sudo=True, load_timeout=21600, replay_timeout=7200,
                    address='127.0.0.2', port=6363, allow_misses=False, incr_rows=0,
                    environment={}, loader=str(ROOT / 'tools/traceReader/twitterZstLoader.x'),
                    replay=str(ROOT / 'tools/traceReader/twitterZstReplay.x'), swap_backend=None,
                    baseline_definition='ENABLE_LLVM_PASS=0, Guide pointers retained; decay/migration disabled',
                    obase_definition='ENABLE_LLVM_PASS=1; decay/migration enabled after preload')
    require(not required - data.keys(), f'Missing configuration: {required - data.keys()}')
    require(not data.keys() - required - defaults.keys(), f'Unknown configuration: {data.keys() - required - defaults.keys()}')
    result = dict(defaults, **data)
    require(set(result['binaries']) == {'baseline', 'obase'}, 'Supply baseline and obase binaries')
    require(set(result['expected_counts']) == {'GET', 'SET', 'DEL'}, 'Expected counts must contain GET, SET, DEL')
    require(result['replay_rows'] == sum(result['expected_counts'].values()) + result['incr_rows'],
            'Replay rows must equal GET + SET + DEL + skipped INCR')
    require(all(isinstance(v, int) and v >= 0 for v in [*result['expected_counts'].values(), result['incr_rows']]),
            'Trace operation counts must be nonnegative integers')
    require(sum(result['expected_counts'].values()) > 0, 'Replay must execute at least one operation')
    for key in ('threads', 'scan_seconds', 'sample_period', 'ldlat', 'interval', 'integrity_interval',
                'load_timeout', 'replay_timeout', 'expected_preload_keys'):
        require(result[key] > 0, key + ' must be positive')
    require(result['dram_gib_order'] and all(size > 0 for size in result['dram_gib_order'])
            and len(set(result['dram_gib_order'])) == len(result['dram_gib_order']), 'DRAM sizes must be positive and unique')
    require(result['memory_swap_max_bytes'] >= 0, 'Swap limit cannot be negative')
    require(set(result['profiles']) <= {'cpu', 'samples', 'uncore'}
            and len(result['profiles']) == len(set(result['profiles'])), 'Invalid/duplicate collector profile')
    # Crest reserves 127.0.0.1 for its fixed UNIX socket; use a distinct loopback address.
    require(re.fullmatch(r'127\.\d+\.\d+\.\d+', result['address'])
            and result['address'] != '127.0.0.1', 'Use a loopback TCP address other than 127.0.0.1')
    for key in ('load_trace', 'replay_trace', 'loader', 'replay'):
        result[key] = str(Path(result[key]).expanduser().resolve(strict=True))
    result['binaries'] = {k: str(Path(v).expanduser().resolve(strict=True)) for k, v in result['binaries'].items()}
    require(result['binaries']['baseline'] != result['binaries']['obase'], 'Supply separate mode binaries')
    return result


def command(settings, request):
    with socket.create_connection((settings['address'], settings['port']), timeout=10) as connection:
        connection.sendall(request.encode() + b'\n')
        data = b''
        while not data.endswith(b'\n'):
            part = connection.recv(65536)
            if not part:
                raise RuntimeError('Server closed command response')
            data += part
        require(b'\n' not in data[:-1], 'Extra server response line')
        return data[:-1]


def affinity(settings, side):
    return ['numactl', '--membind=' + str(settings['memory_node']),
            'taskset', '-c', settings[side + '_cpus']]


@contextmanager
def server(settings, mode, gib, folder):
    unit = 'crest-measure-' + uuid.uuid4().hex
    limit = int(gib * 2**30)
    argv = ['sudo', '-n', 'systemd-run', '--quiet', '--unit=' + unit,
            '--property=User=' + getpass.getuser(), '--property=PrivateTmp=yes',
            '--property=MemoryMax=' + str(limit), '--property=MemorySwapMax=' + str(settings['memory_swap_max_bytes']),
            '--property=TimeoutStopSec=5', '--property=KillMode=control-group', '--property=LimitCORE=0',
            '--property=RuntimeMaxSec=' + str(settings['load_timeout'] + settings['replay_timeout'] + 300),
            '--property=StandardOutput=append:' + str(folder / 'server.log'),
            '--property=StandardError=append:' + str(folder / 'server.log'),
            '--setenv=CREST_SCAN_INTERVAL_S=' + str(settings['scan_seconds'])]
    argv += ['--setenv=' + key + '=' + value for key, value in settings['environment'].items()]
    argv += affinity(settings, 'server') + [settings['binaries'][mode], settings['address'],
                                            str(settings['port']), '1', str(settings['threads'])]
    write(folder / 'server-command.json', dict(argv=argv, unit=unit))
    try:
        subprocess.run(argv, check=True, timeout=30)
        info = subprocess.check_output(['systemctl', 'show', unit, '-p', 'MainPID', '-p', 'ControlGroup',
                                        '-p', 'MemoryMax', '-p', 'MemorySwapMax'], text=True)
        (folder / 'service-settings.txt').write_text(info)
        values = dict(line.split('=', 1) for line in info.splitlines())
        pid = int(values['MainPID'])
        require(pid > 0, 'Server did not start')
        cg = Path('/sys/fs/cgroup') / values['ControlGroup'].lstrip('/')
        require((cg / 'memory.max').read_text().strip() == str(limit), 'Incorrect server memory limit')
        require((cg / 'memory.swap.max').read_text().strip() == str(settings['memory_swap_max_bytes']), 'Incorrect swap limit')
        for _ in range(150):
            require(Path(f'/proc/{pid}').exists(), 'Server died at startup')
            try:
                if command(settings, 'STATS') == b'OK':
                    break
            except (OSError, RuntimeError):
                pass
            time.sleep(.1)
        else:
            raise RuntimeError('Server startup timeout')
        log = (folder / 'server.log').read_text(errors='replace')
        require('Data structure: ' + settings['structure'] in log, 'Unexpected data structure')
        require(not ERRORS.search(log), 'Server startup error')
        require(command(settings, 'obase none') == b'OK', 'Cannot disable OBASE during preload')
        write(folder / 'server-state.json', dict(pid=pid, unit=unit, cgroup=str(cg),
              numa_maps=Path(f'/proc/{pid}/numa_maps').read_text(), memory_gib=gib))
        yield pid, cg
    finally:
        stopped = subprocess.run(['sudo', '-n', 'systemctl', 'stop', unit], capture_output=True, text=True, timeout=30)
        final = subprocess.run(['systemctl', 'show', unit, '-p', 'MainPID', '-p', 'Result', '-p', 'ExecMainStatus'],
                               capture_output=True, text=True, timeout=10)
        write(folder / 'server-stop.json', dict(returncode=stopped.returncode, output=stopped.stderr, state=final.stdout))
        require('MainPID=0\n' in final.stdout or 'not found' in final.stderr, 'Owned server cleanup could not be verified')
        subprocess.run(['sudo', '-n', 'systemctl', 'reset-failed', unit], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=10, check=False)


def parse_replay(path, settings):
    text = Path(path).read_text().split('=== Overall Trace Replay Summary ===')[-1]

    def number(label):
        match = re.search(r'^' + re.escape(label) + r':\s+([0-9.]+)', text, re.M)
        require(match is not None, 'Missing replay metric: ' + label)
        return float(match[1])

    count = re.search(r'Queued trace rows: (\d+); processed trace rows: (\d+)', text)
    require(count and tuple(map(int, count.groups())) == (settings['replay_rows'],) * 2, 'Incomplete trace rows')
    result = dict(operations=int(number('Total operations')), successful=int(number('Successful operations')),
                  failed=int(number('Failed operations')), misses=int(number('GET/DEL misses (included in completed operations)')),
                  ops_sec=number('Average throughput'), replay_seconds=number('Total execution time'),
                  avg_latency_us=number('Average latency'), trace_rows=settings['replay_rows'])
    for op, expected in settings['expected_counts'].items():
        count = int(number(op + ' operations'))
        require(count == expected, 'Incorrect ' + op + ' count')
        latency = re.search(r'\[' + op + r': Count=\d+, Max=[0-9.]+, Min=[0-9.]+, Avg=[0-9.]+, p90=[0-9.]+, p99=([0-9.]+)', text)
        require(not count or latency is not None, 'Missing ' + op + ' latency')
        result[op.lower() + '_operations'] = count
        result[op.lower() + '_p99_us'] = float(latency[1]) if latency else None
    require(result['operations'] == result['successful'] == sum(settings['expected_counts'].values())
            and result['failed'] == 0, 'Trace replay failed or incomplete')
    require(0 <= result['misses'] <= settings['expected_counts']['GET'] + settings['expected_counts']['DEL'], 'Invalid misses')
    require(settings['allow_misses'] or result['misses'] == 0, 'Unexpected misses')
    return result


def sweep(settings, output):
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []

    def status(phase, **extra):
        item = dict(status=phase, completed=len(results), pid=os.getpid(),
                    utc=datetime.now(timezone.utc).isoformat(), **extra)
        write(output / 'status.json', item)
        print(json.dumps(item), flush=True)

    try:
        status('preflight')
        # Reject an occupied endpoint before creating or modifying any server.
        with socket.socket() as probe:
            probe.bind((settings['address'], settings['port']))
        if settings['swap_backend']:
            require(settings['swap_backend'] in [line.split()[0] for line in Path('/proc/swaps').read_text().splitlines()[1:]],
                    'Expected swap backend is not active')
        manifest = {p: digest(p) for p in {*settings['binaries'].values(), settings['loader'], settings['replay'],
                                         settings['load_trace'], settings['replay_trace']}}
        require(manifest[settings['binaries']['baseline']] != manifest[settings['binaries']['obase']],
                'Baseline and OBASE binaries are identical')
        manifest.update({str(p): digest(p) for p in [ROOT / 'tools/measure.py',
                         *Path(__file__).parent.glob('*.py'),
                         *[ROOT / 'tools/traceReader' / name for name in
                           ('twitterZstLoader.cc', 'twitterZstReplay.cc', 'crest_protocol.h')]]})
        write(output / 'sha256.json', manifest)
        source = subprocess.check_output(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], text=True).strip()
        dirty = subprocess.check_output(['git', '-C', str(ROOT), 'diff', '--binary'], text=True)
        (output / 'working-tree.patch').write_text(dirty)
        write(output / 'setup.json', dict(settings, schema_version=1, source_commit=source,
                                         modes=['baseline', 'obase'], transport='TCP loopback'))
        write(output / 'machine.json', dict(uname=list(os.uname()), page_size=os.sysconf('SC_PAGE_SIZE'),
              cpuinfo=Path('/proc/cpuinfo').read_text().split('\n\n')[0], swaps=Path('/proc/swaps').read_text(),
              perf_version=subprocess.check_output(['perf', 'version'], text=True).strip() if settings['profiles'] else None))
        for gib in settings['dram_gib_order']:
            for mode in ('baseline', 'obase'):
                folder = output / f'{gib}g-{mode}'
                folder.mkdir()
                row = dict(dram_gib=gib, mode=mode, folder=folder.name, status='running')
                try:
                    status('preload', configuration=folder.name)
                    with server(settings, mode, gib, folder) as (pid, cg):
                        client = affinity(settings, 'client')
                        endpoint = [settings['address'], str(settings['port']), str(settings['threads'])]
                        run(client + [settings['loader'], settings['load_trace']] + endpoint,
                            folder / 'load.log', settings['load_timeout'])
                        log = (folder / 'load.log').read_text()
                        count = re.search(r'Finished inserting (\d+)/(\d+) keys successfully', log)
                        require(count and int(count[1]) == int(count[2]) == settings['expected_preload_keys'], 'Incomplete preload')
                        require('End of file reached after ' in log, 'Preload reached loader key limit')
                        row['loaded_keys'] = int(count[1])
                        request = lambda text: command(settings, text)
                        if mode == 'obase':
                            require(request('obase decay') == request('obase migrate') == b'OK', 'Cannot enable OBASE')
                        status('replay', configuration=folder.name, server_pid=pid)
                        with watch(request, folder, settings['integrity_interval']) as cancel:
                            row.update(capture(pid, folder, client + [settings['replay'], settings['replay_trace']] + endpoint,
                                       cgroup=cg, profiles=settings['profiles'], interval=settings['interval'],
                                       sudo=settings['perf_sudo'], sample_period=settings['sample_period'], ldlat=settings['ldlat'],
                                       timeout=settings['replay_timeout'], cancel=cancel))
                        row.update(parse_replay(folder / 'replay.log', settings))
                        log = (folder / 'server.log').read_text(errors='replace')
                        require(not ERRORS.search(log), 'Server reported a safety/runtime error')
                        require(request('STATS') == b'OK', 'Server stopped responding')
                        events = dict(line.split() for line in (cg / 'memory.events').read_text().splitlines())
                        require(int(events['oom']) == int(events['oom_kill']) == 0, 'Server OOM event')
                        row.update(migration_cycles=log.count('Migration phase ended'),
                                   demoted_objects=sum(map(int, re.findall(r'Demoted (\d+) objects', log))),
                                   promoted_objects=sum(map(int, re.findall(r'Promoted (\d+) objects', log))),
                                   integrity=read(folder / 'integrity-status.json'))
                        quality = [row.get(key) for key in ('cpu_perf_status', 'uncore_status', 'memory_sample_status',
                                                          'process_sample_status', 'cgroup_sample_status', 'swap_status') if key in row]
                        row['status'] = 'passed' if all(v in ('ok', 'partial_sources') for v in quality) else 'completed_with_metric_warnings'
                except BaseException as exc:
                    if (folder / 'metrics.json').exists():
                        row.update(read(folder / 'metrics.json'))
                    row.update(status='failed', error=repr(exc))
                    (folder / 'failure.txt').write_text(traceback.format_exc())
                    raise
                finally:
                    write(folder / 'result.json', row)
                    results.append(row)
                    write(output / 'results.json', results)
        status('complete')
    except BaseException as exc:
        status('failed', error=repr(exc))
        raise
