#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Pixelw and contributors
"""按依赖锁下载公开源码到项目缓存；支持断网核验，不安装全局软件。"""
import argparse,hashlib,json,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser();parser.add_argument('--offline',action='store_true');args=parser.parse_args()
lock=json.loads((ROOT/'dependencies.lock.json').read_text())
for name,row in lock.items():
    path=ROOT/'work/downloads'/row['file'];path.parent.mkdir(parents=True,exist_ok=True)
    if not path.exists():
        if args.offline:raise FileNotFoundError(path)
        part=path.with_suffix('.part')
        try:
            subprocess.run(['curl','-fLsS','--retry','2','--connect-timeout','15','--max-time','180',row['url'],'-o',str(part)],check=True)
            assert hashlib.sha256(part.read_bytes()).hexdigest()==row['sha256'],name
            part.replace(path)
        finally:part.unlink(missing_ok=True)
    assert path.stat().st_size==row['bytes'] and hashlib.sha256(path.read_bytes()).hexdigest()==row['sha256'],name
    print(name,row['version'],'archive verified')
