"""Build and test the experimental Windows runtime from locally generated game objects.

--graphics includes native Vulkan rendering, SDL input and the audio mixer.
Unsupported required services stop explicitly and are recorded in the report.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import switchrecomp as sr


def prepare(out):
    Nso, _ = sr.dependencies()
    manifest = json.loads((out / 'manifest.json').read_text())
    if manifest['architecture'] != 'ARM64':
        raise ValueError('Probe requires ARM64 generated objects')
    modules = []
    for module in manifest['modules']:
        path = Path(module['path'])
        if sr.sha256(path) != module['sha256']:
            raise ValueError(f'Input changed since generation: {path}')
        modules.append((module, Nso(path)))
    exports = {}
    for module, nso in modules:
        for name, info, shndx, value, _ in nso.exports():
            address = value if shndx == 0xFFF1 else int(module['base'], 16) + value
            if name not in exports or info >> 4 != 2:
                exports[name] = address
    start = exports.get('_ZN2nn4init5StartEmmPFvvES2_')
    if not start:
        raise ValueError('SDK does not export the expected nn::init::Start ABI')
    target = out / 'probe'
    target.mkdir(exist_ok=True)
    declarations, records, relocation_counts = [], [], {}
    for module, nso in modules:
        name, base = module['name'], int(module['base'], 16)
        for offset, kind, index, addend in nso.relocs:
            if kind == 1027:
                value = base + addend
            elif kind in (257, 1025, 1026):
                symbol, info, shndx, address, _ = nso.syms[index]
                resolved = (address if shndx == 0xFFF1 else base + address) if shndx else exports.get(symbol)
                if resolved is None and info >> 4 != 2:
                    raise ValueError(f'{name}: unresolved import {symbol}')
                value = (resolved + addend) if resolved else 0
            else:
                raise ValueError(f'{name}: unsupported relocation {kind}')
            struct.pack_into('<Q', nso.image, offset, value & ((1 << 64) - 1))
        relocation_counts[name] = len(nso.relocs)
        (target / f'{name}.image').write_bytes(nso.image)
        declarations.append(f'extern const uint64_t {name}_fn_addrs[], {name}_fn_count;\nextern const GuestFn {name}_fn_ptrs[];')
        init = base + nso.dyn[12] if nso.dyn.get(12) else 0
        init_array = base + nso.dyn[25] if nso.dyn.get(25) else 0
        records.append(f'{{"{name}", "{name}.image", {base}ull, {nso.size}ull, {init}ull, {init_array}ull, {nso.dyn.get(27, 0)}ull, {name}_fn_addrs, &{name}_fn_count, {name}_fn_ptrs}}')
    header = '\n'.join(declarations)
    header += '\ntypedef struct { const char *name, *image; uint64_t base, size, init, init_array, init_array_size; const uint64_t *addresses, *count; const GuestFn *functions; } ProbeModule;\n'
    header += 'static const ProbeModule probe_modules[] = {\n' + ',\n'.join(records) + '\n};\n'
    header += f'#define PROBE_MODULE_COUNT {len(records)}\n#define PROBE_START {start}ull\n#define PROBE_TITLE_ID 0x{manifest["expected_title_id"]}ull\n'
    source_root = Path(next(m['path'] for m in manifest['modules'] if m['name'] == 'main')).parent
    romfs_root = source_root.parent / 'romfs' if source_root.name.lower() == 'exefs' else source_root
    if manifest.get('romfs_path'):
        romfs_root=Path(manifest['romfs_path']).resolve()
    header += '#define PROBE_ROMFS_ROOT ' + json.dumps(romfs_root.as_posix()) + '\n'
    header += '#define PROBE_NVN_BOOTSTRAP ' + str(exports.get('nvnBootstrapLoader', 0)) + 'ull\n'
    header += '#define PROBE_NV_INIT ' + str(exports.get('_ZN2nv18InitializeGraphicsEPvm', 0)) + 'ull\n'
    header += 'static const struct { const char *name; uint64_t address; } probe_exports[] = {\n'
    header += ',\n'.join('{' + json.dumps(name) + ',' + str(value) + 'ull}' for name, value in sorted(exports.items()))
    header += '\n};\n#define PROBE_EXPORT_COUNT ' + str(len(exports)) + '\n'
    (target / 'probe_modules.h').write_text(header)
    return manifest, target, relocation_counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('generated')
    parser.add_argument('--timeout', type=int, default=20)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--graphics', action='store_true', help='Use the ported NVN/Vulkan backend and open its game window')
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('The development probe currently uses Windows VirtualAlloc')
    out = Path(args.generated).resolve()
    manifest, target, relocations = prepare(out)
    # Check source identity and complete or resume object compilation.
    sr.compile_objects(argparse.Namespace(generated=str(out), cc=None, jobs=4))
    objects = [out / 'objects' / Path(p).with_suffix('.o') for p in manifest['generated_sources']]
    compiler = sr.compiler_command()
    if args.graphics:
        import build_graphics
        objects += build_graphics.build(out)
        if 'ziglang' in compiler:
            compiler[-1] = 'c++'
    exe = target / ('KirbyRecomp.exe' if args.graphics else 'KirbyBootProbe.exe')
    response = target / 'objects.rsp'
    response.write_text('\n'.join('"' + str(p).replace('\\', '/') + '"' for p in objects))
    sources = [str(ROOT / 'runtime' / name) for name in ('boot_probe.c', 'ipc_runtime.c', 'kernel_runtime.c', 'romfs_runtime.c', 'filesystem_runtime.c')]
    sources.append(str(sr.VENDOR / 'runtime' / 'romfs_build.c'))
    native = ROOT / 'build' / 'native-deps' / 'ucrt64'
    sources.extend([str(sr.VENDOR / 'services' / 'audio.c'), str(ROOT / 'runtime' / 'audio_bridge.c')])
    command = compiler + ['-std=c11', '-O1', '-fno-strict-aliasing', '-I', str(sr.VENDOR / 'runtime'),
                          '-I', str(target), '-I', str(ROOT / 'runtime'), '-idirafter', str(native / 'include')] + sources + ['@' + str(response),
                          str(native / 'lib' / 'libSDL3.dll.a'), str(native / 'lib' / 'libwinpthread.dll.a'),
                          '-Wl,--stack,16777216', '-o', str(exe)] + sr.architecture_flags(compiler)
    if args.graphics:
        # Driver name selects C++ linking, but the host sources remain C.
        command[command.index('-std=c11')] = '-std=gnu11'
        command += ['-DPORT_GRAPHICS', '-D_FILE_OFFSET_BITS=64', '-D_CRT_RAND_S',
                    str(native / 'lib/libvulkan-1.dll.a'), str(native / 'lib/libshaderc_shared.dll.a')]
    print('Linking development boot probe', flush=True)
    subprocess.run(command, check=True, stdout=sys.stdout, stderr=sys.stderr, **sr.host_process_options())
    for library in (native / 'bin').glob('*.dll'):
        shutil.copy2(library, target / library.name)
    if args.build_only:
        print(exe)
        return
    try:
        environment = dict(os.environ)
        environment['PATH'] = str(native / 'bin') + os.pathsep + environment.get('PATH', '')
        result = subprocess.run([str(exe)], cwd=target, env=environment, capture_output=True, text=True, timeout=args.timeout)
        output, code = result.stdout + result.stderr, result.returncode
    except subprocess.TimeoutExpired as e:
        output = (e.stderr or b'').decode(errors='replace') if isinstance(e.stderr, bytes) else e.stderr or ''
        output += '\n[stop] watchdog_timeout\n'
        code = None
    (target / 'boot.log').write_text(output, encoding='utf-8')
    stops = re.findall(r'\[stop\] ([^\n]+)', output)
    completed = re.findall(r'\[capture-complete\] frames=(\d+) functions=(\d+) svc_calls=(\d+)', output)
    frames = int(completed[-1][0]) if completed else max([int(n) for n in re.findall(r'\[vk\].*?frame (\d+)', output)] or [0])
    counts = re.findall(r'(?:\[stop\]|\[progress\])[^\n]*functions=(\d+) svc_calls=(\d+)', output)
    if completed:
        counts.append(completed[-1][1:])
    report = {
        'game': manifest['game'], 'architecture': manifest['architecture'],
        'playable': False, 'graphics_implemented': args.graphics, 'audio_implemented': True,
        'frames_presented': frames, 'capture_completed': bool(completed),
        'shader_errors': sorted(set(re.findall(r'\[shader\] [^\n]*failed[^\n]*', output))),
        'exit_code': code, 'stop': stops[-1] if stops else 'capture_complete' if completed else 'process_exited',
        'functions_entered': int(counts[-1][0]) if counts else None,
        'svc_calls': int(counts[-1][1]) if counts else None,
        'services_requested': re.findall(r'\[service\] ([^\n]+)', output),
        'relocations_applied': relocations,
        'svc_trace': re.findall(r'\[svc\] ([^\n]+)', output),
        'executable': str(exe), 'log': str(target / 'boot.log'),
    }
    sr.save_json(ROOT / 'reports' / 'kirby-boot-probe.json', report)
    print(output[-10000:])
    print('Report:', ROOT / 'reports' / 'kirby-boot-probe.json')


if __name__ == '__main__':
    main()
