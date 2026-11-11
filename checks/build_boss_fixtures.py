"""Derive boss approach coordinates from the checked-in level object source.

These fixtures start via normal level initialization, not mid-frame teleports.
No boss encounter flags or boss outcomes are injected.
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = ROOT / 'sonic3air_source/Oxygen/sonic3air/scripts/standalone/resources/level_objects'
TARGETS = [
    ('AIZ1', '1_91'), ('AIZ2', '1_92'), ('HCZ1', '1_99'), ('HCZ2', '1_9a'),
    ('MGZ1', '1_9f'), ('MGZ2', '1_a1'), ('CNZ1', '1_a6'), ('CNZ2', '1_a7'),
    ('FBZ1', '1_aa'), ('FBZ2', '1_ac'), ('ICZ2', '1_bc'), ('ICZ2', '1_bd'),
    ('LBZ1', '1_c9'), ('LBZ2', '1_ca'), ('MHZ1', '2_92'), ('MHZ2', '2_93'),
    ('SOZ1', '2_97'), ('SOZ2', '2_98'), ('LRZ1', '2_9d'), ('LRZ3', '2_9f'),
    ('SSZ1', '2_a3'), ('DEZ1', '2_a6'), ('DEZ2', '2_a7'),
]

def build():
    sources = list(SOURCES.glob('levelobjects_*.lemon'))
    script_root = SOURCES.parents[2]
    scripts = [(p, re.sub(r'/\*.*?\*/', '', p.read_text(encoding='utf-8-sig'), flags=re.S)) for p in script_root.rglob('*.lemon')]
    definitions = (SOURCES / 'level_objects.lemon').read_text(encoding='utf-8-sig')
    result = {}
    for index, (act, object_id) in enumerate(TARGETS, 1):
        for path in sources:
            source = re.sub(r'/\*.*?\*/', '', path.read_text(encoding='utf-8-sig'), flags=re.S)
            match = re.search(r'function void LevelObjectTableBuilder\.buildObjects_' + act + r'\(\)(.*?)(?=function void|\Z)', source, re.S)
            if not match:
                continue
            # Controllers can supply a boss's scene position without being the
            # boss actor whose health is measured.
            coordinate_id = {'1_c9': '1_c4', '2_92': '2_91'}.get(object_id, object_id)
            entries = list(re.finditer(r'addObject\((0x[0-9a-f]+),\s*(0x[0-9a-f]+),\s*OBJECTTABLE' + coordinate_id + r',', match[1], re.I))
            if entries:
                # AIZ2's upper route is Sonic's; HCZ2 and ICZ select their
                # first entries for Sonic. Later entries can be Knuckles arenas.
                entry = entries[-1] if act == 'AIZ2' else entries[0]
                x, y = (int(entry[g], 16) for g in (1, 2))
                address = re.search(r'constant u8 OBJECTTABLE' + object_id + r'\s*=.*?Object address (0x[0-9a-f]+)', definitions, re.I)[1]
                result[f'H{index:02}'] = dict(x=f'0x{max(x - 0x180, 0):04x}', y=f'0x{max(y - 0x20, 0):04x}', source=path.name, object=object_id, address=address, targetX=f'0x{x:04x}', targetY=f'0x{y:04x}')
            break
        # Some bosses are spawned by camera controllers, not level object tables.
        key = f'H{index:02}'
        if key not in result:
            definition = re.search(r'constant u8 OBJECTTABLE' + object_id + r'\s*=.*?Object address (0x[0-9a-f]+)', definitions, re.I)
            if not definition:
                continue
            address = definition[1]
            for path, source in scripts:
                pattern = r'objA1\.update_address\s*=\s*' + address + r'\s*\n\s*objA1\.position.x.u16\s*=\s*(0x[0-9a-f]+)\s*\n\s*objA1\.position.y.u16\s*=\s*(0x[0-9a-f]+)'
                dynamic = re.search(pattern, source, re.I)
                if dynamic:
                    x, y = (int(dynamic[g], 16) for g in (1, 2))
                    result[key] = dict(x=f'0x{max(x - 0x180, 0):04x}', y=f'0x{max(y - 0x20, 0):04x}', source=str(path.relative_to(script_root)), object=object_id, address=address, targetX=f'0x{x:04x}', targetY=f'0x{y:04x}')
                    break
    # Camera-spawned scenes whose locations are specified by their controllers.
    # These are inputs/provenance, never an injected encounter flag or outcome.
    camera_scenes = {
        'H01': (0x11f0, 0x0289, '0x068a24', 'level/01_aiz/level_aiz.lemon:AIZ1a.checkPreBossArea'),
        'H10': (0x31c0, 0x0690, '0x0705ea', 'level/08_fbz/fbz_dualplane.lemon:fn053372'),
        'H17': (0x439d, 0x09f7, '0x076a12', 'bosses/boss_soz1.lemon:fn076a12'),
        'H20': (0x0a00, 0x0560, '0x079786', 'level/10_lrz/level_lrz.lemon:fn059c8c'),
        'H21': (0x1a40, 0x0670, '0x07b288', 'standalone/resources/level_objects/levelobjects_12_ssz.lemon:final teleporter; objects/level/teleporter.lemon:fn045a72'),
    }
    for key, (x, y, address, source) in camera_scenes.items():
        result[key] = dict(x=f'0x{max(x - 0x180, 0):04x}', y=f'0x{max(y - 0x20, 0):04x}', source=source, address=address, targetX=f'0x{x:04x}', targetY=f'0x{y:04x}')
    return result

if __name__ == '__main__':
    output = Path(__file__).with_name('boss_fixtures.json')
    output.write_text(json.dumps(build(), indent=2) + '\n', encoding='utf-8')
    print(f'Wrote {len(build())} source-derived boss approaches to {output.name}')
