"""Build a local Kirby port from the player's own extracted ExeFS and RomFS."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import switchrecomp as sr

def folder(value,prompt):
    text=value if value else input(prompt).strip().strip('"')
    path=Path(text).expanduser().resolve()
    if not path.is_dir():raise ValueError(f'Directory not found: {path}')
    return path

def validate(exefs,romfs):
    _,module_folder=sr.locate_exefs(str(exefs))
    report=sr.analyze_game(str(exefs),None,'kirby')
    if not report['title_id_verified'] or report['architecture']!='ARM64':
        raise ValueError('An ARM64 Super Kirby Clash ExeFS with main.npdm is required.')
    if report['npdm']['program_id'].lower()!='01003fb00c5a8000':
        raise ValueError('These files are not Super Kirby Clash.')
    for name in ('gfx','map','mint','msg','snd'):
        if not (romfs/name).is_dir():raise ValueError(f'RomFS is missing the {name}/ directory: {romfs}')
    if romfs==ROOT or romfs.is_relative_to(ROOT/'vendor'):
        raise ValueError('Choose the extracted game asset directory.')
    return report,module_folder

def run(*arguments):
    subprocess.run([sys.executable,*map(str,arguments)],cwd=ROOT,check=True,
                   stdout=sys.stdout,stderr=sys.stderr,**sr.host_process_options())

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs',help='Dump folder containing main, main.npdm, sdk and rtld')
    parser.add_argument('--romfs',help='Dump folder containing gfx, map, mint, msg and snd')
    parser.add_argument('--check-only',action='store_true',help='Validate the selected dumps without generating or compiling')
    parser.add_argument('--output',default='generated-kirby',help='Local generated build directory (default: generated-kirby)')
    args=parser.parse_args()
    if sys.platform!='win32' or platform.machine().lower() not in ('amd64','x86_64'):
        parser.error('This release targets 64-bit Windows on an x86-64 CPU.')
    print('KirbyRecomp - local build from your own dumps. No game files are supplied.\n',flush=True)
    exefs=folder(args.exefs,'ExeFS folder: ')
    romfs=folder(args.romfs,'RomFS folder: ')
    report,module_folder=validate(exefs,romfs)
    print(f'Validated {report["game"]}, {len(report["modules"])} ARM64 modules.',flush=True)
    if args.check_only:return
    base=(ROOT/args.output).resolve()
    if not base.is_relative_to(ROOT) or base==ROOT:
        raise ValueError('The generated build directory must be inside this project.')
    generated=base
    number=1
    while generated.exists():
        manifest_path=generated/'manifest.json'
        if manifest_path.is_file():
            manifest=json.loads(manifest_path.read_text())
            if manifest.get('expected_title_id','').lower()=='01003fb00c5a8000' and all(
                Path(m['path']).resolve()==(module_folder/m['name']).resolve() and Path(m['path']).is_file()
                and sr.sha256(Path(m['path']))==m['sha256'] for m in manifest['modules']):break
        number+=1;generated=base.with_name(f'{base.name}-{number}')
    if not generated.exists():
        run(ROOT/'switchrecomp.py','generate',exefs,'--profile','kirby','--out',generated)
    manifest_path=generated/'manifest.json'
    manifest=json.loads(manifest_path.read_text());manifest['romfs_path']=str(romfs)
    sr.save_json(manifest_path,manifest)
    native=ROOT/'build/native-deps/ucrt64'
    if not all((native/name).is_file() for name in ('lib/libSDL3.dll.a','lib/libvulkan-1.dll.a','lib/libshaderc_shared.dll.a')):
        run(ROOT/'tools/fetch_native_deps.py')
    run(ROOT/'tools/boot_probe.py',generated,'--graphics','--build-only')
    exe=generated/'probe/KirbyRecomp.exe'
    sr.save_json(ROOT/'kirby-local.json',{'exefs':str(module_folder),'romfs':str(romfs),'generated':str(generated),'executable':str(exe)})
    (ROOT/'kirby-launcher.path').write_text(str(exe)+'\n')
    print('\nBuild complete. Double-click Play Kirby.cmd. Keep your RomFS folder available.')

if __name__=='__main__':
    try:main()
    except (ValueError,OSError,subprocess.CalledProcessError) as error:
        print(f'\nSetup failed: {error}',file=sys.stderr);raise SystemExit(1)
