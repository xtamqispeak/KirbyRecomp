"""Compile original Maxwell regression fixtures through GLSL and shaderc SPIR-V."""
from pathlib import Path
import os
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
import switchrecomp as sr

def main():
    native=ROOT/'build/native-deps/ucrt64'
    exe=ROOT/'build/shader-tests.exe'
    compiler=sr.compiler_command()
    subprocess.run(compiler+['-std=gnu11','-O1','-I',str(sr.VENDOR/'runtime'),'-idirafter',str(native/'include'),
        str(ROOT/'tests/shader_native.c'),str(sr.VENDOR/'runtime/nvn/maxwell/translate.c'),
        str(native/'lib/libshaderc_shared.dll.a'),'-o',str(exe)]+sr.architecture_flags(compiler),check=True)
    env=dict(os.environ);env['PATH']=str(native/'bin')+os.pathsep+env.get('PATH','')
    subprocess.run([str(exe)],env=env,check=True)
    sr.save_json(ROOT/'reports/shader-tests.json',{'result':'passed','synthetic_only':True,'checks':[
        'conditional primitive clip exit preserves subsequent position exports',
        'synthetic vertex shader compiles to SPIR-V',
        'ordinary LEA compiles to SPIR-V; extended carry form rejects explicitly']})

if __name__=='__main__':main()
