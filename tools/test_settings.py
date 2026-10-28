"""Exercise the actual Win32 settings parser and the original fixed-step bridge."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import switchrecomp as sr

def main():
    native=ROOT/'build/native-deps/ucrt64'
    with tempfile.TemporaryDirectory(prefix='settings-check-',dir=ROOT/'build') as directory:
        folder=Path(directory);exe=folder/'settings-tests.exe';cc=sr.compiler_command()
        subprocess.run(cc+['-std=gnu11','-O1','-I',str(ROOT/'tests/settings'),'-I',str(ROOT/'runtime'),
            '-I',str(sr.VENDOR/'runtime'),'-idirafter',str(native/'include'),str(ROOT/'tests/settings_native.c'),
            str(ROOT/'runtime/kirby_settings.c'),str(native/'lib/libSDL3.dll.a'),'-lgdi32','-luser32',
            '-o',str(exe)]+sr.architecture_flags(cc),check=True)
        env={k:v for k,v in os.environ.items() if not k.startswith('KIRBY_TEST_')}
        env['PATH']=str(native/'bin')+os.pathsep+env.get('PATH','')
        config=folder/'kirby-settings.ini'
        for name,text in [('default',None),('invalid','[video]\nwindow_width=-1\nwindow_height=999999\nrender_width=0\nrender_height=0\nfps=999\n'),
            ('custom','[video]\nwindow_width=1366\nwindow_height=768\nrender_width=2560\nrender_height=1440\nfps=120\n')]:
            if text is not None:config.write_text(text)
            subprocess.run([str(exe),name],env=env,check=True)
    sr.save_json(ROOT/'reports/settings-tests.json',{'result':'passed','synthetic_only':True,'checks':[
        'Win32 settings defaults and invalid/custom dimensions', '120 render frames consume exactly 60 fixed simulation updates',
        'repeated delta reads are stable; stalls cannot cause unbounded catch-up']})
if __name__=='__main__':main()
