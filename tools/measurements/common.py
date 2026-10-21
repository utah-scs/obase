"""File and process helpers shared by collectors and the trace runner."""
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def read(path):
    return json.loads(Path(path).read_text())


def write(path, data):
    path = Path(path)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(data, indent=2, allow_nan=False) + '\n')
    temporary.replace(path)


def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
            result.update(block)
    return result.hexdigest()


def stop_process(process, sudo=False):
    """Reap an owned process group, including descendants of a finished leader."""
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
        try:
            if sudo:
                subprocess.run(['sudo', '-n', 'kill', '-' + sig.name[3:], '--',
                                f'-{process.pid}'], stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL, timeout=5, check=False)
            else:
                os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            continue
        # Client/perf wrappers may leave descendants behind; check the group.
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            break
        except PermissionError:
            if not sudo:
                raise
    process.wait(timeout=5)


def run(argv, log, timeout=7200, cancel=None):
    argv = list(map(str, argv))
    started, error, process = time.monotonic(), None, None
    try:
        with Path(log).open('wb') as output:
            process = subprocess.Popen(argv, stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            while True:
                if cancel is not None and cancel.is_set():
                    raise RuntimeError('Integrity check failed; see integrity-checks.jsonl')
                remaining = timeout - (time.monotonic() - started)
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(argv, timeout)
                try:
                    rc = process.wait(min(1, remaining))
                    break
                except subprocess.TimeoutExpired:
                    pass
            if rc:
                raise subprocess.CalledProcessError(rc, argv)
    except BaseException as exc:
        error = repr(exc)
        raise
    finally:
        try:
            if process is not None:
                stop_process(process)
        finally:
            write(str(log) + '.exit.json', dict(argv=argv,
                  returncode=process.returncode if process else None,
                  elapsed_s=time.monotonic() - started, error=error))
