#!/usr/bin/env python3
"""Package an existing device .app for public sideloading without its signing data."""
import argparse
import hashlib
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('app', type=Path)
parser.add_argument('output', type=Path)
args = parser.parse_args()
if not (args.app / 'Info.plist').is_file():
    parser.error('app must be a built iPhoneOS .app')
args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='nfsu2-package-') as scratch:
    payload = Path(scratch) / 'Payload'
    app = payload / args.app.name
    shutil.copytree(args.app, app)
    for p in list(app.rglob('*')):
        if p.name == '_CodeSignature' and p.is_dir():
            shutil.rmtree(p)
        elif p.name in {'embedded.mobileprovision', '.DS_Store'} and p.is_file():
            p.unlink()
    magic = {b'\xcf\xfa\xed\xfe', b'\xce\xfa\xed\xfe', b'\xca\xfe\xba\xbe'}
    for p in app.rglob('*'):
        if not p.is_file():
            continue
        with p.open('rb') as f:
            is_macho = f.read(4) in magic
        if is_macho:
            subprocess.run(['codesign', '--remove-signature', str(p)], check=True)
        # FFmpeg embeds its configure command as informational text. Replace
        # only its local --prefix value, preserving every Mach-O byte offset.
        if is_macho and p.name in {'avcodec', 'avutil'}:
            data = p.read_bytes()
            def public_prefix(match):
                replacement = b'--prefix=/build/ffmpeg'
                if len(match[0]) < len(replacement):
                    raise RuntimeError('Unexpected short configure prefix')
                return replacement.ljust(len(match[0]), b' ')
            data = re.sub(rb'--prefix=/(?:Users|Volumes)/[^\x00 ]+', public_prefix, data)
            p.write_bytes(data)
        if re.search(rb'/(?:Users|Volumes)/', p.read_bytes()):
            raise RuntimeError(f'Local build path remains in {p.name}')
        if p.suffix.lower() in {'.xbe', '.iso', '.xiso', '.mp3', '.p12', '.mobileprovision', '.pem', '.key'}:
            raise RuntimeError(f'Unexpected private/game file: {p.name}')
    with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for p in sorted(payload.rglob('*')):
            if p.is_file():
                z.write(p, p.relative_to(Path(scratch)))
    with zipfile.ZipFile(args.output) as z:
        assert z.testzip() is None
checksum = hashlib.sha256(args.output.read_bytes()).hexdigest()
args.output.with_suffix(args.output.suffix + '.sha256').write_text(f'{checksum}  {args.output.name}\n')
print(f'Unsigned IPA: {args.output} ({args.output.stat().st_size:,} bytes)')
