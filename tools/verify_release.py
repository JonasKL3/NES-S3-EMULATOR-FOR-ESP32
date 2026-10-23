#!/usr/bin/env python3
"""Checks release manifest and offline-only invariants; not a hardware test."""
from pathlib import Path
import hashlib,json,sys
base=Path(__file__).resolve().parents[1]
obj=json.loads((base/'SHA256SUMS.json').read_text(encoding='utf-8'))
failed=[]
for rel, sha in obj['files'].items():
 p=base/rel
 actual=hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else 'MISSING'
 if actual!=sha: failed.append(rel)
firmware=base/'firmware'/'NES_S3'
for p in firmware.glob('*'):
 if p.suffix.lower() in ('.cpp','.h','.ino') and any(t in p.read_text(errors='replace') for t in ['#include <WiFi.h>','#include <HTTPClient.h>','#include <LittleFS.h>']):failed.append(str(p.relative_to(base))+' (unexpected WiFi)')
print('CHECKSUMS',len(obj['files']),'files', 'OK' if not failed else 'FAILED')
for f in failed: print('FAIL:',f)
sys.exit(bool(failed))
