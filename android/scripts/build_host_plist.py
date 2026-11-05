#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Pixelw and contributors
"""构建原生主机 plistutil，避免交叉构建时执行 ARM 工具。"""
from pathlib import Path
import os,shutil,subprocess
ROOT=Path(__file__).resolve().parent.parent
source=ROOT/'work/deps/host/libplist-2.7.0'
prefix=ROOT/'work/host-tools'
if not (prefix/'bin/plistutil').exists():
    if not source.exists():shutil.copytree(ROOT/'work/deps/target/libplist',source)
    env=os.environ.copy();env['PATH']=str(prefix/'bin')+':'+env['PATH']
    env.update(CC='clang',CXX='clang++',AR='ar',RANLIB='ranlib',CFLAGS='-O2',CXXFLAGS='-O2',CPPFLAGS='',LDFLAGS='',PKG_CONFIG_LIBDIR=str(prefix/'lib/pkgconfig'),PKG_CONFIG_PATH='')
    with (ROOT/'work/logs/host-plistutil.log').open('w') as log:
        for command in [['make','distclean'],['sh','configure',f'--prefix={prefix}','--disable-shared','--enable-static','--without-cython','--without-tests'],['make','-j6'],['make','install']]:
            subprocess.run(command,cwd=source,env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
print('Host-native plistutil ready; target utilities never executed.')
