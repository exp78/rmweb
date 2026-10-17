#!/usr/bin/env python3
"""Run the real relocatable launcher with disposable runtime and entry fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


LAUNCHER = Path(__file__).resolve().parents[1] / "device/auth/entry"
CLEARED_BEFORE_RUNTIME = ("LD_PRELOAD", "RMWEB_JSC_OPTS", "RMWEB_JIT", "RMWEB_SKIA_THREADS")
CLEARED_BEFORE_ENTRY = ("QT_QPA_PLATFORM", "QT_QUICK_BACKEND", "QSG_RENDER_LOOP",
                        "WEBKIT_INSPECTOR_SERVER", "WEBKIT_INSPECTOR_HTTP_SERVER",
                        "WEBKIT_DEBUG", "SSLKEYLOGFILE")


class AuthLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="rmweb-auth-launcher-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name).resolve()
        self.root = self.directory / "relocated auth 'quoted'"
        (self.root / "bin").mkdir(parents=True)
        (self.root / "runtime").mkdir()
        self.launcher = self.root / "entry"
        shutil.copyfile(LAUNCHER, self.launcher)
        self.launcher.chmod(0o700)
        self.record = self.directory / "record.json"
        self.environment = {
            "PATH": os.environ["PATH"], "QTFB_KEY": "123", "FIXTURE_RESULT": str(self.record),
            "RMWEB_AUTH_RUNTIME": "/untrusted/runtime-override",
            **{name: "private-test-value" for name in (*CLEARED_BEFORE_RUNTIME, *CLEARED_BEFORE_ENTRY)},
        }
        # An empty inherited value tests unsetting without dynamic-loader
        # diagnostics before the shell can clear LD_PRELOAD on Linux.
        self.environment["LD_PRELOAD"] = ""
        self.runtime = self.root / "runtime/rmweb-env.sh"
        self.runtime.write_text(
            "[ -n \"${RMWEB_AUTH_RUNTIME:-}\" ] || exit 21\n"
            + "\n".join(f'[ "${{{name}+set}}" != set ] || exit 22' for name in CLEARED_BEFORE_RUNTIME)
            + '\nexport LD_LIBRARY_PATH="$RMWEB_AUTH_RUNTIME/lib"\n'
            + "\n".join(f"export {name}=runtime-test-value" for name in CLEARED_BEFORE_ENTRY)
            + "\n"
        )
        self.native = self.root / "bin/rmweb-auth-entry"
        self.native.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, resource, sys\n"
            "mode = os.umask(0o077)\n"
            "record = {'argv': sys.argv[1:], 'runtime': os.environ.get('RMWEB_AUTH_RUNTIME'),\n"
            "          'libraries': os.environ.get('LD_LIBRARY_PATH'), 'umask': mode,\n"
            "          'core': resource.getrlimit(resource.RLIMIT_CORE), 'pid': os.getpid(),\n"
            f"          'cleared': {{key: os.environ.get(key) for key in {(*CLEARED_BEFORE_RUNTIME, *CLEARED_BEFORE_ENTRY)!r}" "}}\n"
            "pathlib.Path(os.environ['FIXTURE_RESULT']).write_text(json.dumps(record))\n"
        )
        self.native.chmod(0o700)

    def run_launcher(self, arguments, okay=True, environment=None, launcher=None):
        process = subprocess.Popen([str(launcher or self.launcher), *arguments], cwd=self.directory,
                                   env=environment or self.environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode == 0, okay, (process.returncode, stdout, stderr))
        self.assertEqual(stdout, "")
        if okay:
            self.assertEqual(stderr, "")
            value = json.loads(self.record.read_text())
            self.assertEqual(value["pid"], process.pid)
            return value
        self.assertEqual(stderr, "[Authentication] Browser is unavailable.\n")
        self.assertFalse(self.record.exists())
        return None

    def test_relocated_launcher_preserves_literal_url_and_private_environment(self):
        marker = self.directory / "must-not-exist"
        url = f"https://login.example.test:8443/authorize?secret=private-code&x=$(touch${{IFS}}{marker})&y=`false`"
        value = self.run_launcher([url])
        self.assertEqual(value["argv"], [url])
        self.assertEqual(value["runtime"], str(self.root / "runtime"))
        self.assertEqual(value["libraries"], str(self.root / "runtime/lib"))
        self.assertEqual(value["umask"], 0o077)
        self.assertEqual(value["core"], [0, 0])
        self.assertEqual(value["cleared"], dict.fromkeys((*CLEARED_BEFORE_RUNTIME, *CLEARED_BEFORE_ENTRY)))
        self.assertFalse(marker.exists())

    def test_device_code_for_any_provider_is_forwarded_without_logging(self):
        arguments = ["https://another.example.test/device", "--device-code", "PRIVATE-1234"]
        self.assertEqual(self.run_launcher(arguments)["argv"], arguments)

    def test_invalid_argument_shapes_fail_with_fixed_message(self):
        for arguments in ([], ["private-url", "extra"], ["private-url", "--other", "private-code"],
                          ["private-url", "--device-code", "private-code", "extra"]):
            with self.subTest(count=len(arguments)):
                self.run_launcher(arguments, okay=False)

    def test_invalid_display_key_fails_without_disclosure(self):
        for key in (None, "", "private-display-key", "-1"):
            environment = dict(self.environment)
            if key is None:
                environment.pop("QTFB_KEY")
            else:
                environment["QTFB_KEY"] = key
            with self.subTest(key_present=key is not None):
                self.run_launcher(["https://example.test/"], okay=False, environment=environment)

    def test_missing_native_entry_fails_with_fixed_message(self):
        self.native.unlink()
        self.run_launcher(["https://example.test/?code=private"], okay=False)

    def test_symlink_native_entry_is_rejected(self):
        original = self.native.with_suffix(".original")
        self.native.rename(original)
        self.native.symlink_to(original)
        self.run_launcher(["https://example.test/?code=private"], okay=False)

    def test_symlink_runtime_script_is_rejected(self):
        original = self.runtime.with_suffix(".original")
        self.runtime.rename(original)
        self.runtime.symlink_to(original)
        self.run_launcher(["https://example.test/?code=private"], okay=False)

    def test_symlink_launcher_is_rejected(self):
        alias = self.directory / "alias"
        alias.symlink_to(self.launcher)
        self.run_launcher(["https://example.test/?code=private"], okay=False, launcher=alias)


if __name__ == "__main__":
    unittest.main()
