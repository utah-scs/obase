"""Pure parsers for perf stat and numeric Linux perf memory sources."""
import csv
import math
import re

CPU_EVENTS = {"cycles": "cycles:u", "instructions": "instructions:u",
              "cache_references": "cache-references:u", "cache_misses": "cache-misses:u"}
UNCORE_EVENTS = {
    'dram_read': 'uncore_imc/event=0x04,umask=0x03/',
    'dram_write': 'uncore_imc/event=0x04,umask=0x0c/',
    'pmem_read': 'uncore_imc/event=0xe3,umask=0x00/',
    'pmem_write': 'uncore_imc/event=0xe7,umask=0x00/',
}


def perf_counts(path, events):
    """Parse scaled perf counts; never turn missing measurements into zeros."""
    counts = dict.fromkeys(events)
    coverage = dict.fromkeys(events)
    seen = set()
    for fields in csv.reader(path.read_text().splitlines(), delimiter=';'):
        if len(fields) < 5 or fields[2] not in events.values():
            continue
        name = next(k for k, v in events.items() if v == fields[2])
        if name in seen:
            raise ValueError(f'Duplicate perf event in {path}: {name}')
        seen.add(name)
        try:
            count, runtime, running = map(float, (fields[0], fields[3], fields[4]))
        except ValueError:
            continue
        if (all(math.isfinite(v) for v in (count, runtime, running)) and
                count >= 0 and runtime > 0 and 0 < running <= 100):
            counts[name], coverage[name] = count, running
    status = ('unavailable' if None in counts.values() else
              'low_coverage' if min(coverage.values()) < 99 else 'ok')
    return counts, coverage, status


def uncore_metrics(path, seconds):
    counts, coverage, status = perf_counts(path, UNCORE_EVENTS)
    result = {'uncore_status': status, 'uncore_window_seconds': seconds}
    for name in UNCORE_EVENTS:
        result.update({'uncore_' + name + '_requests': counts[name],
                       'uncore_' + name + '_running_pct': coverage[name],
                       'uncore_' + name + '_mib_sec':
                           counts[name] * 64 / 1048576 / seconds if status == 'ok' else None})
    return result


def memory_samples(path):
    """Classify perf's numeric data_src using linux/perf_event.h bit fields."""
    counts = dict.fromkeys(('dram_samples', 'pmem_samples', 'cache_samples', 'unknown_samples'), 0)
    lost = False
    with path.open() as handle:
        for line in handle:
            if 'LOST' in line.upper():
                lost = True
                continue
            if not line.strip() or line.startswith('#'):
                continue
            match = re.match(r'\s*[\d.]+:\s+[0-9a-f]+\s+([0-9a-f]+)\s+\|', line)
            if not match:
                raise ValueError(f'Unrecognized perf sample in {path}: {line[:160]}')
            source = int(match[1], 16)
            level, legacy = (source >> 33) & 0xf, (source >> 5) & 0x3fff
            if level == 14:
                kind = 'pmem'
            elif level == 13 or (level in (0, 15) and legacy & 0x380):
                kind = 'dram'
            elif level in (1, 2, 3, 4, 11, 12) or (level in (0, 15) and legacy & 0xc78):
                kind = 'cache'
            else:
                kind = 'unknown'
            counts[kind + '_samples'] += 1
    total = sum(counts.values())
    status = ('lost_samples' if lost else 'no_samples' if not total else
              'source_unavailable' if counts['unknown_samples'] == total else
              'partial_sources' if counts['unknown_samples'] else 'ok')
    return dict(counts, memory_samples_total=total, memory_sample_status=status)
