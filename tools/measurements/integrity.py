"""Small immutable sentinel set checked during every measured trace replay."""
from contextlib import contextmanager
import hashlib
import json
import threading
import time
import uuid

from .common import write


@contextmanager
def watch(command, folder, interval=5):
    prefix = '__crest_measure_' + uuid.uuid4().hex
    values = {}
    for index, size in enumerate((64, 320, 4096, 8192) * 2):
        token = f'guard{index:02d}0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'
        values[f'{prefix}_{index}'] = (token * (size // len(token) + 1))[:size].encode()
    for key, value in values.items():
        if command('SET ' + key + ' ' + value.decode()) != b'OK':
            raise RuntimeError('Failed to install integrity sentinel: ' + key)
    write(folder / 'integrity-values.json', {k: dict(bytes=len(v), sha256=hashlib.sha256(v).hexdigest())
                                            for k, v in values.items()})
    failed, done = threading.Event(), threading.Event()
    result = dict(passed=True, rounds=0, values_per_round=len(values), interval_seconds=interval, errors=[])

    def check():
        row = dict(monotonic_s=time.monotonic(), passed=False, checked=0)
        try:
            for key, expected in values.items():
                actual = command('GET ' + key)
                if actual != expected:
                    row.update(key=key, expected_bytes=len(expected), actual_bytes=len(actual),
                               actual_sha256=hashlib.sha256(actual).hexdigest())
                    raise RuntimeError('CORRUPTION: sentinel value changed: ' + key)
                row['checked'] += 1
            result['rounds'] += 1
            row['passed'] = True
        except Exception as exc:
            result['errors'].append(repr(exc))
            result['passed'] = False
            failed.set()
            row['error'] = repr(exc)
        finally:
            with (folder / 'integrity-checks.jsonl').open('a') as stream:
                stream.write(json.dumps(row) + '\n')

    def worker():
        while not done.wait(interval):
            check()
            if failed.is_set():
                return

    check()
    thread = threading.Thread(target=worker)
    thread.start()
    try:
        if failed.is_set():
            raise RuntimeError('Initial integrity check failed')
        yield failed
    finally:
        done.set()
        thread.join()
        if not failed.is_set():
            check()
        write(folder / 'integrity-status.json', result)
    if failed.is_set():
        raise RuntimeError('Integrity check failed; see integrity-checks.jsonl')
