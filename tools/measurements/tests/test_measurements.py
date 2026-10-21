import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from measurements.collect import ProcessSampler, capture, summarize_samples
from measurements.common import read, write
from measurements.integrity import watch
from measurements.parsers import CPU_EVENTS, memory_samples, perf_counts
from measurements.report import configurations, swap_series
from measurements.sweep import parse_replay


class Measurements(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.folder = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_perf_coverage_missing_and_duplicate(self):
        path = self.folder / 'perf.csv'
        path.write_text('100;;cycles:u;200;50.0;;\n<not supported>;;instructions:u;0;0;;\n')
        counts, coverage, status = perf_counts(path, CPU_EVENTS)
        self.assertEqual(counts['cycles'], 100)
        self.assertIsNone(counts['instructions'])
        self.assertEqual(coverage['cycles'], 50)
        self.assertEqual(status, 'unavailable')
        path.write_text('100;;cycles:u;200;50.0;;\n' * 2)
        with self.assertRaises(ValueError):
            perf_counts(path, CPU_EVENTS)
        path.write_text('100;;cycles:u;200;50.0;;\n')
        self.assertEqual(perf_counts(path, {'cycles': 'cycles:u'})[2], 'low_coverage')

    def test_memory_sources_and_loss(self):
        path = self.folder / 'samples.txt'
        sources = [13 << 33, 14 << 33, 1 << 33, 0x80 << 5, 0]
        path.write_text(''.join(f'123.0: abc {src:x} |source\n' for src in sources))
        result = memory_samples(path)
        self.assertEqual((result['dram_samples'], result['pmem_samples'], result['cache_samples'],
                          result['unknown_samples']), (2, 1, 1, 1))
        self.assertEqual(result['memory_sample_status'], 'partial_sources')
        with path.open('a') as stream:
            stream.write('PERF_RECORD_LOST 12\n')
        self.assertEqual(memory_samples(path)['memory_sample_status'], 'lost_samples')
        path.write_text('unexpected format\n')
        with self.assertRaises(ValueError):
            memory_samples(path)

    def test_final_snapshot_and_counter_reset(self):
        before = dict(monotonic_s=1, host_pswpin_pages=10, host_pswpout_pages=20,
                      Rss=100, Swap=2, cpu_ticks=50, minor_faults=4, major_faults=6, starttime_ticks=1)
        after = dict(before, monotonic_s=3, host_pswpin_pages=13, host_pswpout_pages=25,
                     Rss=200, minor_faults=9, major_faults=8)
        with patch('measurements.collect.snapshot', side_effect=[before, after]):
            with ProcessSampler(123, self.folder, interval=100) as sampler:
                self.assertFalse(sampler.result)
        self.assertEqual(sampler.result['swapin_pages'], 3)
        self.assertEqual(sampler.result['swapout_pages'], 5)
        self.assertEqual(sampler.result['major_faults'], 2)
        self.assertEqual(sampler.result['rss_mean_kib'], 150)
        reset = summarize_samples([before, dict(after, host_pswpout_pages=0, starttime_ticks=2)])
        self.assertIsNone(reset['swapout_pages'])
        self.assertIsNone(reset['major_faults'])
        self.assertEqual(reset['swap_status'], 'counter_reset')

    def test_command_failure_keeps_boundary_metrics(self):
        with self.assertRaises(subprocess.CalledProcessError):
            capture(os.getpid(), self.folder, [sys.executable, '-c', 'raise SystemExit(7)'], profiles=[])
        result = read(self.folder / 'metrics.json')
        self.assertEqual(result['capture_status'], 'failed')
        self.assertEqual(result['process_sample_status'], 'ok')
        self.assertIsNotNone(result['swapout_pages'])
        self.assertEqual(read(self.folder / 'replay.log.exit.json')['returncode'], 7)

    def test_failed_client_still_finalizes_perf(self):
        # A protocol-level perf stand-in checks FIFO lifecycle and exception unwinding.
        fake = self.folder / 'perf'
        fake.write_text('''#!/usr/bin/env python3
import os, signal, sys, time
args = sys.argv
output = args[args.index('-o') + 1]
control, ack = args[args.index('--control') + 1][5:].split(',')
def finish(*_):
    with open(output, 'w') as f:
        for event in ('cycles:u', 'instructions:u', 'cache-references:u', 'cache-misses:u'):
            f.write('100;;' + event + ';1000;100.00;;\\n')
    sys.exit(0)
signal.signal(signal.SIGINT, finish)
with open(control) as commands, open(ack, 'w', buffering=1) as replies:
    for line in commands:
        replies.write('ack\\n')
''')
        fake.chmod(0o755)
        with patch.dict(os.environ, PATH=str(self.folder) + ':' + os.environ['PATH']):
            with self.assertRaises(subprocess.CalledProcessError):
                capture(os.getpid(), self.folder, [sys.executable, '-c', 'raise SystemExit(9)'], profiles=['cpu'])
        self.assertEqual(read(self.folder / 'metrics.json')['cycles'], 100)
        metadata = read(self.folder / 'replay-cpu-metadata.json')
        self.assertEqual(metadata['status'], 'aborted')
        self.assertIsNotNone(metadata['returncode'])
        self.assertFalse(list(self.folder.glob('*.fifo')))

    def test_missing_and_failed_results_are_not_zero(self):
        write(self.folder / 'results.json', [dict(dram_gib=2, mode='baseline', folder='.', status='failed', ops_sec=123)])
        row = configurations(self.folder)[0]
        self.assertFalse(row['valid_full_trace_comparison'])
        self.assertIsNone(row['throughput_ops_sec'])
        self.assertIsNone(row['rss_end_kib'])
        self.assertIsNone(row['pmem_samples'])

    def test_capture_samples_do_not_claim_trace_throughput(self):
        write(self.folder / 'metrics.json', dict(capture_status='complete', dram_samples=5,
                                                memory_sample_status='ok'))
        row = configurations(self.folder)[0]
        self.assertTrue(row['measurement_complete'])
        self.assertFalse(row['valid_full_trace_comparison'])
        self.assertEqual(row['dram_samples'], 5)
        self.assertIsNone(row['throughput_ops_sec'])

    def test_integrity_failure_cancels_client(self):
        values = {}

        def command(request):
            fields = request.split(' ', 2)
            if fields[0] == 'SET':
                values[fields[1]] = fields[2].encode()
                return b'OK'
            return values[fields[1]]

        with self.assertRaisesRegex(RuntimeError, 'Integrity check failed'):
            with watch(command, self.folder, interval=.01) as cancel:
                values[next(iter(values))] = b'corrupted'
                self.assertTrue(cancel.wait(2))
        self.assertFalse(read(self.folder / 'integrity-status.json')['passed'])

    def test_swap_rates_use_actual_intervals(self):
        path = self.folder / 'replay-memory.jsonl'
        path.write_text('\n'.join(json.dumps(dict(monotonic_s=t, host_pswpin_pages=n, host_pswpout_pages=n))
                                  for t, n in [(1, 10), (3, 16), (4, 2)]))
        rows = swap_series(self.folder)
        self.assertEqual(rows[0]['swapin_pages_per_sec'], 3)
        self.assertIsNone(rows[1]['swapin_pages_per_sec'])


class TraceProtocol(unittest.TestCase):
    """Exercise both real clients against fragmented, missing, and invalid replies."""
    def test_clients(self):
        client_dir = Path(__file__).resolve().parents[2] / 'traceReader'
        clients = {name: client_dir / f'twitterZst{name.title()}.x' for name in ('loader', 'replay')}
        if not all(p.exists() for p in clients.values()):
            self.skipTest('Build Twitter trace clients before running protocol tests')
        cases = [('loader', 'set', b'OK\n', 0), ('loader', 'set', b'ERR\n', 1),
                 ('loader', 'set', None, 1), ('replay', 'set', b'OK\n', 0),
                 ('replay', 'set', b'ERR\n', 1), ('replay', 'get', None, 1),
                 ('replay', 'get', [b'ab', b'cdef\n'], 0), ('replay', 'get', b'ERR\n', 0),
                 ('replay', 'get', b'abc\ndef\n', 1), ('replay', 'get', b'\n', 1)]
        for client, operation, reply, expected in cases:
            with self.subTest(client=client, operation=operation, reply=reply), tempfile.TemporaryDirectory() as temp:
                folder = Path(temp)
                trace = folder / 'one.zst'
                trace.write_bytes(subprocess.check_output(['zstd', '-q', '-c'],
                                  input=f'1,frame-key,9,80,0,{operation},0\n'.encode()))
                endpoint = str(folder / 'server.sock')
                errors = []
                with socket.socket(socket.AF_UNIX) as listener:
                    listener.bind(endpoint)
                    listener.listen(1)
                    listener.settimeout(10)

                    def respond():
                        try:
                            with listener.accept()[0] as conn:
                                conn.settimeout(10)
                                data = b''
                                while not data.endswith(b'\n'):
                                    part = conn.recv(65536)
                                    if not part:
                                        raise RuntimeError('Client closed request')
                                    data += part
                                if operation == 'set':
                                    fields = data.rstrip(b'\n').split(b' ', 2)
                                    self.assertEqual(fields[:2], [b'set', b'frame-key'])
                                    self.assertEqual(len(fields[2]), 80)
                                    self.assertTrue(fields[2].isalnum())
                                else:
                                    self.assertEqual(data, b'get frame-key\n')
                                if reply is not None:
                                    for chunk in reply if isinstance(reply, list) else [reply]:
                                        conn.sendall(chunk)
                                        time.sleep(.01)
                        except Exception as exc:
                            errors.append(exc)

                    thread = threading.Thread(target=respond)
                    thread.start()
                    argv = [str(clients[client]), str(trace), endpoint, '0', '1']
                    if client == 'replay':
                        argv.append('1')
                    try:
                        result = subprocess.run(argv, capture_output=True, text=True, timeout=15)
                    finally:
                        thread.join(11)
                    self.assertFalse(thread.is_alive())
                    self.assertFalse(errors, errors)
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                    if client == 'replay' and expected == 0:
                        path = folder / 'replay.log'
                        path.write_text(result.stdout)
                        settings = dict(replay_rows=1, expected_counts=dict(GET=int(operation == 'get'),
                                        SET=int(operation == 'set'), DEL=0), allow_misses=True)
                        self.assertEqual(parse_replay(path, settings)['operations'], 1)


if __name__ == '__main__':
    unittest.main()
