"""Verify the 3D color-LUT layout with original synthetic texture data."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import switchrecomp as sr


def main():
    (ROOT / 'build').mkdir(exist_ok=True)
    exe = ROOT / 'build/texture-tests.exe'
    compiler = sr.compiler_command()
    subprocess.run(compiler + ['-std=c11', '-O1', '-I', str(sr.VENDOR / 'runtime'),
        str(ROOT / 'tests/tegra_native.c'), str(sr.VENDOR / 'runtime/nvn/tegra.c'),
        '-o', str(exe)] + sr.architecture_flags(compiler), check=True)
    subprocess.run([str(exe)], check=True)
    sr.save_json(ROOT / 'reports/texture-tests.json', {'result': 'passed', 'synthetic_only': True,
        'checks': ['16x16x16 coordinate LUT from explicit hardware address bits',
                   'non-power-of-two depth and tile-depth cap', '2D layout preserved']})


if __name__ == '__main__':
    main()
