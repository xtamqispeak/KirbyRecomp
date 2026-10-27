#!/usr/bin/env python3
"""Switch static-recompilation development tools. This is not a game launcher."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = Path(__file__).resolve().parent
VENDOR = ROOT / 'vendor' / 'lasrecomp'
UPSTREAM = 'a4b53a3fc5e91bf101367063bafe45017dc23335'
TITLE_ID = '0100152000022000'
PROFILES = {
    'mk8d': {'title_id': TITLE_ID, 'name': 'Mario Kart 8 Deluxe'},
    'kirby': {'title_id': '01003fb00c5a8000', 'name': 'Super Kirby Clash'},
}
MODULE_NAMES = ['rtld', 'main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']


def save_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')


def sha256(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def dependencies():
    sys.path.insert(0, str(VENDOR / 'tools'))
    try:
        from nso import Nso
        from gen import Module
    except ImportError as e:
        raise ValueError('Missing generator dependency; install requirements.txt with this Python interpreter') from e
    return Nso, Module


def probe_package(path):
    """Read only the public PFS0 directory; never read tickets or decrypt NCAs."""
    path = Path(path).resolve()
    total = path.stat().st_size
    with path.open('rb') as f:
        header = f.read(16)
        if len(header) != 16 or header[:4] != b'PFS0':
            raise ValueError('Expected an NSP/PFS0 container')
        count, string_size = struct.unpack_from('<II', header, 4)
        if count > 100_000 or string_size > 16 * 1024 * 1024:
            raise ValueError('Unreasonable PFS0 directory size')
        data_offset = 16 + count * 24 + string_size
        if data_offset > total:
            raise ValueError('Truncated PFS0 directory')
        entries, strings = f.read(count * 24), f.read(string_size)
        files, names, ranges = [], set(), []
        for i in range(count):
            offset, size, name_off = struct.unpack_from('<QQI', entries, i * 24)
            end = strings.find(b'\0', name_off)
            if name_off >= len(strings) or end < 0:
                raise ValueError('Invalid PFS0 filename')
            name = strings[name_off:end].decode('utf-8')
            if not name or name in names or '/' in name or '\\' in name or name in ('.', '..'):
                raise ValueError('Unsafe or duplicate PFS0 filename')
            names.add(name)
            if data_offset + offset + size > total:
                raise ValueError(f'PFS0 entry {name} extends outside package')
            if size:
                ranges.append((offset, offset + size))
            files.append({'name': name, 'size': size, 'offset': data_offset + offset})
        ranges.sort()
        if any(a[1] > b[0] for a, b in zip(ranges, ranges[1:])):
            raise ValueError('Overlapping PFS0 entries')
    return {
        'file': path.name, 'size': total, 'format': 'PFS0', 'files': files,
        'nca_count': sum(x['name'].endswith('.nca') for x in files),
        'contains_direct_exefs_main': 'main' in names,
        'ready_for_recompilation': False,
        'reason': 'Container inventory only. Supply extracted exefs/main and romfs; no NCA payload was decoded.',
        'title_id_verified': False,
    }


def read_npdm(path):
    if not path.is_file():
        return None
    if path.stat().st_size > 1024 * 1024:
        raise ValueError('NPDM exceeds 1 MiB limit')
    data = path.read_bytes()
    if len(data) < 0x80 or data[:4] != b'META':
        raise ValueError('Invalid main.npdm META header')
    aci, size = struct.unpack_from('<II', data, 0x70)
    if aci < 0x80 or size < 0x40 or aci + size > len(data) or data[aci:aci + 4] != b'ACI0':
        raise ValueError('Invalid NPDM ACI0 range')
    return {
        'program_id': f'{struct.unpack_from("<Q", data, aci + 0x10)[0]:016x}',
        'is_64_bit': bool(data[0xC] & 1),
        'priority': data[0xE], 'stack_size': struct.unpack_from('<I', data, 0x1C)[0],
        'name': data[0x20:0x30].split(b'\0')[0].decode('utf-8', 'replace'),
    }


def locate_exefs(path):
    root = Path(path).resolve()
    if root.is_file():
        raise ValueError('Expected an extracted game folder, not a package file; use probe for NSP inventory')
    exefs = root / 'exefs' if (root / 'exefs').is_dir() else root
    if not (exefs / 'main').is_file():
        raise ValueError(f'No extracted NSO at {exefs / "main"}')
    return root, exefs


def analyze_game(path, reference=None, profile='mk8d'):
    Nso, Module = dependencies()
    root, exefs = locate_exefs(path)
    npdm = read_npdm(exefs / 'main.npdm')
    target = PROFILES[profile]
    if npdm and npdm['program_id'] != target['title_id']:
        raise ValueError(f'Wrong title: expected {target["title_id"]}, got {npdm["program_id"]}')
    bits = 64 if npdm is None or npdm['is_64_bit'] else 32
    hle_candidates = set()
    if reference:
        ref = Path(reference).resolve()
        if not (ref / 'runtime' / 'hle').is_dir():
            raise ValueError('Reference folder has no runtime/hle directory')
        for source in (ref / 'runtime').rglob('*.c'):
            hle_candidates.update(re.findall(r'HLE_FUNC\(\s*"([^"]+)"', source.read_text(encoding='utf-8')))
    modules, base = [], 0x7000000000 if bits == 64 else 0x10000000
    unexpected = []
    for item in exefs.iterdir():
        if item.is_file() and item.name not in MODULE_NAMES:
            with item.open('rb') as f:
                if f.read(4) == b'NSO0':
                    unexpected.append(item.name)
    if unexpected:
        raise ValueError(f'Unexpected NSO module names: {unexpected}; extend the module order before generating')
    for name in MODULE_NAMES:
        source = exefs / name
        if not source.is_file():
            continue
        n = Nso(source, bits)
        unsupported, traps, svc, kinds = [], [], Counter(), Counter()
        print(f'Inspecting {name}: ARM{bits}, {n.text[1]:,} text bytes', file=sys.stderr, flush=True)
        if bits == 64:
            from lifter import Unsupported
            m = Module(name, source, base, 'c')
            for address in range(m.lo, m.hi, 4):
                word = m.word(address)
                try:
                    insn = m.lifter.lift(word, address)
                except Unsupported:
                    unsupported.append({'offset': hex(address - base), 'word': f'{word:08x}'})
                    kinds['unsupported'] += 1
                    continue
                kinds[insn.kind] += 1
                if 'rt_udf(' in insn.code:
                    record = {'offset': hex(address - base), 'word': f'{m.word(address):08x}'}
                    traps.append(record)
                    if '/*' in insn.code:
                        unsupported.append(record)
                if (word & 0xFFE0001F) == 0xD4000001:
                    svc[f'0x{(word >> 5) & 0xFFFF:x}'] += 1
        else:
            import capstone
            md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
            md.skipdata = True
            text = bytes(n.image[n.text[0]:n.text[0] + n.text[1]])
            for _, _, mnemonic, _ in md.disasm_lite(text, base + n.text[0]):
                kinds[mnemonic] += 1
            for offset in range(0, len(text), 4):
                word = struct.unpack_from('<I', text, offset)[0]
                if (word & 0x0F000000) == 0x0F000000 and word >> 28 != 15:
                    svc[hex(word & 0xFFFFFF)] += 1
        modules.append({
            'name': name, 'path': str(source), 'architecture': f'ARM{bits}', 'base': hex(base), 'image_size': n.size,
            'build_id': n.build_id, 'sha256': sha256(source),
            'segments': {k: {'offset': getattr(n, k)[0], 'size': getattr(n, k)[1]} for k in ('text', 'rodata', 'data')},
            'bss_size': n.bss_size, 'needed': n.needed,
            'imports': [{'name': s[0], 'weak': s[1] >> 4 == 2} for s in n.imports()],
            'exports': [s[0] for s in n.exports()],
            'relocations': dict(Counter(str(r[1]) for r in n.relocs)),
            'text_scan': {'kinds': dict(kinds), 'unsupported_count': len(unsupported) if bits == 64 else None,
                          'unsupported_samples': unsupported[:100], 'trap_count': len(traps),
                          'svc_counts': dict(svc), 'note': 'Linear scan includes embedded data; reachability and ARM/Thumb transitions are unknown.'},
        })
        base = (base + n.size + 0xFFFF) & ~0xFFFF
    exports = {s for m in modules for s in m['exports']}
    for module in modules:
        for imp in module['imports']:
            name = imp['name']
            imp['resolution'] = 'guest_export' if name in exports else 'weak_zero' if imp['weak'] else 'unresolved'
            imp['upstream_hle_candidate'] = name in hle_candidates
    unresolved = sorted({i['name'] for m in modules for i in m['imports'] if i['resolution'] == 'unresolved'})
    romfs = exefs.parent / 'romfs' if exefs.name.lower() == 'exefs' else root / 'romfs'
    asset_names = ('Course', 'Driver', 'Kart', 'UI') if profile == 'mk8d' else ('gfx', 'map', 'mint', 'snd')
    flat_romfs = all((root / name).is_dir() for name in asset_names)
    return {
        'schema_version': 1, 'project': 'SwitchRecomp', 'profile': profile,
        'game': target['name'], 'expected_title_id': target['title_id'],
        'npdm': npdm, 'architecture': f'ARM{bits}', 'title_id_verified': bool(npdm),
        'romfs_present': romfs.is_dir() or flat_romfs,
        'romfs_layout': 'directory' if romfs.is_dir() else 'flat' if flat_romfs else 'missing',
        'upstream_commit': UPSTREAM,
        'layout': 'Development address layout, allocated sequentially at 64 KiB boundaries; not captured Switch addresses.',
        'modules': modules, 'unresolved_strong_imports': unresolved,
        'hle_note': 'Upstream matches are candidates only; no host implementations are linked in this project.',
        'playable': False, 'runtime_implemented': False,
    }


def generate_game(args):
    report = analyze_game(args.game, args.reference, getattr(args, 'profile', 'mk8d'))
    if not report['title_id_verified']:
        raise ValueError('Generation requires main.npdm identifying the selected game; analysis alone can run without it')
    if report['architecture'] != 'ARM64':
        raise ValueError('This game build is ARM32. LASRecomp only provides an ARM64 lifter; full-game generation requires an ARM32 backend. See docs/PORTING.md.')
    out = Path(args.out).resolve()
    game, exefs = locate_exefs(args.game)
    if out == game or out.is_relative_to(exefs) or game.is_relative_to(out):
        raise ValueError('Output must be separate from input ExeFS and must not contain the input game')
    if out.exists():
        raise ValueError('Output already exists; choose a fresh output folder to avoid stale or overwritten files')
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.switchrecomp-', dir=out.parent) as temp:
        staging = Path(temp) / 'result'
        staging.mkdir()
        translations = []
        for module in report['modules']:
            source = Path(module['path'])
            if sha256(source) != module['sha256']:
                raise ValueError(f'{module["name"]}: source changed during analysis')
            dest = staging / module['name']
            command = [sys.executable, str(VENDOR / 'tools' / 'gen.py'), module['name'], str(source),
                       module['base'], str(dest), '--simd', 'c', '--chunk', str(args.chunk)]
            subprocess.run(command, check=True)
            translation = json.loads((dest / 'translation.json').read_text())
            translations.append(translation)
        report['translations'] = translations
        report['generated_sources'] = {str(p.relative_to(staging).as_posix()): sha256(p) for p in sorted(staging.rglob('*.c'))}
        report['runtime_headers'] = {p.name: sha256(p) for p in sorted((VENDOR / 'runtime').glob('*.h'))}
        save_json(staging / 'manifest.json', report)
        os.replace(staging, out)
    print(f'Generated C in {out}; a game runtime is still required.')


def compiler_command(cc=None):
    if cc:
        path = shutil.which(cc)
        if not path:
            raise ValueError(f'Compiler not found: {cc}')
        return [path]
    if importlib.util.find_spec('ziglang'):
        return [sys.executable, '-m', 'ziglang', 'cc']
    if shutil.which('clang'):
        return [shutil.which('clang')]
    raise ValueError('Install requirements-build.txt or pass --cc with the path to Clang')


def architecture_flags(compiler):
    if platform.machine().lower() in ('amd64', 'x86_64'):
        return ['-mcpu=x86_64_v3'] if 'ziglang' in compiler else ['-march=x86-64-v3']
    return []


def compile_objects(args):
    out = Path(args.generated).resolve()
    manifest = json.loads((out / 'manifest.json').read_text(encoding='utf-8'))
    for name, digest in manifest['runtime_headers'].items():
        if sha256(VENDOR / 'runtime' / name) != digest:
            raise ValueError('Runtime headers changed; regenerate C before compiling')
    compiler = compiler_command(args.cc)
    compiler_version = subprocess.check_output(compiler + ['--version'], text=True)
    tasks = []
    for relative, digest in manifest['generated_sources'].items():
        source = (out / relative).resolve()
        if not source.is_relative_to(out) or source.suffix != '.c' or sha256(source) != digest:
            raise ValueError(f'Modified or invalid generated source: {relative}')
        obj = out / 'objects' / Path(relative).with_suffix('.o')
        obj.parent.mkdir(parents=True, exist_ok=True)
        command = compiler + ['-std=c11', '-O1', '-fno-strict-aliasing', '-Wno-unused-label',
                              '-I', str(VENDOR / 'runtime'), '-c', str(source), '-o', str(obj)]
        command += architecture_flags(compiler)
        identity = hashlib.sha256(json.dumps([digest, manifest['runtime_headers'], command, compiler_version]).encode()).hexdigest()
        stamp = obj.with_suffix('.build-id')
        if obj.is_file() and stamp.is_file() and stamp.read_text() == identity:
            continue
        tasks.append((relative, obj, command, stamp, identity))
    def compile_one(task):
        relative, obj, command, stamp, identity = task
        result = subprocess.run(command, capture_output=True, text=True)
        log = obj.with_suffix('.log')
        log.write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise ValueError(f'{relative} failed to compile; see {log}\n{result.stderr[:3000]}')
        stamp.write_text(identity)
        return relative
    jobs = getattr(args, 'jobs', 4)
    print(f'Compiling {len(tasks)} changed sources with {jobs} workers', flush=True)
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(compile_one, task) for task in tasks]
        for i, future in enumerate(as_completed(futures), 1):
            relative = future.result()
            print(f'[{i}/{len(tasks)}] {relative}', flush=True)
    print('Object compilation complete. These objects are not a playable executable.')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    probe = sub.add_parser('probe', help='Inventory the public NSP directory, without decrypting payloads')
    probe.add_argument('package')
    probe.add_argument('--out', default='reports/package.json')
    analyze = sub.add_parser('analyze', help='Inspect extracted NSOs, imports, instructions and metadata')
    analyze.add_argument('game')
    analyze.add_argument('--out', default='reports/analysis.json')
    analyze.add_argument('--reference', help='Optional LASRecomp checkout for HLE candidate matching')
    analyze.add_argument('--profile', choices=PROFILES, default='kirby')
    gen = sub.add_parser('generate', help='Translate extracted ARM64 NSO modules to C')
    gen.add_argument('game')
    gen.add_argument('--out', default='generated')
    gen.add_argument('--reference')
    gen.add_argument('--profile', choices=PROFILES, default='kirby')
    gen.add_argument('--chunk', type=int, default=1500)
    compile_parser = sub.add_parser('compile-objects', help='Compile generated C; no game runtime is linked')
    compile_parser.add_argument('generated')
    compile_parser.add_argument('--cc', help='Clang executable (default: Python ziglang package, then clang on PATH)')
    compile_parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args(argv)
    try:
        if args.command == 'probe':
            report = probe_package(args.package)
            save_json(args.out, report)
            print(f'{len(report["files"])} container entries; {report["nca_count"]} NCAs. Report: {Path(args.out).resolve()}')
            print(report['reason'])
        elif args.command == 'analyze':
            report = analyze_game(args.game, args.reference, args.profile)
            save_json(args.out, report)
            print(f'{len(report["modules"])} NSO modules, {len(report["unresolved_strong_imports"])} unresolved strong imports.')
            print(f'Report: {Path(args.out).resolve()}. Runtime missing; not playable.')
        elif args.command == 'generate':
            if args.chunk < 1:
                raise ValueError('--chunk must be positive')
            generate_game(args)
        else:
            if args.jobs < 1:
                raise ValueError('--jobs must be positive')
            compile_objects(args)
    except (ValueError, OSError, subprocess.CalledProcessError, KeyError) as e:
        print(f'Error: {e}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
