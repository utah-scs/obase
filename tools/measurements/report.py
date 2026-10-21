"""Normalize new captures and archived sweeps without rewriting source artifacts."""
import csv
import json
from pathlib import Path

from .collect import counter_delta
from .common import digest, read, write

NOTES = [
    'Swap counts are host-wide base pages, including readahead; other workloads can contribute.',
    'Uncore bandwidth is host-wide across all sockets, not attributable to this server alone.',
    'Memory samples are sampled userspace loads above the configured latency threshold, not all accesses.',
    'With PMEM-backed swap, zero userspace PMEM samples does not mean zero PMEM traffic.',
    'Missing measurements remain null. Failed trials are excluded from full-trace throughput comparisons.',
    'Legacy swap-out and page-fault totals use the original sampled window; new captures include a final boundary.',
    'One trial per mode and size is descriptive; it does not establish statistical significance.',
]


def csv_write(path, rows):
    if not rows:
        raise ValueError('No measurements to write')
    with Path(path).open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def trial_folder(root, result):
    # Prefer files beside results.json so archived runs remain relocatable.
    recorded = Path(result.get('folder', '.'))
    local = root / recorded.name if recorded.is_absolute() else root / recorded
    if not local.is_dir():
        raise FileNotFoundError(local)
    return local


def configurations(root):
    root = Path(root).resolve()
    results = read(root / 'results.json') if (root / 'results.json').exists() else [
        dict(read(root / 'metrics.json'), mode='capture', dram_gib=None, folder='.', status='capture')]
    rows = []
    for result in results:
        folder = trial_folder(root, result)
        complete = result['status'] in ('passed', 'completed_with_metric_warnings')
        row = dict(dram_cap_gib=result.get('dram_gib'), mode=result['mode'], status=result['status'],
                   valid_full_trace_comparison=complete,
                   measurement_complete=complete or (result['status'] == 'capture'
                                                      and result.get('capture_status') == 'complete'),
                   throughput_ops_sec=result.get('ops_sec') if complete else None,
                   operations=result.get('operations'), misses=result.get('misses'),
                   elapsed_seconds=result.get('replay_seconds'),
                   swapin_pages=result.get('swapin_pages'),
                   swapout_pages=result.get('swapout_pages', result.get('host_swapout_pages')),
                   swapout_window='boundary' if 'swapout_pages' in result else 'legacy sampled',
                   minor_faults=result.get('minor_faults'), major_faults=result.get('major_faults'),
                   fault_window='boundary' if 'capture_status' in result else 'legacy sampled',
                   dram_samples=result.get('dram_samples'), pmem_samples=result.get('pmem_samples'),
                   total_samples=result.get('memory_samples_total'), sample_status=result.get('memory_sample_status'),
                   cpu_perf_status=result.get('cpu_perf_status'), uncore_status=result.get('uncore_status'),
                   process_sample_status=result.get('process_sample_status'),
                   swap_status=result.get('swap_status', result.get('swapin_status')),
                   ipc=result.get('ipc'), rss_mean_kib=result.get('rss_mean_kib'),
                   rss_peak_kib=result.get('rss_peak_kib'), rss_end_kib=result.get('rss_end_kib'),
                   swap_end_kib=result.get('swap_end_kib'),
                   get_p99_us=result.get('get_p99_us'), set_p99_us=result.get('set_p99_us'),
                   migration_cycles=result.get('migration_cycles'), folder=str(folder))
        for tier in ('dram', 'pmem'):
            for direction in ('read', 'write'):
                row[f'host_{tier}_{direction}_mib_s'] = result.get(f'uncore_{tier}_{direction}_mib_sec')
        boundary = folder / 'replay-swap.json'
        if not boundary.exists():
            boundary = folder / 'replay-swapins.json'
        if boundary.exists():
            swap = read(boundary)
            for field, new_key, old_key in (('swapin_pages', 'host_pswpin_pages', 'pswpin'),
                                             ('swapout_pages', 'host_pswpout_pages', 'pswpout')):
                key = new_key if new_key in swap['before'] else old_key
                delta = counter_delta(swap['before'], swap['after'], key)
                if row[field] is not None and delta is not None and delta != row[field]:
                    raise ValueError(f'{folder}: {field} disagrees with boundary artifact')
            row['page_size_bytes'] = swap.get('page_size')
        else:
            row['page_size_bytes'] = None
        rows.append(row)
    return rows


def summarize(root, output):
    root, output = Path(root).resolve(), Path(output).resolve()
    rows = configurations(root)
    output.mkdir(parents=True, exist_ok=False)
    setup = read(root / 'setup.json') if (root / 'setup.json').exists() else {}
    sources = {name: digest(root / name) for name in ('setup.json', 'results.json', 'metrics.json', 'status.json')
               if (root / name).exists()}
    summary = dict(source=str(root), source_sha256=sources, structure=setup.get('structure'),
                   setup=setup, notes=NOTES, configurations=rows,
                   run_status=read(root / 'status.json') if (root / 'status.json').exists() else None)
    write(output / 'summary.json', summary)
    csv_write(output / 'comparison.csv', rows)
    return summary


def swap_series(folder):
    """Rates use adjacent actual samples; resets/missing counters are not clamped to zero."""
    samples = [json.loads(line) for line in (Path(folder) / 'replay-memory.jsonl').read_text().splitlines()]
    if len(samples) < 2:
        return []
    result = []
    for before, after in zip(samples, samples[1:]):
        seconds = after['monotonic_s'] - before['monotonic_s']
        if seconds <= 0:
            raise ValueError('Sample times must increase')
        row = dict(seconds=after['monotonic_s'] - samples[0]['monotonic_s'], interval_seconds=seconds,
                   cgroup_swap_bytes=after.get('cgroup', {}).get('memory.swap.current'))
        for label, key in (('swapin', 'host_pswpin_pages'), ('swapout', 'host_pswpout_pages')):
            delta = counter_delta(before, after, key)
            row[label + '_pages_per_sec'] = delta / seconds if delta is not None else None
            row[label + '_cumulative_pages'] = counter_delta(samples[0], after, key)
        result.append(row)
    return result
