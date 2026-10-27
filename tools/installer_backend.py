"""Backend for the standalone, open-source Windows setup application."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def run(arguments, **kwargs):
    kwargs.setdefault('stdout', sys.stdout)
    kwargs.setdefault('stderr', sys.stderr)
    if os.name == 'nt':
        kwargs.setdefault('creationflags', subprocess.CREATE_NO_WINDOW)
    subprocess.run(arguments, cwd=ROOT, check=True, **kwargs)


def install():
    print('Installing compiler and build dependencies from PyPI...', flush=True)
    run([sys.executable, '-m', 'pip', 'install', '--disable-pip-version-check',
         '--progress-bar', 'off', '--only-binary=:all:', '--index-url',
         'https://pypi.org/simple', '-r', str(ROOT / 'requirements-build.txt')])
    import switchrecomp as sr
    print('Checking compiler...', flush=True)
    run(sr.compiler_command() + ['--version'])
    native = ROOT / 'build/native-deps/ucrt64'
    if not all((native / name).is_file() for name in (
        'lib/libSDL3.dll.a', 'lib/libvulkan-1.dll.a', 'lib/libshaderc_shared.dll.a')):
        run([sys.executable, '-u', str(ROOT / 'tools/fetch_native_deps.py')])
    print('Tools installed. Select your own ExeFS and RomFS, then click Build game.', flush=True)


def play():
    config = json.loads((ROOT / 'kirby-local.json').read_text(encoding='utf-8'))
    exe = Path(config['executable']).resolve()
    if not exe.is_relative_to(ROOT) or not exe.is_file():
        raise ValueError('Local game build missing. Select your dumps and build first.')
    env = dict(os.environ)
    for name in list(env):
        if name.startswith(('KIRBY_TRACE', 'KIRBY_INPUT', 'KIRBY_TEST', 'KIRBY_FUNCTION_BUDGET', 'LARECOMP_')):
            env.pop(name, None)
    with (exe.parent / 'runtime.stdout.log').open('wb') as output, \
         (exe.parent / 'runtime.log').open('wb') as errors:
        subprocess.Popen([str(exe)], cwd=exe.parent, stdout=output, stderr=errors,
                         env=env, creationflags=subprocess.CREATE_NO_WINDOW)
    print('Game launched. Saves and logs are beside your local game build.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['self-test', 'install', 'build', 'play'])
    parser.add_argument('--exefs')
    parser.add_argument('--romfs')
    args = parser.parse_args()
    if args.action == 'self-test':
        import pip
        import ssl
        assert (ROOT / 'runtime/boot_probe.c').is_file()
        assert sys.version_info >= (3, 12)
        print(f'Setup payload OK: Python {sys.version.split()[0]}, pip {pip.__version__}, {ssl.OPENSSL_VERSION}', flush=True)
    elif args.action == 'install':
        install()
    elif args.action == 'build':
        if not args.exefs or not args.romfs:
            raise ValueError('Choose your own dumped ExeFS and RomFS folders.')
        import switchrecomp as sr
        _, modules = sr.locate_exefs(args.exefs)
        meta = sr.read_npdm(modules / 'main.npdm')
        if not meta or meta['program_id'] != '01003fb00c5a8000' or not meta['is_64_bit']:
            raise ValueError('Choose an ARM64 Super Kirby Clash ExeFS with main.npdm.')
        for name in ('gfx', 'map', 'mint', 'msg', 'snd'):
            if not (Path(args.romfs) / name).is_dir():
                raise ValueError(f'RomFS is missing the {name}/ folder.')
        install()
        run([sys.executable, '-u', str(ROOT / 'tools/setup_kirby.py'),
             '--exefs', args.exefs, '--romfs', args.romfs])
        (ROOT / 'kirby-setup-folders.txt').write_text(args.exefs + '\n' + args.romfs + '\n', encoding='utf-16')
        print('Build complete. Click Play game.', flush=True)
    else:
        play()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'Setup failed: {error}', file=sys.stderr, flush=True)
        raise SystemExit(1)
