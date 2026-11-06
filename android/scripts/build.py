#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Pixelw and contributors
"""从仓库内源码构建SPS/NQPTP并审计ARMv7/API27导入；不部署设备。"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
from build_environment import ROOT, ndk_root, toolchain_bin, build_triplet

REPO = ROOT.parent


def digest(path):
    """计算输入或产物摘要。"""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log, cwd, env):
    """将构建输出写入本地日志并保留失败返回码。"""
    print(' '.join(map(str, command)), flush=True)
    log.write('$ ' + ' '.join(map(str, command)) + '\n')
    log.flush()
    subprocess.run(command, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)


def audit(binary, ndk, tool):
    """核验ELF32/ARM/PIE和全部API27动态导入，不执行目标程序。"""
    report = subprocess.check_output([str(tool / 'llvm-readelf'), '-h', '-l', '-d', '--dyn-syms', '--wide', str(binary)], text=True)
    assert all(s in report for s in ('ELF32', 'ARM', 'DYN', '/system/bin/linker]', 'GNU_RELRO', 'BIND_NOW')), binary
    assert not re.search(r'hard-float ABI|RPATH|RUNPATH|TEXTREL|pthread_cancel|__atomic_|AudioTrack', report), binary
    needed = re.findall(r'Shared library: \[(.*?)\]', report)
    assert set(needed) <= {'libc.so', 'libm.so', 'libdl.so', 'libaaudio.so', 'liblog.so'}, needed
    imports = {f[7].split('@')[0] for line in report.splitlines() if len(f := line.split()) >= 8 and f[0].rstrip(':').isdigit() and f[6] == 'UND' and f[4] in ('GLOBAL', 'WEAK')}
    available = set()
    for lib in needed:
        stub = subprocess.check_output([str(tool / 'llvm-readelf'), '--dyn-syms', '--wide', str(tool.parent / 'sysroot/usr/lib/arm-linux-androideabi/27' / lib)], text=True)
        available |= {f[7].split('@')[0] for line in stub.splitlines() if len(f := line.split()) >= 8 and f[0].rstrip(':').isdigit() and f[6] != 'UND' and f[4] in ('GLOBAL', 'WEAK')}
    assert not imports - available, imports - available
    binary.with_suffix('.elf.txt').write_text(report)
    return {'sha256': digest(binary), 'needed': needed, 'imports': sorted(imports)}


def main():
    """恢复公开依赖，在隔离目录构建并输出审计清单。"""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true', help='Require all locked dependency archives in android/work/downloads')
    args = parser.parse_args()
    ndk = ndk_root()
    tool = toolchain_bin(ndk)
    logs = ROOT / 'work/logs'
    logs.mkdir(parents=True, exist_ok=True)
    # 禁止优化模式关闭构建及ABI核验。
    if not __debug__:
        raise RuntimeError('Run without python -O / PYTHONOPTIMIZE')
    for script in ('bootstrap_dependencies.py', 'build_host_tools.py', 'build_dependencies.py', 'build_host_plist.py'):
        command = ['python3', str(ROOT / 'scripts' / script)]
        if script == 'bootstrap_dependencies.py' and args.offline:
            command.append('--offline')
        subprocess.run(command, cwd=ROOT, check=True)
    host = ROOT / 'work/host-tools'
    prefix = ROOT / 'work/target'
    env = os.environ.copy()
    env.update(PATH=f'{host}/bin:{tool}:' + env['PATH'], CC=str(tool/'armv7a-linux-androideabi27-clang'), CXX=str(tool/'armv7a-linux-androideabi27-clang++'), AR=str(tool/'llvm-ar'), RANLIB=str(tool/'llvm-ranlib'), STRIP=str(tool/'llvm-strip'), PKG_CONFIG_LIBDIR=f'{prefix}/lib/pkgconfig', PKG_CONFIG_PATH='', CFLAGS=f'-O2 -g -fPIE -D_GNU_SOURCE', CPPFLAGS=f'-I{prefix}/include', LDFLAGS=f'-L{prefix}/lib -pie -Wl,--gc-sections,--as-needed,-z,relro,-z,now,--no-undefined', LIBS='-lm -ldl -llog')
    output = ROOT / 'outputs/armv7-api27'
    output.mkdir(parents=True, exist_ok=True)
    artifacts = {}
    sources = {}
    for project in ('nqptp', 'shairport-sync'):
        original = ROOT if project == 'nqptp' else REPO
        snapshot = ROOT / 'work/build' / project
        if snapshot.exists():
            shutil.rmtree(snapshot)
        # 构建副本保持源码相对布局，但不复制任何本地缓存与产物。
        shutil.copytree(original, snapshot, ignore=shutil.ignore_patterns('.git', 'work', 'outputs', '.DS_Store', '__pycache__', '.gradle', 'app'))
        source = snapshot / 'nqptp' if project == 'nqptp' else snapshot
        sources[project] = {str(p.relative_to(snapshot)): digest(p) for p in sorted(snapshot.rglob('*')) if p.is_file()}
        flags = ['--host=arm-linux-androideabi', '--build='+build_triplet(), '--prefix=/data/local/tmp/lx04-ap2']
        config_env = env.copy()
        if project == 'shairport-sync':
            flags += ['--with-os=android', '--with-android-companion', '--with-airplay-2', '--with-ssl=openssl', '--with-dns_sd', '--without-configfiles', '--without-create-user-group']
            config_env['LIBS'] += ' -laaudio -lgpg-error'
        with (logs / (project+'.log')).open('w') as log:
            for command in (['autoreconf', '-fi'], ['sh', 'configure']+flags, ['make', '-j6']):
                run(command, log, source, config_env)
        shutil.copy2(source/project, output/project)
        artifacts[project] = audit(output/project, ndk, tool)
    (output/'manifest.json').write_text(json.dumps({'target': 'ARMv7/Bionic/API27/PIE', 'ndk_revision': '27.3.13750724', 'artifacts': artifacts, 'sources': sources, 'shared_headers': {p.name: digest(p) for p in sorted((ROOT/'include').glob('*.h'))}}, indent=2)+'\n')
    print('PASS: SPS and NQPTP built; ELF/ABI/API27 import audits passed. Output: '+str(output), flush=True)


if __name__ == '__main__':
    main()
