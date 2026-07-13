#!/usr/bin/env python3
"""
DELETE stress for CrestDB: mixed SET/GET/DEL/SCAN traffic from several
threads while OBASE decay+migration runs, verifying:
  - a GET after SET returns the exact value
  - a GET after DEL returns ERR
  - a persistent (never-deleted) key set stays byte-exact throughout
Exit code 0 = no integrity violations.

Usage: verify_delete.py [threads] [seconds]  (server must be running,
migration mode is the caller's choice)
"""
import socket
import sys
import threading
import time

SOCK = "/tmp/server.sock"
THREADS = int(sys.argv[1]) if len(sys.argv) > 1 else 4
SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 60

errors = []
counts = {"set": 0, "get": 0, "del": 0, "scan": 0}
lock = threading.Lock()


class Conn:
    def __init__(self):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.connect(SOCK)
        self.buf = b""

    def cmd(self, line):
        self.s.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            d = self.s.recv(65536)
            if not d:
                raise IOError("closed")
            self.buf += d
        resp, self.buf = self.buf.split(b"\n", 1)
        return resp.decode()


def fail(msg):
    with lock:
        errors.append(msg)
        if len(errors) < 10:
            print("INTEGRITY: " + msg)


def worker(tid):
    c = Conn()
    deadline = time.time() + SECONDS
    # persistent keys this thread owns and re-verifies forever
    persist = {}
    for i in range(50):
        k = "persist%d:field%d" % (tid, i)
        v = "persistent value %d %d abcdefghijklmnopqrstuvwxyz" % (tid, i)
        if c.cmd("SET %s %s" % (k, v)) != "OK":
            fail("persist SET failed " + k)
        persist[k] = v

    i = 0
    while time.time() < deadline:
        i += 1
        k = "churn%d:key%d" % (tid, i % 500)
        v = "value %d-%d with some payload padding 0123456789" % (tid, i)
        if c.cmd("SET %s %s" % (k, v)) != "OK":
            fail("SET failed " + k)
        counts["set"] += 1
        got = c.cmd("GET " + k)
        counts["get"] += 1
        if got != v:
            fail("GET after SET mismatch %s: %r != %r" % (k, got[:40], v[:40]))
        if c.cmd("DEL " + k) != "OK":
            fail("DEL failed " + k)
        counts["del"] += 1
        got = c.cmd("GET " + k)
        counts["get"] += 1
        if got != "ERR":
            fail("GET after DEL returned data for %s: %r" % (k, got[:40]))
        # periodically verify the persistent set + a scan
        if i % 100 == 0:
            for pk, pv in persist.items():
                got = c.cmd("GET " + pk)
                counts["get"] += 1
                if got != pv:
                    fail("persistent key corrupted %s: %r" % (pk, got[:40]))
            resp = c.cmd("SCAN persist%d:field0 10" % tid)
            counts["scan"] += 1
            if not (resp.startswith("OK ") or resp == "ERR unsupported"):
                fail("SCAN bad response: %r" % resp[:60])


ts = [threading.Thread(target=worker, args=(t,)) for t in range(THREADS)]
for t in ts:
    t.start()
for t in ts:
    t.join()

print("ops: %r, integrity errors: %d" % (counts, len(errors)))
sys.exit(1 if errors else 0)
