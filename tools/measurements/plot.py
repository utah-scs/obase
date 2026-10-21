"""Plots consume the same normalized rows as the CSV report."""
from pathlib import Path

from .common import write
from .report import csv_write, summarize, swap_series


def plot(root, output):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MaxNLocator
    import numpy as np

    summary = summarize(root, output)
    rows = summary['configurations']
    output = Path(output)
    sizes = list(dict.fromkeys(r['dram_cap_gib'] for r in rows))
    modes = list(dict.fromkeys(r['mode'] for r in rows))
    title = summary.get('structure') or 'Server'
    pairs = {(r['dram_cap_gib'], r['mode']): r for r in rows}
    if len(pairs) != len(rows):
        raise ValueError('Duplicate size/mode entries; plot independent repetitions separately')

    def save(fig, name):
        for suffix in ('png', 'pdf', 'svg'):
            fig.savefig(output / (name + '.' + suffix), dpi=180, bbox_inches='tight')
        plt.close(fig)

    fig, axes = plt.subplots(1, 2, figsize=(10, 4), squeeze=False)
    for axis, tier in zip(axes[0], ('dram', 'pmem')):
        for index, mode in enumerate(modes):
            selected = [pairs.get((size, mode), {}) for size in sizes]
            values = [r.get(tier + '_samples') if r.get('measurement_complete')
                      and r.get('sample_status') in ('ok', 'partial_sources') else None for r in selected]
            width = .8 / len(modes)
            x = np.arange(len(sizes)) - .4 + width * (index + .5)
            axis.bar(x, [v if v is not None else np.nan for v in values], width=width, label=mode)
            for position, value in zip(x, values):
                if value is None or value == 0:
                    axis.annotate('N/A' if value is None else '0', (position, 0), xytext=(0, 3),
                                  textcoords='offset points', ha='center', fontsize=8)
        axis.set_xticks(np.arange(len(sizes)), ['capture' if s is None else f'{s:g}' for s in sizes])
        axis.set(xlabel='DRAM limit (GiB)', ylabel='Recorded load samples', title=tier.upper())
        axis.set_ylim(bottom=0, top=max(1, axis.get_ylim()[1]))
        axis.yaxis.set_major_locator(MaxNLocator(integer=True))
        axis.legend()
    fig.suptitle(title + ': userspace memory samples (see summary.json for period and latency threshold)')
    fig.tight_layout()
    save(fig, 'memory-samples')

    fig, axes = plt.subplots(len(sizes), 3, figsize=(13, 3 * len(sizes)), squeeze=False)
    series_status = []
    for size, row_axes in zip(sizes, axes):
        for mode in modes:
            row = pairs.get((size, mode))
            if not row:
                continue
            source = Path(row['folder']) / 'replay-memory.jsonl'
            if not source.exists():
                series_status.append(dict(folder=row['folder'], status='missing samples'))
                continue
            series = swap_series(row['folder'])
            if not series:
                continue
            csv_write(output / f'{size}-{mode}-swap-series.csv', series)
            label = mode + (' (partial)' if not row['measurement_complete'] else '')
            time = [s['seconds'] for s in series]
            for axis, key in zip(row_axes, ('swapin_pages_per_sec', 'swapout_pages_per_sec', 'cgroup_swap_bytes')):
                values = [float(s[key]) if s[key] is not None else np.nan for s in series]
                if key == 'cgroup_swap_bytes':
                    values = [v / 2**30 for v in values]
                axis.plot(time, values, label=label, linewidth=.8)
            series_status.append(dict(folder=row['folder'], status='plotted', intervals=len(series)))
        for axis, metric in zip(row_axes, ('Host swap-in (pages/s)', 'Host swap-out (pages/s)', 'Server cgroup swap (GiB)')):
            axis.set(xlabel='Seconds since first sample', ylabel=metric, title=f'{size} GiB')
            if axis.lines:
                axis.legend()
            axis.grid(alpha=.2)
    fig.suptitle(title + ': swap activity; host rates include other workloads')
    fig.tight_layout()
    save(fig, 'swap-activity')
    write(output / 'plot-status.json', series_status)
