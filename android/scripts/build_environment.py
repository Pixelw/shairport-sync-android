#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Pixelw and contributors
"""解析可配置NDK路径和电脑端工具链，不访问设备。"""
from pathlib import Path
import os
import platform

ROOT = Path(__file__).resolve().parent.parent


def ndk_root():
    """从环境或项目缓存选择NDK，并固定使用已验证的r27d。"""
    configured = os.environ.get('ANDROID_NDK_ROOT') or os.environ.get('ANDROID_NDK_HOME')
    ndk = Path(configured).expanduser().resolve() if configured else ROOT / 'work/toolchains/android-ndk-r27d'
    properties = (ndk / 'source.properties').read_text()
    if 'Pkg.Revision = 27.3.13750724' not in properties:
        raise ValueError('This build requires NDK r27d (27.3.13750724): ' + str(ndk))
    return ndk


def toolchain_bin(ndk):
    """选择当前主机的NDK LLVM工具，不混用系统交叉编译器。"""
    tag = 'darwin-x86_64' if platform.system() == 'Darwin' else 'linux-x86_64'
    if platform.system() not in ('Darwin', 'Linux'):
        raise ValueError('Use a macOS or Linux build host')
    path = ndk / 'toolchains/llvm/prebuilt' / tag / 'bin'
    if not (path / 'armv7a-linux-androideabi27-clang').is_file():
        raise FileNotFoundError(path)
    return path


def build_triplet():
    """生成Autoconf使用的主机三元组。"""
    cpu = 'aarch64' if platform.machine() in ('arm64', 'aarch64') else 'x86_64'
    return cpu + ('-apple-darwin' if platform.system() == 'Darwin' else '-pc-linux-gnu')
