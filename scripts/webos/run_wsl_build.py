#!/usr/bin/env python3
import pathlib
import subprocess
import sys

src = pathlib.Path("/mnt/c/Projetos/moonlight/lg/moonlight-tv/scripts/webos/wsl_build_once.sh")
dst = pathlib.Path("/tmp/wsl_build_once.sh")
data = src.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"")
dst.write_bytes(data)
dst.chmod(0o755)
raise SystemExit(subprocess.call(["bash", str(dst)] + sys.argv[1:]))
