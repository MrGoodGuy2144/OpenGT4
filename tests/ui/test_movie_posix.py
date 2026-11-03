"""Exercise the real FFmpeg child with synthetic MPEG-2 and unusual paths."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

binary, ffmpeg = map(lambda p: str(Path(p).resolve()), sys.argv[1:])
with tempfile.TemporaryDirectory(prefix="opengt4 movie test ") as folder:
    fixture = Path(folder) / "synthetic video.mpg"
    subprocess.run([ffmpeg, "-v", "error", "-f", "lavfi", "-i",
                    "testsrc2=size=640x448:rate=30000/1001", "-t", "0.5",
                    "-c:v", "mpeg2video", "-an", str(fixture)], check=True)
    env = dict(os.environ, TMPDIR=folder)
    subprocess.run([binary, str(fixture), ffmpeg], env=env, check=True, timeout=30)
    leftovers = list(Path(folder).glob("opengt4-movie-*"))
    if leftovers:
        raise RuntimeError(f"Leaked movie temporary files: {leftovers}")
