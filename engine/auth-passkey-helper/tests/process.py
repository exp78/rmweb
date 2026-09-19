#!/usr/bin/env python3
"""Real helper-process tests; all inputs terminate before Bluetooth acquisition."""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time
import unittest

COMMAND = []


def signal_ready(pid):
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            status = Path(f"/proc/{pid}/status").read_text()
            caught = next(line.split()[1] for line in status.splitlines() if line.startswith("SigCgt:"))
            if int(caught, 16) & (1 << (signal.SIGTERM - 1)):
                return
        except (FileNotFoundError, StopIteration):
            pass
        time.sleep(0.01)
    raise AssertionError("helper did not install its signal handler")


class ProcessTest(unittest.TestCase):
    def rejected(self, payload, expected="OperationError"):
        result = subprocess.run(COMMAND, input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stderr, b"")
        self.assertEqual(json.loads(result.stdout), {"type":"error", "name":expected})
        self.assertEqual(result.stdout.count(b"\n"), 1)

    def test_malformed_private_input_is_not_echoed(self):
        self.rejected(b'{"private":"MUST_NOT_APPEAR_IN_OUTPUT"}')

    def test_oversized_input_is_bounded(self):
        self.rejected(b"x" * (128 * 1024 + 1))

    def test_multiple_requests_are_rejected(self):
        self.rejected(b"{}\n{}\n")

    def test_missing_eof_has_two_second_deadline(self):
        process = subprocess.Popen(COMMAND, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        start = time.monotonic()
        try:
            process.wait(timeout=4)
            self.assertLess(time.monotonic() - start, 3.5)
            self.assertEqual(json.loads(process.stdout.read()), {"type":"error","name":"NotAllowedError"})
            self.assertEqual(process.stderr.read(),b"")
        finally:
            if process.poll() is None: process.kill()
            process.communicate()

    def test_sigterm_cancels_waiting_input(self):
        process = subprocess.Popen(COMMAND, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            signal_ready(process.pid)
            process.send_signal(signal.SIGTERM)
            # Keep stdin open until exit so EOF cannot win the signal race.
            process.wait(timeout=3)
            stdout, stderr = process.communicate(timeout=3)
            self.assertEqual(json.loads(stdout), {"type":"error","name":"NotAllowedError"})
            self.assertEqual(stderr,b"")
            self.assertEqual(process.returncode,1)
        finally:
            if process.poll() is None: process.kill(); process.wait()

    def test_closed_stderr_is_replaced_without_closing_null_descriptor(self):
        process = subprocess.Popen(COMMAND, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, preexec_fn=lambda: os.close(2))
        try:
            signal_ready(process.pid)
            self.assertEqual(os.readlink(f"/proc/{process.pid}/fd/2"), "/dev/null")
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=3)
        finally:
            if process.poll() is None: process.kill()
            process.communicate()

    def test_parent_death_cancels_waiting_input(self):
        input_read, input_write = os.pipe()
        output_read, output_write = os.pipe()
        wrapper = None
        helper_pid = None
        try:
            program = "import subprocess,sys,time; p=subprocess.Popen(sys.argv[3:],stdin=int(sys.argv[1]),stdout=int(sys.argv[2]),stderr=subprocess.DEVNULL); print(p.pid,flush=True); time.sleep(10)"
            wrapper = subprocess.Popen([sys.executable,"-c",program,str(input_read),str(output_write),*COMMAND],
                pass_fds=(input_read,output_write),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            os.close(input_read); input_read = -1
            os.close(output_write); output_write = -1
            helper_pid = int(wrapper.stdout.readline())
            signal_ready(helper_pid)
            wrapper.kill(); wrapper.wait(timeout=2)
            # Keep stdin's writer open: EOF must not explain this cancellation.
            chunks = []
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                ready,_,_ = select.select([output_read],[],[],deadline-time.monotonic())
                self.assertTrue(ready,"helper did not terminate after parent death")
                data = os.read(output_read,4096)
                if not data: break
                chunks.append(data)
            else:
                self.fail("helper pipe remained open")
            self.assertEqual(json.loads(b"".join(chunks)), {"type":"error","name":"NotAllowedError"})
        finally:
            if wrapper is not None:
                if wrapper.poll() is None: wrapper.kill()
                wrapper.communicate()
            for fd in [input_read,input_write,output_read,output_write]:
                if fd >= 0: os.close(fd)
            if helper_pid is not None:
                try:
                    state = Path(f"/proc/{helper_pid}/status").read_text()
                    if "\nState:\tZ" not in state: os.kill(helper_pid,signal.SIGKILL)
                except ProcessLookupError: pass
                except FileNotFoundError: pass


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary",required=True)
    parser.add_argument("--loader",required=True)
    parser.add_argument("--library-path",required=True)
    args = parser.parse_args()
    COMMAND[:] = [args.loader,"--library-path",args.library_path,args.binary]
    unittest.main(argv=[sys.argv[0]])
