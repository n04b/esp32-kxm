#!/usr/bin/env python3
"""Сжимает веб-страницу для вшивания в прошивку и проверяет лимит 40 КБ."""
import gzip
import sys

LIMIT = 40 * 1024

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
gz = gzip.compress(data, compresslevel=9, mtime=0)
if len(gz) > LIMIT:
    sys.exit(f"index.html.gz = {len(gz)} байт, больше лимита {LIMIT}")
open(dst, "wb").write(gz)
print(f"index.html: {len(data)} -> {len(gz)} байт (gzip)")
