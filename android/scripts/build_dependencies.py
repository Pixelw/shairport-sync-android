#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Pixelw and contributors
"""在项目内为 ARMv7/Bionic/API27 构建静态 PIC 依赖，主机工具严格隔离。"""
import argparse,os,json,tarfile,subprocess,hashlib
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
os.chdir(ROOT)
native='--native' in __import__('sys').argv
base=ROOT/('work/deps/native' if native else 'work/deps/target');base.mkdir(parents=True,exist_ok=True)
prefix=ROOT/('work/native' if native else 'work/target');prefix.mkdir(parents=True,exist_ok=True)
from build_environment import ndk_root, toolchain_bin, build_triplet
ndk=ndk_root()
bin=toolchain_bin(ndk)
host=ROOT/'work/host-tools'
env=os.environ.copy()
env.update(PATH=f'{host}/bin:{bin}:'+env['PATH'],CC=str(bin/'armv7a-linux-androideabi27-clang'),CXX=str(bin/'armv7a-linux-androideabi27-clang++'),AR=str(bin/'llvm-ar'),RANLIB=str(bin/'llvm-ranlib'),STRIP=str(bin/'llvm-strip'),CFLAGS='-O2 -fPIC -ffunction-sections -fdata-sections',CXXFLAGS='-O2 -fPIC -ffunction-sections -fdata-sections',LDFLAGS=f'-L{prefix}/lib',CPPFLAGS=f'-I{prefix}/include',PKG_CONFIG_LIBDIR=f'{prefix}/lib/pkgconfig',PKG_CONFIG_PATH='',ANDROID_NDK_ROOT=str(ndk))
if native:
    env.update(CC='/usr/bin/clang',CXX='/usr/bin/clang++',AR='/usr/bin/ar',RANLIB='/usr/bin/ranlib',STRIP='/usr/bin/strip',PATH=f'{host}/bin:'+os.environ['PATH'])
parser=argparse.ArgumentParser();parser.add_argument('--native',action='store_true');parser.add_argument('packages',nargs='*');args=parser.parse_args()
lock=json.loads(Path('dependencies.lock.json').read_text())
order=args.packages or ['popt','libconfig','libplist','libsodium','libgpg-error','libgcrypt','util-linux','openssl','ffmpeg','mdnsresponder']
recipe=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
products={'popt':['libpopt.a'],'libconfig':['libconfig.a'],'libplist':['libplist-2.0.a','libplist++-2.0.a'],'libsodium':['libsodium.a'],'libgpg-error':['libgpg-error.a'],'libgcrypt':['libgcrypt.a'],'util-linux':['libuuid.a'],'openssl':['libcrypto.a','libssl.a'],'ffmpeg':['libavcodec.a','libavformat.a','libavutil.a','libswresample.a'],'mdnsresponder':['libdns_sd.a']}
for name in order:
    row=lock[name];marker=prefix/f'.{name}-{row["version"]}'
    if marker.exists() and marker.stat().st_size:
        saved=json.loads(marker.read_text())
        if saved.get('recipe_sha256')==recipe and saved.get('source_sha256')==row['sha256'] and all((prefix/'lib'/file).exists() and hashlib.sha256((prefix/'lib'/file).read_bytes()).hexdigest()==checksum for file,checksum in saved['outputs'].items()):
            print(name,'verified build cache',flush=True);continue
    archive=ROOT/'work/downloads'/row['file']
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==row['sha256'],name
    source=base/name
    if not source.exists():
        source.mkdir()
        with tarfile.open(archive) as bundle:
            members=bundle.getmembers();first=members[0].name.split('/')[0]
            staging=base/f'.unpack-{name}';staging.mkdir(exist_ok=True)
            bundle.extractall(staging,filter='data')
            for path in (staging/first).iterdir():path.rename(source/path.name)
            (staging/first).rmdir();(staging/'.DS_Store').unlink(missing_ok=True);staging.rmdir()
    if name=='popt':
        # 本接收器不支持 popt 外部程序别名；显式报错，不链接 execvp 路径。
        with tarfile.open(archive) as bundle:
            member=next(m for m in bundle if m.name.endswith('/src/popt.c'))
            original=bundle.extractfile(member).read().decode()
        begin=original.index('static int execCommand(')
        end=original.index('\nstatic const struct poptOption *',begin)
        changed=original[:begin]+'static int execCommand(poptContext con)\n{\n    /* LX04 禁用外部程序别名，不执行任意命令。 */\n    (void)con; return POPT_ERROR_BADOPERATION;\n}\n'+original[end:]
        (source/'src/popt.c').write_text(changed)
        import difflib
    logfile=ROOT/f'work/logs/dependency-{"native-" if native else ""}{name}.log'
    with logfile.open('w') as log:
        def run(command,variables=None):
            """记录并执行单项配置/构建命令，不运行目标生成器。"""
            print(name,' '.join(command),flush=True);log.write('$ '+' '.join(command)+'\n');log.flush()
            subprocess.run(command,cwd=source,env=variables or env,stdout=log,stderr=subprocess.STDOUT,check=True)
        common=[f'--prefix={prefix}',('--host=aarch64-apple-darwin' if native else '--host=arm-linux-androideabi'),'--build='+build_triplet(),'--disable-shared','--enable-static']
        if name in ['popt','libconfig','libplist','libsodium','libgpg-error','libgcrypt','util-linux']:
            if name=='libplist':(source/'.tarball-version').write_text(row['version']+'\n')
            if name=='libplist' or not (source/'configure').exists():run(['autoreconf','-fi'])
            flags=[]
            if name=='popt':
                env['ac_cv_header_glob_h']='no'
                flags=['--disable-nls']
            if name=='libconfig':flags=['--disable-cxx','--disable-examples','--disable-doc','--disable-tests']
            if name=='libplist':
                (source/'.tarball-version').write_text(row['version']+'\n')
                flags=['--without-cython','--without-tools','--without-tests']
            if name=='libsodium':flags=['--disable-ssp','--disable-asm']
            if name=='libgpg-error':flags=['--disable-nls','--disable-doc','--disable-tests','--disable-languages','--enable-install-gpg-error-config']
            if name=='libgcrypt':flags=['--disable-doc','--disable-asm',f'--with-libgpg-error-prefix={prefix}']
            if name=='util-linux':flags=['--disable-all-programs','--disable-year2038','--enable-libuuid','--disable-tls','--without-ncurses','--without-ncursesw','--without-readline','--disable-nls','--disable-rpath']
            run(['sh','configure']+common+flags)
            run(['make','-j6'])
            run(['make','install'])
        elif name=='openssl':
            openssl_env=env.copy();openssl_env['CC']='/usr/bin/clang' if native else 'clang'
            if not native:openssl_env['PATH']=f'{bin}:'+openssl_env['PATH']
            run(['perl','Configure',('darwin64-arm64-cc' if native else 'android-arm'),f'--prefix={prefix}','--libdir=lib']+([] if native else ['-D__ANDROID_API__=27'])+['no-shared','no-tests','no-asm','no-dso','no-engine']+([] if native else ['--with-rand-seed=devrandom']),openssl_env)
            run(['make','-j6','build_libs'],openssl_env);run(['make','install_dev'],openssl_env)
        elif name=='ffmpeg':
            run(['sh','configure',f'--prefix={prefix}',('--target-os=darwin' if native else '--target-os=android'),('--arch=aarch64' if native else '--arch=arm'),('--cpu=apple-m1' if native else '--cpu=armv7-a'),'--enable-cross-compile',f'--cc={env["CC"]}',f'--cxx={env["CXX"]}',f'--ar={env["AR"]}',f'--ranlib={env["RANLIB"]}',f'--strip={env["STRIP"]}','--disable-shared','--enable-static','--enable-pic','--disable-programs','--disable-doc','--disable-network','--disable-autodetect','--disable-mediacodec','--disable-jni','--disable-hwaccels','--disable-everything','--enable-avcodec','--enable-avformat','--enable-avutil','--enable-swresample','--enable-decoder=alac,aac,pcm_s16le,pcm_s16be','--enable-parser=aac','--enable-demuxer=mov,aac,pcm_s16le,pcm_s16be','--disable-asm','--disable-symver','--extra-cflags=-O2 -fPIC'])
            run(['make','-j6']);run(['make','install'])
        elif name=='mdnsresponder':
            shared=source/'mDNSShared'
            flags=[env['CC'],'-O2','-fPIC','-D_GNU_SOURCE','-DNOT_HAVE_SA_LEN','-DUSES_NETLINK','-DMDNS_UDS_SERVERPATH="/dev/socket/mdnsd"',f'-I{shared}',f'-I{source}/mDNSCore',f'-I{source}/mDNSPosix']
            objects=[]
            for file in ['dnssd_clientlib.c','dnssd_clientstub.c','dnssd_ipc.c']:
                obj=source/(file+'.o');run(flags+['-c',str(shared/file),'-o',str(obj)]);objects.append(str(obj))
            (prefix/'lib').mkdir(exist_ok=True);(prefix/'include').mkdir(exist_ok=True)
            run([env['AR'],'rcs',str(prefix/'lib/libdns_sd.a')]+objects)
            import shutil
            shutil.copy2(shared/'dns_sd.h',prefix/'include/dns_sd.h')
    marker.write_text(json.dumps({'recipe_sha256':recipe,'source_sha256':row['sha256'],'native':native,'outputs':{file:hashlib.sha256((prefix/'lib'/file).read_bytes()).hexdigest() for file in products[name]}},indent=2)+'\n')
    print(name,'PASS',flush=True)
