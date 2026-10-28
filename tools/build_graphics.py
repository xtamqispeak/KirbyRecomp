"""Compile the reusable LASRecomp NVN/Vulkan backend against the Kirby host bridge."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import switchrecomp as sr

SOURCES = [
    'objects.c','cmdbuf.c','tegra.c','formats.c','astc.c','rqueue.c','slowframe.c',
    'output_scale.c','shader_prep.c','shader_seen.c','smaa_data.c','maxwell/translate.c',
    'vulkan/device.c','vulkan/exec.c','vulkan/textures.c','vulkan/shaders.c','vulkan/record.c','vulkan/vma.cpp',
]


def build(generated):
    native = ROOT / 'build/native-deps/ucrt64'
    out = ROOT / 'build/graphics'
    out.mkdir(parents=True, exist_ok=True)
    vendor = sr.VENDOR / 'runtime'
    headers=sorted(vendor.rglob('*.h'))+sorted((ROOT/'runtime').glob('*.h'))+[generated/'probe/probe_modules.h']
    header_digest = hashlib.sha256(''.join(sr.sha256(p) for p in headers).encode()).hexdigest()
    sources = [vendor / 'nvn' / name for name in SOURCES] + [ROOT / 'runtime/graphics_bridge.c', ROOT / 'runtime/input_runtime.c', ROOT / 'runtime/kirby_settings.c']
    def compile_one(source):
        obj = out / (source.stem + '.o')
        compiler = sr.compiler_command()
        cpp = source.suffix == '.cpp'
        if cpp and 'ziglang' in compiler:
            compiler[-1] = 'c++'
        args = compiler + ['-std=c++17' if cpp else '-std=gnu11','-O1','-fno-strict-aliasing',
             '-D_FILE_OFFSET_BITS=64','-D_CRT_RAND_S','-D_GNU_SOURCE','-D_USE_MATH_DEFINES',
             '-I', str(vendor), '-I', str(ROOT / 'runtime'), '-I', str(generated / 'probe'),
             '-I', str(sr.VENDOR / 'third_party/smaa/Textures'), '-I', str(out),
             '-idirafter', str(native / 'include'), '-c', str(source), '-o', str(obj)] + sr.architecture_flags(compiler)
        digest = hashlib.sha256(json.dumps([args,sr.sha256(source),header_digest]).encode()).hexdigest()
        stamp = obj.with_suffix('.build-id')
        if obj.exists() and stamp.exists() and stamp.read_text() == digest:
            return obj
        result = subprocess.run(args, capture_output=True, text=True, **sr.host_process_options())
        obj.with_suffix('.log').write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(f'{source.name}:\n{result.stderr[:12000]}')
        stamp.write_text(digest)
        print('Built graphics:', source.name, flush=True)
        return obj
    (out / 'shader_variants.inc').write_text('""\n')
    shader = (sr.VENDOR / 'third_party/smaa/SMAA.hlsl').read_bytes().decode('latin1').encode('ascii',errors='replace').decode()
    (out / 'smaa_hlsl.inc').write_text('\n'.join(json.dumps(line + '\n') for line in shader.splitlines()))
    failures, objects = [], []
    with ThreadPoolExecutor(max_workers=4) as workers:
        for future in as_completed([workers.submit(compile_one, source) for source in sources]):
            try:
                objects.append(future.result())
            except Exception as e:
                failures.append(str(e))
    if failures:
        raise RuntimeError('\n'.join(failures))
    return objects


if __name__ == '__main__':
    print(build(Path(sys.argv[1]).resolve()))
