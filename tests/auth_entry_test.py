#!/usr/bin/env python3
"""Exercise the actual SDK entry binary in a disposable, root-owned container."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--without-mount-capability", action="store_true")
    parser.add_argument("--application-root", type=Path, default=Path("/opt/rmweb-auth-fixture"))
    args = parser.parse_args()
    if not Path("/.dockerenv").is_file() or os.geteuid() != 0:
        parser.error("run only inside a disposable root-owned Docker container")
    auth = args.application_root
    if not auth.is_absolute() or auth != auth.resolve() or len(auth.parts) < 3:
        parser.error("fixture application root must be a canonical absolute subdirectory")
    runtime = auth / "runtime"
    if auth.exists():
        parser.error("fixture application paths must not already exist")
    (runtime / "libexec/wpe-webkit-2.0").mkdir(parents=True)
    (auth / "bin").mkdir(parents=True)
    entry = auth / "bin/rmweb-auth-entry"
    shutil.copyfile(args.binary, entry)
    entry.chmod(0o755)
    target = Path("/usr/libexec")
    target.mkdir(exist_ok=True)
    marker = target / "auth-entry-parent-marker"
    marker.write_text("parent")
    for name in ("WPEWebProcess", "WPENetworkProcess", "WPEGPUProcess"):
        shutil.copy2("/usr/bin/true", runtime / "libexec/wpe-webkit-2.0" / name)
    browser = auth / "bin/rmweb-auth-browser"
    source = Path("/tmp/auth-entry-fixture.c")
    source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/statvfs.h>
int main(int argc, char **argv) {
    struct statvfs fs = {0};
    if (statvfs("/usr/libexec", &fs)) return 2;
    printf("%ld\n%d\n%s\n%lu\n", (long)getpid(), argc,
        getenv("LD_PRELOAD") ? "preload" : "clean", (unsigned long)(fs.f_flag & ST_RDONLY));
    for (int i = 1; i < argc; ++i) puts(argv[i]);
    return access("/usr/libexec/auth-entry-parent-marker", F_OK) == 0 ? 3 : 0;
}
''')
    subprocess.run(["gcc", str(source), "-o", str(browser)], check=True, timeout=30)
    env = dict(os.environ, QTFB_KEY="123", LD_PRELOAD="/missing-auth-test-preload.so")
    count = 0

    def run(values, okay=False, contains="", environment=None):
        nonlocal count
        process = subprocess.Popen([str(entry), *values], env=environment or env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        out, err = process.communicate(timeout=5)
        assert (process.returncode == 0) == okay, (process.returncode, out, err)
        assert contains in out + err, (out, err)
        assert marker.read_text() == "parent"
        assert not (target / "wpe-webkit-2.0").exists()
        count += 1
        return process.pid, out

    if args.without_mount_capability:
        run(["--check"], contains="could not create a private mount namespace")
        print("PASS missing mount capability fails closed; parent namespace preserved")
        return
    url = "https://login.example.test/device"
    for values in ([], [url, "extra"], [url, "--other", "AB-12"],
                   ["http://login.example.test/device", "--device-code", "AB-12"],
                   [url, "--device-code", ""], [url, "--device-code", "a"],
                   [url, "--device-code", "A" * 65], [url, "--device-code", "AB\n12"],
                   ["http://login.example.test/"], ["file:///tmp/x"], ["https://"], [url + "\n"], [url + "a" * 8193]):
        run(values)
    for key in (None, "", "-1", "12x", "2147483648", " 12", "+12"):
        changed = dict(env)
        if key is None:
            changed.pop("QTFB_KEY")
        else:
            changed["QTFB_KEY"] = key
        run([url], contains="valid QTFB_KEY", environment=changed)
    browser.chmod(0o777)
    run(["--check"], contains="ownership")
    browser.chmod(0o755)
    original = browser.with_suffix(".original")
    browser.rename(original)
    browser.symlink_to(original)
    run(["--check"], contains="ownership")
    browser.unlink()
    original.rename(browser)
    helpers = runtime / "libexec"
    helpers.chmod(0o777)
    run(["--check"], contains="ownership")
    helpers.chmod(0o755)
    saved_helpers = runtime / "saved-libexec"
    helpers.rename(saved_helpers)
    helpers.symlink_to(saved_helpers)
    run(["--check"], contains="ownership")
    helpers.unlink()
    saved_helpers.rename(helpers)
    auth.chmod(0o777)
    run(["--check"], contains="ownership")
    auth.chmod(0o755)
    run(["--check"], okay=True, contains="private read-only helper overlay verified")
    for values in ([url], [url, "--device-code", "ABCD-1234"],
                   ["https://other.example.test/authorize?state=literal$(test)`argv`&x=1"]):
        pid, out = run(values, okay=True)
        assert out.splitlines() == [str(pid), str(len(values) + 1), "clean", "1", *values]
    print(f"PASS {count} auth entry cases; literal arguments, same PID, private read-only mounts")


if __name__ == "__main__":
    main()
