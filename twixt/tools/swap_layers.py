#!/usr/bin/env python3
"""Swap converted layer files (from --convert-layers) into a layer store.

Run only while no solver uses the store (driver stopped between steps).
For each entry of CONVERTED/LAYERS.txt whose kind/ply/file/node count match
the store's manifest, the converted file replaces the store's file (via a
hard link and rename) and the manifest entry gets the new size and checksum.
The manifest is rewritten atomically.

usage: swap_layers.py STORE CONVERTED
"""
import os
import sys

store, converted = sys.argv[1], sys.argv[2]
manifest = os.path.join(store, 'manifest.twxman')
done = {}
for line in open(os.path.join(converted, 'LAYERS.txt')):
    kind, ply, name, size, checksum, nodes = line.split()
    path = os.path.join(converted, name)
    if os.path.getsize(path) != int(size):
        raise SystemExit('size mismatch: ' + path)
    done[(kind, int(ply))] = (name, size, checksum, nodes)
lines, swapped = [], 0
for line in open(manifest):
    f = line.split()
    if len(f) == 6 and f[0] in ('forward', 'result') and (f[0], int(f[1])) in done:
        name, size, checksum, nodes = done[(f[0], int(f[1]))]
        if name != f[2] or nodes != f[5]:
            raise SystemExit('converted entry does not match the manifest: ' + line.strip())
        if size != f[3] or checksum != f[4]:
            target = os.path.join(store, name)
            temporary = target + '.swap'
            if os.path.exists(temporary):
                os.remove(temporary)
            os.link(os.path.join(converted, name), temporary)
            os.replace(temporary, target)
            swapped += 1
        line = '%s %s %s %s %s %s\n' % (f[0], f[1], name, size, checksum, nodes)
    lines.append(line)
temporary = manifest + '.partial'
with open(temporary, 'w') as out:
    out.writelines(lines)
    out.flush()
    os.fsync(out.fileno())
os.replace(temporary, manifest)
print('swapped %d layer files; manifest updated' % swapped)
