"""Generate, compile and execute an original ARM64 fixture; compare it with Unicorn."""
import json
from pathlib import Path
import platform
import random
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))
import switchrecomp as mk8d
from fixtures import write_game
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A


def reference(nso, a, b, cell):
    base = 0x7000000000
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(base, nso.size)
    mu.mem_write(base, bytes(nso.image))
    slot, stop = base + 0x2800, base + 0xFF0
    mu.mem_write(slot, struct.pack('<Q', cell))
    for reg, value in ((A.UC_ARM64_REG_X0, a), (A.UC_ARM64_REG_X1, b),
                       (A.UC_ARM64_REG_X3, slot), (A.UC_ARM64_REG_X30, stop),
                       (A.UC_ARM64_REG_CPACR_EL1, 0x300000),
                       (A.UC_ARM64_REG_S0, 0x3FA00000), (A.UC_ARM64_REG_S1, 0x40200000)):
        mu.reg_write(reg, value)
    mu.emu_start(base, stop, count=100)
    if mu.reg_read(A.UC_ARM64_REG_PC) != stop:
        raise AssertionError('Reference did not return within instruction budget')
    return [mu.reg_read(A.UC_ARM64_REG_X0), struct.unpack('<Q', mu.mem_read(slot, 8))[0],
            mu.reg_read(A.UC_ARM64_REG_S0), mu.reg_read(A.UC_ARM64_REG_NZCV)]


def main():
    Nso, _ = mk8d.dependencies()
    (ROOT / 'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='synthetic-', dir=ROOT / 'build') as tmp:
        tmp = Path(tmp)
        game = write_game(tmp / 'input', compressed=True, extra_modules=True)
        generated = tmp / 'generated'
        subprocess.run([sys.executable, str(ROOT / 'switchrecomp.py'), 'generate', str(game), '--profile', 'mk8d', '--out', str(generated)], check=True)
        subprocess.run([sys.executable, str(ROOT / 'switchrecomp.py'), 'compile-objects', str(generated)], check=True)
        exe = tmp / ('synthetic.exe' if sys.platform == 'win32' else 'synthetic')
        objects = list((generated / 'objects' / 'main').glob('*.o'))
        compiler = mk8d.compiler_command()
        command = compiler + ['-std=c11', '-O1', '-fno-strict-aliasing', '-I', str(mk8d.VENDOR / 'runtime'),
                                            str(ROOT / 'tools' / 'smoke_runtime.c')]
        command += [str(x) for x in objects] + ['-o', str(exe)]
        command += mk8d.architecture_flags(compiler)
        if sys.platform != 'win32':
            command += ['-lm']
        subprocess.run(command, check=True)
        nso = Nso(game / 'exefs' / 'main')
        rng = random.Random(8)
        cases = [(0, 0, 0), (4, 6, 1), (5, 6, 3), ((1 << 64) - 20, 2, 9),
                 ((1 << 63) - 1, 1, 0), ((1 << 64) - 1, 1, 2)]
        cases += [(rng.getrandbits(64), rng.getrandbits(64), rng.getrandbits(64)) for _ in range(24)]
        for a, b, cell in cases:
            actual = list(map(int, subprocess.check_output([str(exe), str(a), str(b), str(cell)], text=True).split()))
            expected = reference(nso, a, b, cell)
            if actual != expected:
                raise AssertionError(f'{a}, {b}, {cell}: native {actual}, ARM64 {expected}')
        report = {
            'result': 'passed', 'synthetic_only': True, 'game_tested': False,
            'cases': len(cases), 'modules_generated_and_compiled': ['main', 'subsdk0', 'sdk'],
            'verified': ['ARM64 to C', 'native object compilation', 'native execution',
                         'conditional branches', 'signed overflow flags', 'memory loads and stores', 'scalar float addition'],
            'reference': 'Unicorn ARM64 execution', 'host': platform.platform(),
        }
        mk8d.save_json(ROOT / 'reports' / 'synthetic-smoke.json', report)
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
