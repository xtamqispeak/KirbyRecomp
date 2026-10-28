"""Inject original host timing calls into a specifically identified local build.

No game instructions or generated game source are distributed by this module.
Unknown versions retain their original timing.
"""
from pathlib import Path
import hashlib

BUILD = 'c9649e053bd7be2751ef0b00e7c03f5d59695b94'
SITES = (0x70000172b0, 0x7000017260, 0x70000173d0, 0x7000017460,
         0x7000017530, 0x7000017560, 0x700001ab90, 0x700001af90)


def apply(folder, manifest):
    main = next(m for m in manifest['modules'] if m['name'] == 'main')
    supported = (main['build_id'].lower().rstrip('0') == BUILD and
                 int(main['base'], 16) == 0x7000010000)
    if not supported:
        return False
    found = {site: 0 for site in SITES}
    pending = []
    for path in sorted((Path(folder) / 'main').glob('*.c')):
        relative = path.relative_to(folder).as_posix()
        before = path.read_bytes()
        if hashlib.sha256(before).hexdigest() != manifest['generated_sources'][relative]:
            raise ValueError(f'Generated source changed before hook validation: {relative}')
        text = before.decode()
        changed = text
        for site in SITES:
            marker = f'    FN_ENTER(0x{site:x}ull);'
            found[site] += changed.count(marker)
            hook = f'\n    {{ extern int kirby_frame_hook(Cpu *, uint64_t); if (kirby_frame_hook(c, 0x{site:x}ull)) return; }}'
            if marker in changed and marker + hook not in changed:
                changed = changed.replace(marker, marker + hook)
        if changed != text:
            pending.append((path, relative, changed.encode()))
    if any(count != 1 for count in found.values()):
        raise ValueError('Known timing build is missing unique function entry sites; regenerate the local build.')
    for path, relative, content in pending:
        path.write_bytes(content)
        manifest['generated_sources'][relative] = hashlib.sha256(content).hexdigest()
    return True
