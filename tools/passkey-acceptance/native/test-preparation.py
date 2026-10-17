#!/usr/bin/env python3
"""Check the self-contained launcher and catalog-generation boundaries."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from catalog import manifest


class PreparationTests(unittest.TestCase):
    def launcher(self, directory):
        application = Path(directory) / "standalone app"
        (application / "bin").mkdir(parents=True)
        (application / "runtime").mkdir()
        shutil.copyfile(Path(__file__).with_name("launch"), application / "launch")
        (application / "launch").chmod(0o700)
        (application / "runtime/rmweb-env.sh").write_text(
            'export LD_LIBRARY_PATH="$RMWEB_AUTH_RUNTIME/lib"\n')
        for name in ("rmweb-auth-entry", "rmweb-auth-browser", "rmweb-auth-passkey"):
            path = application / "bin" / name
            path.write_text('#!/bin/sh\nprintf "%s\\n" "$RMWEB_AUTH_RUNTIME" "$LD_LIBRARY_PATH" "$1"\n')
            path.chmod(0o700)
        return application

    def run_launcher(self, application):
        return subprocess.run([str(application / "launch"), "https://passkey.example.com/verify"],
            env={**os.environ, "QTFB_KEY": "123", "RMWEB_AUTH_RUNTIME": "/unused/runtime"},
            capture_output=True, text=True, timeout=5)

    def test_launcher_uses_only_its_own_runtime_and_bin(self):
        with tempfile.TemporaryDirectory(prefix="acceptance-launch-") as directory:
            application = self.launcher(directory)
            result = self.run_launcher(application)
            self.assertEqual(result.returncode, 0, result.stderr)
            runtime = application.resolve() / "runtime"
            self.assertEqual(result.stdout.splitlines(),
                [str(runtime), str(runtime / "lib"), "https://passkey.example.com/verify"])

    def test_launcher_rejects_missing_helper_and_redirected_runtime(self):
        for component in ("bin/rmweb-auth-passkey", "runtime/rmweb-env.sh"):
            with self.subTest(component=component), tempfile.TemporaryDirectory() as directory:
                application = self.launcher(directory)
                path = application / component
                path.unlink()
                result = self.run_launcher(application)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                target = Path(directory) / "elsewhere"
                target.write_text("#!/bin/sh\nexit 0\n")
                target.chmod(0o700)
                path.symlink_to(target)
                result = self.run_launcher(application)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")

    @unittest.skipUnless(sys.platform == "linux" and os.geteuid() == 0, "root-owned tablet file contract")
    def test_private_demo_code_is_appended_only_to_browser_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            application = self.launcher(directory)
            code = application / "session-code"
            code.write_text("A" * 43)
            code.chmod(0o600)
            result = self.run_launcher(application)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines()[-1],
                "https://passkey.example.com/verify#join=" + "A" * 43)

    @unittest.skipUnless(sys.platform == "linux" and os.geteuid() == 0, "root-owned tablet file contract")
    def test_demo_code_rejects_unsafe_mode_symlink_and_invalid_contents(self):
        for value, mode, redirected in (("A" * 43, 0o644, False), ("A" * 129, 0o600, False),
                ("short", 0o600, False), ("A" * 43 + "&x=y", 0o600, False),
                ("A" * 43, 0o600, True)):
            with self.subTest(mode=mode, redirected=redirected), tempfile.TemporaryDirectory() as directory:
                application = self.launcher(directory)
                code = application / "session-code"
                target = Path(directory) / "redirect" if redirected else code
                target.write_text(value)
                target.chmod(mode)
                if redirected:
                    code.symlink_to(target)
                result = self.run_launcher(application)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, "")
                self.assertNotIn(value, result.stderr)

    def test_catalog_is_separate_and_uses_existing_keyboard(self):
        data = manifest("https://passkey.example.com:8443")
        self.assertEqual(data["args"], ["https://passkey.example.com:8443/verify"])
        self.assertTrue(data["supportsVirtualKeyboard"])
        self.assertEqual(data["application"], "/home/root/rmweb-passkey-acceptance/launch")
        self.assertEqual(data["workingDirectory"], "/home/root/rmweb-passkey-acceptance")
        self.assertEqual(data["environment"], {})

    def test_catalog_rejects_noncanonical_or_unsafe_origins(self):
        for origin in ("http://localhost", "https://example.com/", "https://user@example.com",
                       "https://example.com?x=1", "https://example.com#x", "https://1.2.3.4",
                       "https://EXAMPLE.com", "https://example.com:bad", "https://example.com\n",
                       "https://example.com:443", "https://localhost"):
            with self.subTest(origin=origin), self.assertRaises(ValueError):
                manifest(origin)


if __name__ == "__main__":
    unittest.main()
