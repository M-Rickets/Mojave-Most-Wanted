"""Generate MojaveMostWanted.esp from the sheets (data records only)."""
import json, struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
import esp
S = lambda n: json.load(open(os.path.join(os.path.dirname(__file__), '..', 'sheets', n + '.json')))

def fid(s): return int(s, 16)

def stat(edid, formid, path, bounds):
    return esp.record('STAT', formid, [esp.sub('EDID', edid), esp.obnd(*bounds), esp.sub('MODL', path)])

def soun(row):
    flags = 0x10 if row['loop'] else 0
    sndd = struct.pack('<BBbBIhBB5hhiii', 10, 30, 0, 0, flags, 0, 0, 0, 100, 50, 20, 5, 0, 0, 0, 0, 0)
    return esp.record('SOUN', fid(row['formId']), [esp.sub('EDID', row['edid']), esp.obnd(0, 0, 0, 0, 0, 0), esp.sub('FNAM', row['outFile']), esp.sub('SNDD', sndd)])

def main(out):
    cars, sounds = S('cars')['rows'], S('sounds')['rows']
    bounds = (-110, -170, 0, 110, 170, 110)
    stats, souns = [], []
    ready = lambda r: 'TBD' not in json.dumps(r.get('vehicleId', '')) and 'TBD' not in json.dumps(r.get('bundles', ''))
    for r in cars:
        if not ready(r): continue
        stats.append(stat(r['driveEdid'], fid(r['driveFormId']), 'mmw\\cars\\%s\\drive.nif' % r['id'], bounds))
        stats.append(stat(r['staticEdid'], fid(r['staticFormId']), 'mmw\\cars\\%s\\drive.nif' % r['id'], bounds))
    ligh = []
    for r in S('lights')['rows']:
        data = struct.pack('<iI4BIffIf', -1, r['radius'], r['color'][0], r['color'][1], r['color'][2], 0, 0x1, 1.0, 90.0, 0, 0.0)
        ligh.append(esp.record('LIGH', fid(r['formId']), [esp.sub('EDID', r['edid']), esp.obnd(-8, -8, -8, 8, 8, 8), esp.sub('DATA', data), esp.sub('FNAM', struct.pack('<f', r['fade']))]))
    conts = [esp.record('CONT', fid(r['formId']), [esp.sub('EDID', r['edid']), esp.obnd(-8, -8, 0, 8, 8, 16), esp.sub('FULL', r['name']), esp.sub('MODL', r['model']), esp.sub('DATA', struct.pack('<Bf', 0, 0.0))]) for r in S('containers')['rows']]
    mstt = []
    for r in S('fx')['rows']:
        for i in range(r.get('variants', 1)):
            model = r['model'] % i if '%d' in r['model'] else r['model']
            mstt.append(esp.record('MSTT', fid(r['formId']) + i, [esp.sub('EDID', r['edid'] + ('%d' % i if r.get('variants', 1) > 1 else '')), esp.obnd(-20, -20, 0, 20, 20, 40), esp.sub('MODL', model), esp.sub('DATA', b'\x00')]))
    esp.write(out, 'Mojave Most Wanted', 'Need for Speed Most Wanted in the Mojave. Models, textures, sounds and music come from your own copies of the Need for Speed games.',
              [('SOUN', souns), ('CONT', conts), ('LIGH', ligh), ('STAT', stats), ('MSTT', mstt)])
    print('wrote', out, len(conts), 'CONT', len(stats), 'STAT', len(ligh), 'LIGH', len(souns), 'SOUN', len(mstt), 'MSTT')

if __name__ == '__main__':
    main(sys.argv[1])
