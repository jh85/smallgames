#!/usr/bin/env python3
"""Summarize a TWIXT_PROFILE trace: pool-slot sources per wave and RSS owners."""
import collections
import sys

GiB = float(1 << 30)
UMAP_ENTRY = 32   # libstdc++ node (24 B: next + 12 B key + 4 B value) in a 32 B glibc chunk
BUCKET = 8


def parse(path):
    rows = []
    with open(path) as f:
        for line in f:
            parts = line.rstrip('\n').split(',')
            if len(parts) < 2:
                continue
            row = {'t': float(parts[0]), 'event': parts[1]}
            for item in parts[2:]:
                k, _, v = item.partition('=')
                try:
                    row[k] = int(v)
                except ValueError:
                    try:
                        row[k] = float(v)
                    except ValueError:
                        row[k] = v
            rows.append(row)
    return rows


def main(path):
    rows = parse(path)
    steps = [r for r in rows if r['event'] == 'step' and r.get('ply', -1) >= 0]
    jobs = [r for r in rows if r['event'] == 'job']
    unions = {(r['ply'], r['wave']): r for r in rows if r['event'] == 'union'}
    gcs = {(r['ply'], r['wave']): r for r in rows if r['event'] == 'gc'}
    census = [r for r in rows if r['event'] == 'census']
    job_wave = {(r['ply'], r['job']): r['wave'] for r in jobs}

    per_wave = collections.defaultdict(lambda: collections.Counter())
    for s in steps:
        wave = job_wave.get((s['ply'], s['job']))
        if wave is None:
            continue
        kind = s['kind']
        per_wave[(s['ply'], wave)][kind] += s['created']
    for j in jobs:
        c = per_wave[(j['ply'], j['wave'])]
        c['jobs'] += 1
        c['job_created'] += j['created']
        c['job_live_new'] += j.get('new_reach', 0)
        c['job_reach'] += j.get('reach', 0)

    print('== Pool slots created per wave (millions) ==')
    print('ply wave jobs  retained  filter   cbit    flip  img_total img_live_new  union  union_live  gaps   slots_preGC  after_GC')
    pre = {(r['ply'], r['wave']): r for r in census if r['label'] == 'pre_gc'}
    start = {(r['ply'], r['wave']): r for r in census if r['label'] in ('wave_start', 'post_gc')}
    totals = collections.Counter()
    for key in sorted(per_wave):
        ply, wave = key
        c = per_wave[key]
        u = unions.get(key, {})
        g = gcs.get(key, {})
        p = pre.get(key, {})
        base = None
        if wave == 0:
            ws = [r for r in census if r['label'] == 'wave_start' and r['ply'] == ply]
            base = ws[0]['slots'] if ws else None
        else:
            prev = [r for r in census if r['label'] == 'post_gc' and r['ply'] == ply and r['wave'] == wave - 1]
            base = prev[0]['slots'] if prev else None
        gaps = (p.get('discarded', 0) + p.get('tail', 0)) if p else 0
        m = 1e6
        print('%3d %4d %4d %9.1f %7.1f %7.1f %7.1f %9.1f %10.1f %7.1f %10.1f %6.2f %10.1f %9.1f' % (
            ply, wave, c['jobs'], (base or 0) / m, c['filter'] / m, c['cbit'] / m, c['flip'] / m,
            c['job_created'] / m, c['job_live_new'] / m, u.get('created', 0) / m,
            u.get('new_reach', 0) / m, gaps / m, g.get('before', 0) / m, g.get('after', 0) / m))
        for k in ('filter', 'cbit', 'flip', 'job_created', 'job_live_new'):
            totals[k] += c[k]
        totals['union'] += u.get('created', 0)
        totals['gaps'] += gaps
    if totals['job_created']:
        jc = totals['job_created']
        print('share of image-created slots: filter %.1f%%, cbit %.1f%%, flip %.1f%%; '
              'live-new at job end %.1f%%' % (100 * totals['filter'] / jc, 100 * totals['cbit'] / jc,
                                               100 * totals['flip'] / jc, 100 * totals['job_live_new'] / jc))
        print('union created / image created = %.2f; gaps / all created = %.3f%%' % (
            totals['union'] / jc, 100 * totals['gaps'] / max(1, jc + totals['union'])))

    print('\n== Per-move image steps (ply, move: created M by kind; result reach M; seconds) ==')
    by_job = collections.defaultdict(list)
    for s in steps:
        by_job[(s['ply'], s['job'])].append(s)
    for j in sorted(jobs, key=lambda r: (r['ply'], r['job'])):
        ss = by_job[(j['ply'], j['job'])]
        desc = ' '.join('%s%s=%.2f' % (s['kind'][0], '' if s['k'] < 0 else s['k'], s['created'] / 1e6) for s in ss)
        print('ply %d job %2d move %2d created=%.2fM live_new=%.2fM reach=%.2fM t=%.1fs | %s' % (
            j['ply'], j['job'], j['move'], j['created'] / 1e6, j.get('new_reach', 0) / 1e6,
            j.get('reach', 0) / 1e6, j['seconds'], desc))

    print('\n== RSS owners at census points (GiB) ==')
    print('label          ply wave   slots(M) unused%  nodes  unique  filter(cap/used) apply  prod  est_sum   RSS    VSZ  m_inuse m_free m_mmap')
    for r in census:
        nodes = r['blocks'] * (1 << 16) * 16
        unique = r['unique_bytes'] if 'unique_bytes' in r else r['unique_entries'] * UMAP_ENTRY + r['unique_buckets'] * BUCKET
        apply = sum(r['apply_%s' % op] * UMAP_ENTRY + r['apply_%s_buckets' % op] * BUCKET for op in 'uid')
        prod = r['products'] * UMAP_ENTRY + r['product_buckets'] * BUCKET
        est = nodes + unique + r['filter_used'] + apply + prod
        print('%-14s %3d %4d %9.1f %6.2f %6.1f %7.1f %6.1f/%5.1f %6.2f %5.2f %7.1f %6.1f %6.1f %7.1f %6.1f %6.1f' % (
            r['label'], r['ply'], r['wave'], r['slots'] / 1e6, 100.0 * r['unused'] / max(1, r['slots']),
            nodes / GiB, unique / GiB, r['filter_bytes'] / GiB, r['filter_used'] / GiB, apply / GiB, prod / GiB,
            est / GiB, r['rss'] / GiB, r['vsz'] / GiB, r['m_inuse'] / GiB, r['m_free'] / GiB, r['m_mmap'] / GiB))

    print('\n== add_layer steps ==')
    for r in rows:
        if r['event'] == 'layer_step' and r['created'] > 10000:
            print('ply %d %-14s created=%.2fM won_memo=%.2fM rss=%.1f GiB t=%.1fs' % (
                r['ply'], r['kind'], r['created'] / 1e6, r['won_memo'] / 1e6,
                r['rss'] / GiB, r['seconds']))


if __name__ == '__main__':
    main(sys.argv[1])
