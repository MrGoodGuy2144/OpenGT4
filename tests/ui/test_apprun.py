"""AppRun path persistence and argv boundaries without a real disc or game."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class AppRunTest(unittest.TestCase):
    def test_arguments_and_remembered_disc(self):
        with tempfile.TemporaryDirectory(prefix="opengt4 wrapper ") as directory:
            root = Path(directory)
            app = root / "AppDir"
            (app / "usr/bin").mkdir(parents=True)
            shutil.copy2(ROOT / "packaging/linux/AppRun", app / "AppRun")
            game = app / "usr/bin/opengt4"
            game.write_text('#!/usr/bin/env python3\nimport json,os,sys\nprint(json.dumps({"args":sys.argv[1:],"ffmpeg":os.environ["OPENGT4_FFMPEG_BINARY"]}))\n')
            game.chmod(0o755)
            disc = root / "disc $(no-shell) ' spaces.iso"
            disc.touch()
            env = dict(os.environ, XDG_CONFIG_HOME=str(root / "config"))
            def run(*args):
                result = subprocess.run([str(app / "AppRun"), *args], env=env, text=True,
                                        capture_output=True, check=True)
                return json.loads(result.stdout)
            first = run(str(disc), "--save-dir", str(root / "my saves"))
            self.assertEqual(first["args"], [str(disc), "--fullscreen", "--save-dir", str(root / "my saves")])
            self.assertEqual(first["ffmpeg"], str(app / "usr/bin/ffmpeg"))
            self.assertEqual(run()["args"], [str(disc), "--fullscreen"])
            # An explicit relative path must survive a later working-directory change.
            relative = os.path.relpath(disc)
            self.assertEqual(run(relative)["args"], [str(disc), "--fullscreen"])
            self.assertEqual(run()["args"], [str(disc), "--fullscreen"])

if __name__ == "__main__":
    unittest.main()
