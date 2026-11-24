#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Bytecode containers for esp-quickjs-ng (format in include/qjs_esp.h).

  qjs_esp_bc.py compile main.js -o main.qbc [--strip source|debug]

compile builds tools/qjsc-esp from the quickjs/ submodule on first use (needs
cmake and a C compiler), so the bytecode matches the device's QuickJS version.
Bundle local imports into one file first; si:*, qjs:* etc. are left to the
device. The device runs the bytecode as it is, there's no signature (yet).
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FORMAT = 1


def host_compiler(build_dir):
    exe = os.path.join(build_dir, 'qjsc-esp')
    src = os.path.join(HERE, 'qjsc-esp')
    if not os.path.exists(os.path.join(build_dir, 'CMakeCache.txt')):
        subprocess.run(['cmake', '-S', src, '-B', build_dir, '-DCMAKE_BUILD_TYPE=Release'],
                       check=True, stdout=subprocess.DEVNULL)
    # a no-op when up to date; rebuilds after a quickjs bump
    subprocess.run(['cmake', '--build', build_dir, '--target', 'qjsc-esp'],
                   check=True, stdout=subprocess.DEVNULL)
    return exe


def cmd_compile(a):
    qjsc = a.qjsc or host_compiler(a.build_dir)
    name = a.name or os.path.basename(a.input)
    strip = {'none': [], 'source': ['-s'], 'debug': ['-ss']}[a.strip]

    with tempfile.TemporaryDirectory() as tmp:
        raw = os.path.join(tmp, 'out.bc')
        r = subprocess.run([qjsc, *strip, '-n', name, a.input, raw])
        if r.returncode:
            sys.exit(r.returncode)
        with open(raw, 'rb') as f:
            bc = f.read()

    blob = b'QJBC' + struct.pack('<BBHI', FORMAT, 0, 0, len(bc)) + bc
    with open(a.output, 'wb') as f:
        f.write(blob)
    print(f'{a.output}: {len(bc)} bytes of bytecode')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)

    c = sub.add_parser('compile', help='ES module -> container')
    c.add_argument('input')
    c.add_argument('-o', '--output', required=True)
    c.add_argument('--name', help='file name in stack traces (default: input basename)')
    c.add_argument('--strip', choices=['none', 'source', 'debug'], default='none',
                   help="source: drop source text; debug: line numbers too (errors lose file:line)")
    c.add_argument('--qjsc', help='use this qjsc-esp instead of building one')
    c.add_argument('--build-dir', default=os.path.join(HERE, 'qjsc-esp', 'build'))
    c.set_defaults(fn=cmd_compile)

    a = p.parse_args()
    a.fn(a)


if __name__ == '__main__':
    main()
