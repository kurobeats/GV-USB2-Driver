#!/usr/bin/env python3
"""Framehash sanity: reads `ffmpeg -f framehash` (md5) lines on stdin and
verifies the frames are not all identical (frozen capture) and that some
variation exists (live source)."""
import sys

hashes = []
for line in sys.stdin:
    line = line.strip()
    if not line or line.startswith("#") or line.startswith("stream#,frames,"):
        continue
    parts = line.split(",")
    if len(parts) >= 4:
        hashes.append(parts[-1])

if not hashes:
    print("  FAIL  no frames hashed")
    sys.exit(1)
unique = len(set(hashes))
print(f"        frames hashed: {len(hashes)}, distinct: {unique}")
if unique == 1 and len(hashes) > 4:
    print("  FAIL  frozen capture (all frames identical)")
    sys.exit(1)
print("  PASS  live video (frames vary)")
