#!/usr/bin/env python3
"""Compresses the web page for embedding in the firmware and checks the 40 KB limit."""
import gzip
import sys

LIMIT = 40 * 1024

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
gz = gzip.compress(data, compresslevel=9, mtime=0)
if len(gz) > LIMIT:
    sys.exit(f"index.html.gz = {len(gz)} bytes, exceeds the limit {LIMIT}")
open(dst, "wb").write(gz)
print(f"index.html: {len(data)} -> {len(gz)} bytes (gzip)")
