#!/usr/bin/env python3
"""Measure the actual TUI before/after verified, entirely offline HTTPS turns.

The test-only DNS shim sends the provider hostname to a loopback TLS server
and rejects other named destinations. Both certificate stores contain the
same system roots plus a temporary api.anthropic.com certificate. No real
credentials, provider traffic, application endpoint changes or child curl
processes are involved. Native timings must run separately from profilers.
"""
import sys
sys.dont_write_bytecode = True
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import subprocess
import tempfile
import termios
import time
import bench_http as http
from bench_startup import read_memory


def run(binary, shim, port, trust_env, work, turns):
    binary_header, shim_header = binary.read_bytes()[:5], shim.read_bytes()[:5]
    if binary_header[:4] != b"\x7fELF" or binary_header != shim_header:
        raise RuntimeError("DNS shim must match the executable's ELF class")
    work.mkdir()
    master, slave = os.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8",
           "TERM": "xterm-256color", "XDG_CONFIG_HOME": str(work / "config"),
           "XDG_STATE_HOME": str(work / "state"), "ANTHROPIC_API_KEY": "offline-benchmark",
           "LD_PRELOAD": str(shim), "PYTTE_TEST_HTTP_PORT": str(port), **trust_env}
    start = time.perf_counter_ns()
    proc = subprocess.Popen([str(binary), "-m", "anthropic/claude-opus-5-5"],
                            stdin=slave, stdout=slave, stderr=slave, cwd=work, env=env)
    os.close(slave)
    wire = bytearray()
    terminal_reply = False
    log = None
    partial = b""
    completed = 0
    entries = {}
    first_text = None
    turn_start = None
    samples = []
    expected = hashlib.sha256(http.response(512)[1]).hexdigest()

    def drain(timeout):
        nonlocal terminal_reply, first_text
        if select.select([master], [], [], timeout)[0]:
            data = os.read(master, 65536)
            if not data:
                raise RuntimeError("TUI exited before completion")
            wire.extend(data)
            if not terminal_reply and b"\x1b[c" in wire:
                os.write(master, b"\x1b[1;1R\x1b[?0u\x1b[?1;2c")
                terminal_reply = True
            if turn_start is not None and first_text is None and b"offline response" in wire:
                first_text = (time.perf_counter_ns() - turn_start) / 1e6
            if len(wire) > 1048576:
                del wire[:-1048576]

    def records():
        nonlocal log, partial, completed
        if log is None:
            paths = list((work / "state/pytte/sessions").glob("*/log.jsonl"))
            if paths:
                log = paths[0].open("rb")
        if log is not None:
            partial += log.read()
            lines = partial.split(b"\n")
            partial = lines.pop()
            for line in lines:
                for item in json.loads(line)["w"]:
                    if item["t"] == "entry" and item.get("kind") == "assistant":
                        text = "".join(c["text"] for c in item["data"]["message"]["content"]
                                       if c["type"] == "text").encode()
                        entries[item["id"]] = hashlib.sha256(text).hexdigest()
                    if item["t"] == "task" and item.get("kind") == "run" and item.get("status") == "done":
                        outcome = item["outcome"]
                        if outcome["status"] != "completed" or entries.get(outcome.get("entry")) != expected:
                            raise RuntimeError(f"incomplete or incorrect TUI response: {outcome}")
                        completed += 1

    def settle():
        end = time.monotonic() + .05
        while time.monotonic() < end:
            drain(max(0, end - time.monotonic()))
            records()

    try:
        end = time.monotonic() + 10
        while b"/help for commands" not in wire:
            if time.monotonic() >= end:
                raise TimeoutError(f"TUI failed to launch: {wire[-1000:]!r}")
            drain(.01)
        launch_ms = (time.perf_counter_ns() - start) / 1e6
        settle()
        samples.append({"phase": "startup", "turn": 0, "launch_ms": launch_ms,
                        "memory_kib": read_memory(proc.pid)})
        for turn in range(1, turns + 1):
            wire.clear()
            first_text = None
            turn_start = time.perf_counter_ns()
            os.write(master, b"hi\r")
            end = time.monotonic() + 15
            while completed < turn:
                if time.monotonic() >= end:
                    raise TimeoutError(f"TUI turn {turn} failed: {wire[-1000:]!r}")
                drain(.002)
                records()
            elapsed_ms = (time.perf_counter_ns() - turn_start) / 1e6
            settle()
            if first_text is None or completed != turn:
                raise RuntimeError("TUI did not render exactly one complete reply")
            if turn in (1, turns):
                children = Path(f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()
                # The TUI has an existing terminal-restoration guardian.
                # Account for it separately and ensure it never loads curl.
                child_memory = []
                for child in children:
                    child_maps = Path(f"/proc/{child}/maps").read_text()
                    if re.search(r"/libcurl\.", child_maps):
                        raise RuntimeError("networking moved into a child process")
                    child_memory.append(read_memory(int(child)))
                mappings = Path(f"/proc/{proc.pid}/maps").read_text()
                libraries = sorted(set(line.split()[-1] for line in mappings.splitlines()
                                       if re.search(r"/lib(?:curl|ssl|crypto)\.", line)))
                samples.append({"phase": "response", "turn": turn, "elapsed_ms": elapsed_ms,
                                "first_text_ms": first_text, "memory_kib": read_memory(proc.pid),
                                "sha256": expected, "child_memory_kib": child_memory, "system_libraries": libraries})
        os.write(master, b"\x03\x03\x04")
        proc.wait(timeout=10)
        if proc.returncode:
            raise RuntimeError(f"TUI exit status {proc.returncode}")
        return samples
    finally:
        if log:
            log.close()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
        os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", nargs="+", required=True, metavar="LABEL=PATH")
    parser.add_argument("--directory-build", nargs="*", default=[])
    parser.add_argument("--shim", type=Path, default=Path("build/bench/tui-dns.so"))
    parser.add_argument("--shim32", type=Path, help="matching 32-bit test-only DNS shim")
    parser.add_argument("--runs", type=int, default=9)
    parser.add_argument("--cases", nargs="+", choices=("https", "http2", "conversation"),
                        default=["https", "http2", "conversation"])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.runs <= 0 or args.runs % 3:
        parser.error("runs must be a positive multiple of three")
    binaries = dict(entry.split("=", 1) for entry in args.bin)
    if len(binaries) != len(args.bin) or any(name not in binaries for name in args.directory_build):
        parser.error("binary labels must be unique and match --directory-build")
    binaries = {name: Path(path).resolve() for name, path in binaries.items()}
    shim = args.shim.resolve()
    shims = {name: (args.shim32.resolve() if path.read_bytes()[4] == 1 and args.shim32 else shim)
             for name, path in binaries.items()}
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    ca = Path("/etc/ssl/certs/ca-certificates.crt")
    data = {"instrumented": False, "shim_sha256": hashlib.sha256(shim.read_bytes()).hexdigest(),
            "shims": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                      for name, path in shims.items()},
            "directory_builds": args.directory_build,
            "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                         for name, path in binaries.items()}, "samples": {}, "summary": {}}
    with tempfile.TemporaryDirectory(prefix="pytte-tui-https-") as td:
        temp = Path(td)
        cert, key, bundle, directory = temp / "local.pem", temp / "local.key", temp / "bundle.pem", temp / "certs"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                        "-subj", "/CN=api.anthropic.com", "-addext", "subjectAltName=DNS:api.anthropic.com,DNS:localhost",
                        "-keyout", str(key), "-out", str(cert)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        bundle.write_bytes(ca.read_bytes() + b"\n" + cert.read_bytes())
        directory.mkdir()
        roots = re.finditer(r"-----BEGIN (TRUSTED CERTIFICATE|CERTIFICATE)-----.*?-----END \1-----", bundle.read_text(), re.S)
        for index, root in enumerate(roots):
            (directory / f"root-{index}.pem").write_text(root.group(0) + "\n")
        subprocess.run(["openssl", "rehash", str(directory)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for case in args.cases:
            turns = 100 if case == "conversation" else 3
            data["samples"][case] = {name: [] for name in binaries}
            with http.server(cert, key, http2=case == "http2") as (srv, _):
                for batch in range(3):
                    for index in range(args.runs // 3 + 1):
                        order = list(binaries.items())
                        if (batch + index) % 2:
                            order.reverse()
                        for name, binary in order:
                            trust = {"SSL_CERT_DIR": str(directory)} if name in args.directory_build else {"SSL_CERT_FILE": str(bundle)}
                            work = temp / f"{case}-{batch}-{index}-{name}"
                            samples = run(binary, shims[name], srv.server_port, trust, work, turns)
                            if index:
                                data["samples"][case][name].append(samples)
                if case == "http2" and srv.h2_responses != srv.requests:
                    raise RuntimeError("actual TUI did not negotiate HTTP/2")
            data["summary"][case] = {}
            for name, runs in data["samples"][case].items():
                stages = {}
                for phase, turn in (("startup", 0), ("response", 1), ("response", turns)):
                    values = [s for samples in runs for s in samples if s["phase"] == phase and s["turn"] == turn]
                    stages[f"{phase}_{turn}"] = {key: http.summary([s[key] for s in values])
                                                    for key in ("launch_ms", "elapsed_ms", "first_text_ms") if key in values[0]}
                    stages[f"{phase}_{turn}"]["memory_kib"] = {key: http.summary([s["memory_kib"][key] for s in values])
                                                                for key in values[0]["memory_kib"]}
                data["summary"][case][name] = stages
                print(case, name, stages, flush=True)
            output.write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    main()
