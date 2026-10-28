"""Minimal Fallout: New Vegas plugin (.esp) writer for data-only records (no scripts).
Layouts follow xEdit's wbDefinitionsFNV.pas (record header 24 bytes, form version 15)."""
import struct

def sub(tag, data):
    if isinstance(data, str): data = data.encode('cp1252') + b'\0'
    assert len(data) < 65536, tag
    return tag.encode() + struct.pack('<H', len(data)) + data

def record(tag, formid, subs, flags=0):
    body = b''.join(subs)
    return tag.encode() + struct.pack('<IIIIHH', len(body), flags, formid, 0, 15, 0) + body

def grup(label, records):
    body = b''.join(records)
    return b'GRUP' + struct.pack('<I', 24 + len(body)) + label.encode() + struct.pack('<iHHHH', 0, 0, 0, 15, 0) + body

def tes4(num_records, next_id, author, desc, masters=('FalloutNV.esm',)):
    subs = [sub('HEDR', struct.pack('<fiI', 1.34, num_records, next_id)), sub('CNAM', author), sub('SNAM', desc)]
    for m in masters:
        subs += [sub('MAST', m), sub('DATA', struct.pack('<Q', 0))]
    return record('TES4', 0, subs)

def obnd(x0, y0, z0, x1, y1, z1):
    return sub('OBND', struct.pack('<6h', *[int(max(-32768, min(32767, v))) for v in (x0, y0, z0, x1, y1, z1)]))

def write(path, author, desc, groups):
    """groups: list of (TYPE, [record bytes]) in the order FNV expects."""
    n = sum(len(r) for _, r in groups) + len(groups)
    nxt = 0x800
    body = b''.join(grup(t, rs) for t, rs in groups if rs)
    with open(path, 'wb') as f:
        f.write(tes4(n, nxt, author, desc) + body)
