"""Build and run native kernel/IPC/save integration tests using original test data."""
from pathlib import Path
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import switchrecomp as sr

def main():
    (ROOT/'build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='runtime-tests-',dir=ROOT/'build') as directory:
        folder=Path(directory).resolve()
        assert folder.is_relative_to((ROOT/'build').resolve())
        exe=folder/'runtime-tests.exe'
        compiler=sr.compiler_command()
        subprocess.run(compiler+['-std=gnu11','-O1','-fno-strict-aliasing','-I',str(sr.VENDOR/'runtime'),'-I',str(ROOT/'runtime'),
            str(ROOT/'tests/runtime_native.c'),str(ROOT/'runtime/kernel_runtime.c'),str(ROOT/'runtime/filesystem_runtime.c'),
            '-o',str(exe)]+sr.architecture_flags(compiler),check=True)
        run=subprocess.run([str(exe)],cwd=folder,capture_output=True,text=True,timeout=25)
        print(run.stdout)
        if run.returncode:
            print(run.stderr);raise SystemExit(run.returncode)
        sr.save_json(ROOT/'reports/native-runtime-tests.json',{'result':'passed','checks':[
            '80000 mutex increments across 4 native threads','event duplicate signal/close/timeout',
            'CMIF service creation/domain/clone lifetime/malformed descriptors','durable save reopen and path confinement']})

if __name__=='__main__':main()
