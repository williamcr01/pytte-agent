#!/usr/bin/env python3
"""Warm-cache launch/input latency on an isolated, offline 80x24 PTY.

Run before/after on the same machine and filesystem. No credentials are
loaded. The benchmark records raw samples; it does not assert a machine's
timing threshold in functional tests.
"""
import argparse
import ctypes
import errno
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import select
import statistics
import struct
import subprocess
import tempfile
import termios
import time


def read_memory(pid):
    values = {}
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()[1:]:
        fields = line.split()
        if fields[0] in ("Rss:", "Pss:", "Private_Clean:", "Private_Dirty:"):
            values[fields[0][:-1]] = int(fields[1])
    values["USS"] = values["Private_Clean"] + values["Private_Dirty"]
    return values


def run(binary, env, script, mode):
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    command = [str(binary), "-m", "faux/faux", "--script", str(script)]
    if mode == "continue_session":
        command = [str(binary), "-c", "--script", str(script)]
    started = time.perf_counter_ns()
    pid = os.posix_spawn(command[0], command, env,
                         file_actions=[(os.POSIX_SPAWN_DUP2, slave, fd) for fd in (0, 1, 2)])
    os.close(slave)
    wire = bytearray()
    replied = False
    reaped = False
    first = None

    def receive(until, deadline=10):
        nonlocal replied, first
        end = time.monotonic() + deadline
        while not until():
            if not select.select([master], [], [], max(0, end - time.monotonic()))[0]:
                raise TimeoutError(f"{mode}: expected output missing: {wire[-1000:]!r}")
            data = os.read(master, 65536)
            if not data:
                raise RuntimeError("PTY closed before expected output")
            now = time.perf_counter_ns()
            if first is None:
                first = (now - started) / 1e6
            wire.extend(data)
            if mode != "silent_terminal" and not replied and b"\x1b[c" in wire:
                os.write(master, b"\x1b[1;1R\x1b[?0u\x1b[?1;2c")
                replied = True
        return time.perf_counter_ns()

    try:
        ready = receive(lambda: b"/help for commands" in wire and b"faux" in wire and
                        (mode != "continue_session" or b"offline response" in wire))
        result = {"first_output_ms": first, "prompt_ms": (ready - started) / 1e6}
        marker = b"input-ready"
        if mode in ("during_commit", "during_stream", "during_switch", "during_settings"):
            # Complete initial setup before submitting a new request, so
            # this case exercises commits rather than just startup I/O.
            end = time.monotonic() + 5
            while True:
                logs = list(Path(env["XDG_STATE_HOME"]).glob("pytte/sessions/*/log.jsonl"))
                if logs and logs[0].read_bytes().count(b"\n") >= 2:
                    break
                if time.monotonic() >= end:
                    raise TimeoutError("initial durable claims did not finish")
                if select.select([master], [], [], .001)[0]:
                    os.read(master, 65536)
            if mode == "during_settings":
                os.write(master, b"/settings thinking hidden\r")
            elif mode == "during_switch":
                os.write(master, b"/new\r")
            else:
                os.write(master, b"hi\r")
            if mode == "during_stream":
                wire.clear()
                receive(lambda: b"offline response" in wire)
            # Input arrives while the new user/request commits become durable.
            # This delay is in the harness, outside the measured input interval.
            time.sleep(.002)
        wire.clear()
        typed = time.perf_counter_ns()
        os.write(master, marker)
        echoed = receive(lambda: marker in wire)
        result["input_ms"] = (echoed - typed) / 1e6
        # Memory is sampled after setup settles, not just at the first
        # frame while the new build's background worker is still opening.
        settle = time.monotonic() + .1
        while time.monotonic() < settle:
            if select.select([master], [], [], max(0, settle - time.monotonic()))[0]:
                os.read(master, 65536)
        result["app_memory_kib"] = read_memory(pid)
        children = Path(f"/proc/{pid}/task/{pid}/children").read_text().split()
        result["child_memory_kib"] = [read_memory(int(p)) for p in children]
        # Clear draft, interrupt any run, then quit normally.
        os.write(master, b"\x03\x03\x04")
        end = time.monotonic() + 10
        while time.monotonic() < end:
            if select.select([master], [], [], .1)[0]:
                try:
                    if not os.read(master, 65536):
                        break
                except OSError as exc:
                    if exc.errno == errno.EIO:
                        break
                    raise
        _, status = os.waitpid(pid, 0)
        reaped = True
        if os.waitstatus_to_exitcode(status) != 0:
            raise RuntimeError(f"pytte exit status {status}")
        while True:
            try:
                os.waitpid(-1, 0)
            except ChildProcessError:
                break
        return result
    finally:
        if not reaped:
            try:
                os.kill(pid, 9)
                os.waitpid(pid, 0)
            except ProcessLookupError:
                pass
        os.close(master)


def summary(samples, key):
    numbers = sorted(s[key] for s in samples)
    return {"median": statistics.median(numbers),
            "p95": numbers[math.ceil(len(numbers) * .95) - 1],
            "min": numbers[0], "max": numbers[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, default=Path("build/pytte"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=30)
    parser.add_argument("--work", type=Path, default=Path("build/opt/work"))
    parser.add_argument("--modes", nargs="+", default=["normal_terminal", "silent_terminal",
                        "during_commit", "during_stream", "during_switch", "during_settings",
                        "continue_session", "large_history"])
    args = parser.parse_args()
    if args.runs < 3 or args.runs % 3:
        parser.error("--runs must be a positive multiple of 3 (three batches)")
    binary = args.bin.resolve()
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    output = args.output.resolve()
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    config = work / "config"
    config.mkdir(exist_ok=True)
    script = work / "stream.json"
    script.write_text(json.dumps({"chunk": 64, "delay_ms": 2, "turns": [
        {"content": [{"type": "text", "text": "offline response " * 128}]}]}))
    os.chdir(work)
    if ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), "PR_SET_CHILD_SUBREAPER")
    seed_state = work / "seed-state"
    seed_state.mkdir(exist_ok=True)
    seed_env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "TERM": "xterm-256color",
                "XDG_STATE_HOME": str(seed_state), "XDG_CONFIG_HOME": str(config),
                "PYTTE_ASCII": "1", "NO_COLOR": "1"}
    subprocess.run([str(binary), "-p", "-m", "faux/faux", "--script", str(script), "seed"],
                   env=seed_env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=True)
    seed_log = sorted((seed_state / "pytte/sessions").glob("*/log.jsonl"))[-1].read_bytes()
    samples = {mode: [] for mode in args.modes}
    for batch in range(3):
        for mode in samples:
            for index in range(args.runs // 3 + 1):
                with tempfile.TemporaryDirectory(prefix="state-", dir=work) as state:
                    env = {"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8", "TERM": "xterm-256color",
                           "XDG_STATE_HOME": state, "XDG_CONFIG_HOME": str(config),
                           "PYTTE_ASCII": "1", "NO_COLOR": "1"}
                    if mode == "continue_session":
                        session = Path(state) / "pytte/sessions/20000101-000000-aaaa"
                        session.mkdir(parents=True)
                        (session / "log.jsonl").write_bytes(seed_log)
                    if mode == "large_history":
                        history = Path(state) / "pytte/history"
                        history.parent.mkdir(parents=True)
                        history.write_text("".join(json.dumps(f"entry {i}: " + "history " * 32) + "\n"
                                                   for i in range(1000)))
                    value = run(binary, env, script, mode)
                if index:
                    samples[mode].append(value)
            print(batch + 1, mode, {k: summary(samples[mode], k) for k in
                  ("prompt_ms", "input_ms")}, flush=True)
        output.write_text(json.dumps({"binary": str(binary), "sha256": digest,
                                     "samples": samples}, indent=2) + "\n")


if __name__ == "__main__":
    main()
