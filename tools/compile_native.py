#!/usr/bin/env python3
"""Build the replaceable telemetry library with the NDK; no APK/Gradle needed."""
import argparse, json, os, pathlib, subprocess, zipfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--name',default='Quest telemetry')
p.add_argument('--source',type=pathlib.Path,default=ROOT/'native/telemetry.cpp',help='C++ source defining android_main(android_app*)')
p.add_argument('--output',type=pathlib.Path,default=ROOT/'build/telemetry.so')
p.add_argument('--ndk',type=pathlib.Path,default=pathlib.Path(os.environ.get('ANDROID_NDK_HOME','/tmp/teleop_tools/android/ndk/27.2.12479018')))
p.add_argument('--openxr-aar',type=pathlib.Path)
a=p.parse_args()
if not a.openxr_aar:
 cache=pathlib.Path(os.environ.get('GRADLE_USER_HOME','/tmp/teleop_tools/gradle'))
 candidates=list((cache/'caches/modules-2/files-2.1/org.khronos.openxr/openxr_loader_for_android/1.1.53').glob('*/*.aar'))
 if not candidates:p.error('Supply --openxr-aar pointing to openxr_loader_for_android 1.1.53 AAR')
 a.openxr_aar=candidates[0]
work=ROOT/'build/native-app';work.mkdir(parents=True,exist_ok=True)
with zipfile.ZipFile(a.openxr_aar) as z:
 for info in z.infolist():
  path=pathlib.PurePosixPath(info.filename)
  if path.is_absolute() or '..' in path.parts:raise ValueError('Unsafe AAR path')
  if info.filename.startswith('prefab/modules/'):z.extract(info,work/'openxr')
prefab=work/'openxr/prefab/modules'
bin=a.ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin'
cc=bin/'aarch64-linux-android28-clang';cxx=bin/'aarch64-linux-android28-clang++'
glue=a.ndk/'sources/android/native_app_glue'
def run(args):subprocess.run([str(x) for x in args],check=True)
common=['-O2','-fPIC','-I'+str(ROOT/'native')]
run([cc,*common,'-DQUEST_NATIVE_APP_NAME='+json.dumps(a.name),'-c',ROOT/'native/identity.c','-o',work/'identity.o'])
run([cc,*common,'-I'+str(glue),'-c',glue/'android_native_app_glue.c','-o',work/'glue.o'])
a.output=a.output.resolve();a.output.parent.mkdir(parents=True,exist_ok=True)
run([cxx,*common,'-std=c++17','-shared','-static-libstdc++','-Wl,-z,max-page-size=16384',
 '-Wl,-u,ANativeActivity_onCreate','-DXR_USE_PLATFORM_ANDROID','-DXR_USE_GRAPHICS_API_OPENGL_ES',
 '-I'+str(glue),'-I'+str(prefab/'headers/include'),a.source.resolve(),
 work/'identity.o',work/'glue.o','-L'+str(prefab/'openxr_loader/libs/android.arm64-v8a'),
 '-lopenxr_loader','-landroid','-lEGL','-lGLESv3','-llog','-o',a.output])
print(a.output)
