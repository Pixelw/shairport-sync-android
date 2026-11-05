#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Pixelw and contributors
"""仅在项目 work/ 构建固定主机工具，不修改全局安装。"""
import os,subprocess,tarfile,json
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
os.chdir(ROOT)
prefix=ROOT/'work/host-tools'
base=ROOT/'work/deps/host'
base.mkdir(parents=True,exist_ok=True)
env=os.environ.copy(); env['PATH']=str(prefix/'bin')+':'+env['PATH']
for name,version in [('make','4.4.1'),('m4','1.4.20'),('autoconf','2.72'),('automake','1.17'),('libtool','2.5.4'),('pkgconf','2.5.1')]:
    marker=prefix/f'.{name}-{version}'
    if marker.exists(): continue
    source=base/f'{name}-{version}'
    if not source.exists():
        with tarfile.open(ROOT/f'work/downloads/{name}-{version}.archive') as archive:
            archive.extractall(base,filter='data')
    log=ROOT/f'work/logs/host-{name}.log'
    with log.open('w') as output:
        cmds=[['sh','configure',f'--prefix={prefix}'],['make','-j6'],['make','install']]
        if name=='make':cmds[0].append('--disable-dependency-tracking')
        for cmd in cmds:
            print(name,' '.join(cmd),flush=True)
            subprocess.run(cmd,cwd=source,env=env,stdout=output,stderr=subprocess.STDOUT,check=True)
    marker.touch()
if not (prefix/'bin/pkg-config').exists():(prefix/'bin/pkg-config').symlink_to('pkgconf')
print('Project-local host tools ready.')
