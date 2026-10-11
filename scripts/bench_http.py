#!/usr/bin/env python3
"""Offline agent/HTTP/TLS benchmark and certificate-verification checks.

Replays identical synthetic Anthropic SSE over loopback HTTP or verified
HTTPS. Uses the system CA bundle plus a temporary localhost CA; this includes
the cost of a realistic trust store. The server is a separate process from
the measured C client. Never reads API credentials or contacts a provider.
Compare builds with --bin before=build/before/bench/http after=build/bench/http.
Profilers run separately using --profile dhat|massif|memcheck --runs 1.
"""
import argparse
import contextlib
import hashlib
import http.server
import json
import math
import os
from pathlib import Path
import platform
import re
import socket
import ssl
import statistics
import subprocess
import tempfile
import threading


def response(size):
    seed = "offline response with streaming text.\n"
    text = (seed * (size // len(seed) + 1))[:size]
    events = []

    def add(kind, **data):
        events.append(f"event: {kind}\ndata: " + json.dumps(
            {"type": kind, **data}, separators=(",", ":")) + "\n\n")

    add("message_start", message={"id": "msg_bench", "role": "assistant", "content": [],
                                  "usage": {"input_tokens": 25, "output_tokens": 1}})
    add("content_block_start", index=0, content_block={"type": "text", "text": ""})
    for at in range(0, len(text), 64):
        add("content_block_delta", index=0, delta={"type": "text_delta", "text": text[at:at + 64]})
    add("content_block_stop", index=0)
    add("message_delta", delta={"stop_reason": "end_turn"}, usage={"output_tokens": size // 4})
    add("message_stop")
    return "".join(events).encode(), text.encode()


class Replay(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def log_message(self, *args):
        pass

    def handle(self):
        try:
            if isinstance(self.connection, ssl.SSLSocket) and self.connection.selected_alpn_protocol() == "h2":
                self.handle_h2()
            else:
                super().handle()
        except (BrokenPipeError, ConnectionResetError):
            # Negative TLS cases close immediately after verification fails.
            pass

    def handle_h2(self):
        # A bounded replay peer, not a general HTTP/2 server. RFC 9113 frame
        # layout and RFC 7541 static indexes let this test use only stdlib.
        # Its small responses stay within the initial 65,535-byte windows;
        # large-response and 100-turn cases use the HTTP/1.1 replay peer.
        self.connection.settimeout(10)
        if self.rfile.read(24) != b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n":
            raise ValueError("invalid HTTP/2 preface")

        def frame(kind, flags=0, stream=0, payload=b""):
            self.wfile.write(len(payload).to_bytes(3, "big") + bytes([kind, flags]) +
                             stream.to_bytes(4, "big") + payload)
            self.wfile.flush()

        frame(4)  # SETTINGS
        bodies = {}
        while True:
            header = self.rfile.read(9)
            if not header:
                return
            if len(header) != 9:
                raise ValueError("incomplete HTTP/2 frame")
            n = int.from_bytes(header[:3], "big")
            kind, flags = header[3:5]
            stream = int.from_bytes(header[5:], "big") & 0x7fffffff
            if n > 16384:
                raise ValueError("oversized replay request frame")
            payload = self.rfile.read(n)
            if len(payload) != n:
                raise ValueError("incomplete HTTP/2 payload")
            if kind == 4 and not flags & 1:
                frame(4, 1)
            elif kind == 6 and not flags & 1:
                frame(6, 1, payload=payload)
            elif kind == 7:
                return
            elif kind == 3:
                bodies.pop(stream, None)
            elif kind in (0, 1):
                body = bodies.setdefault(stream, bytearray())
                if kind == 0:
                    if flags & 8:
                        payload = payload[1:len(payload) - payload[0]]
                    body.extend(payload)
                    if len(body) > 65535:
                        raise ValueError("replay request exceeds bounded HTTP/2 window")
                    if payload:
                        increment = len(payload).to_bytes(4, "big")
                        frame(8, stream=stream, payload=increment)
                        frame(8, payload=increment)
                if flags & 1:
                    json.loads(body)
                    self.server.requests += 1
                    wire, _ = response(self.server.reply_bytes)
                    if len(wire) > 16384:
                        raise ValueError("HTTP/2 replay response exceeds one frame")
                    # Indexed :status 200; literal content-type by static
                    # index 31. No dynamic HPACK state is needed for replies.
                    frame(1, 4, stream, b"\x88\x0f\x10\x11text/event-stream")
                    frame(0, 1, stream, wire)
                    bodies.pop(stream)
                    self.server.h2_responses += 1

    def do_POST(self):
        self.server.requests += 1
        raw = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        try:
            value = json.loads(raw)
            if self.path != "/transport":
                assert isinstance(value["messages"], list) and value["stream"] is True
        except (ValueError, KeyError, AssertionError):
            self.send_error(400, "invalid benchmark request")
            return
        wire, _ = response(self.server.reply_bytes)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(wire)))
        if self.server.reconnect:
            self.send_header("Connection", "close")
            self.close_connection = True
        self.end_headers()
        try:
            # Bounded network writes exercise incremental delivery. TCP may
            # coalesce them; parser byte fragmentation has its own benchmark.
            for at in range(0, len(wire), 512):
                self.wfile.write(wire[at:at + 512])
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


@contextlib.contextmanager
def server(cert=None, key=None, reconnect=False, tls_version=None, http2=False, cipher=None, curve=None):
    class ReplayServer(http.server.ThreadingHTTPServer):
        def get_request(self):
            self.connections += 1
            return super().get_request()

    srv = ReplayServer(("127.0.0.1", 0), Replay)
    srv.reply_bytes = 512
    srv.reconnect = reconnect
    srv.h2_responses = 0
    srv.requests = 0
    srv.connections = 0
    if cert:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.minimum_version = tls_version or ssl.TLSVersion.TLSv1_2
        if tls_version:
            ctx.maximum_version = tls_version
        if cipher:
            ctx.set_ciphers(cipher)
        if curve:
            ctx.set_ecdh_curve(curve)
        ctx.load_cert_chain(cert, key)
        ctx.set_alpn_protocols(["h2", "http/1.1"] if http2 else ["http/1.1"])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    thread = threading.Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    try:
        yield srv, f"{'https' if cert else 'http'}://localhost:{srv.server_port}"
    finally:
        srv.shutdown()
        srv.server_close()
        thread.join()


def measure(binary, url, turns, size, env, work, profile=None, transport=False, error=False):
    wire, text = response(size)
    with tempfile.TemporaryDirectory(prefix="session-", dir=work) as state:
        command = [str(binary), url + ("/transport" if transport else "/messages"),
                   str(turns), str(len(wire) if transport else len(text))]
        command += ["transport"] if transport else [str(Path(state) / "session")]
        if error:
            command += ["error"]
        if profile:
            prefix = Path(state)
            if profile == "dhat":
                extra = ["--tool=dhat", f"--dhat-out-file={prefix}/profile.json"]
            elif profile == "massif":
                extra = ["--tool=massif", "--time-unit=B", f"--massif-out-file={prefix}/profile.txt"]
            else:
                extra = ["--tool=memcheck", "--leak-check=full", "--show-leak-kinds=definite,indirect",
                         "--errors-for-leak-kinds=definite,indirect"]
            command = ["valgrind", "--vgdb=no", "--error-exitcode=99", *extra, *command]
        result = subprocess.run(command, env=env, capture_output=True, text=True,
                                timeout=180 if profile else 45)
        if result.returncode:
            raise RuntimeError(f"{binary}: status {result.returncode}\n{result.stdout}\n{result.stderr}")
        samples = [json.loads(line) for line in result.stdout.splitlines()]
        replies = [s for s in samples if s["phase"] == "response"]
        if len(replies) != turns:
            raise RuntimeError("response count mismatch")
        digest = hashlib.sha256(wire if transport else text).hexdigest()
        if not error and any(s["sha256"] != digest for s in replies):
            raise RuntimeError("response contents differ from replay")
        if profile:
            number = len(list(work.glob(f"{profile}-*.log")))
            stem = work / f"{profile}-{binary.parent.parent.name}-{number}"
            stem.with_suffix(".log").write_text(result.stderr)
            for path in Path(state).glob("profile.*"):
                stem.with_suffix(path.suffix).write_bytes(path.read_bytes())
        return samples


def summary(numbers):
    numbers = sorted(numbers)
    return {"median": statistics.median(numbers),
            "p95": numbers[math.ceil(len(numbers) * .95) - 1],
            "min": numbers[0], "max": numbers[-1]}


def verify_cli(binary, url, env):
    _, text = response(512)
    result = subprocess.run([str(binary), "anthropic/claude-opus-5-5", "--base-url", url, "hello"],
                            env={**env, "ANTHROPIC_API_KEY": "offline-benchmark"},
                            capture_output=True, timeout=15)
    if result.returncode or result.stdout not in (text, text + b"\n"):
        raise RuntimeError(f"{binary}: incomplete CLI response, status {result.returncode}: {result.stderr!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", nargs="+", default=["system=build/bench/http"])
    parser.add_argument("--ai-bin", nargs="+", default=[],
                        help="optional matching label=pytte-ai binaries to verify the actual CLI over HTTPS")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=9)
    parser.add_argument("--cases", nargs="+", default=["http", "https", "http2", "reconnect", "large", "conversation"])
    parser.add_argument("--profile", choices=("dhat", "massif", "memcheck"))
    parser.add_argument("--transport-only", action="store_true",
                        help="measure TLS and HTTP delivery without the agent or durable writes")
    parser.add_argument("--verify-only", action="store_true")
    parser.add_argument("--ca-bundle", type=Path, help="system trust bundle, before adding temporary localhost CA")
    parser.add_argument("--directory-build", nargs="*", default=[], metavar="LABEL",
                        help="labels measured with an equivalent hashed CA directory instead of a bundle")
    parser.add_argument("--defaults-bin", type=Path,
                        help="test-only client whose compiled-in curl trust defaults can be set by the fixture")
    args = parser.parse_args()
    cases = {"http": (False, False, 512, 3), "https": (True, False, 512, 3), "http2": (True, False, 512, 3),
             "reconnect": (True, True, 512, 3), "large": (True, False, 65536, 3),
             "conversation": (True, False, 512, 100)}
    if args.runs <= 0 or (not args.profile and not args.verify_only and args.runs % 3):
        parser.error("timing runs must be a positive multiple of three (three batches)")
    if any(case not in cases for case in args.cases):
        parser.error("unknown workload")
    binaries = {}
    for entry in args.bin:
        name, path = entry.split("=", 1) if "=" in entry else (Path(entry).parent.parent.name, entry)
        if name in binaries:
            parser.error(f"duplicate binary label {name}")
        binaries[name] = Path(path).resolve()
    ai_binaries = {}
    if any(name not in binaries for name in args.directory_build):
        parser.error("--directory-build labels must match --bin")
    for entry in args.ai_bin:
        if "=" not in entry:
            parser.error("--ai-bin requires label=path")
        name, path = entry.split("=", 1)
        if name not in binaries or name in ai_binaries:
            parser.error("--ai-bin labels must uniquely match --bin labels")
        ai_binaries[name] = Path(path).resolve()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    work = output.parent / (output.stem + "-profiles")
    work.mkdir(exist_ok=True)
    ca = args.ca_bundle or next((p for p in (Path("/etc/ssl/certs/ca-certificates.crt"),
                         Path("/etc/pki/tls/certs/ca-bundle.crt"), Path("/etc/ssl/cert.pem")) if p.is_file()), None)
    if ca is None:
        parser.error("set --ca-bundle to the system's CA bundle")
    data = {"machine": platform.platform(), "instrumented": args.profile,
            "transport_only": args.transport_only,
            "system_ca_sha256": hashlib.sha256(ca.read_bytes()).hexdigest(),
            "directory_builds": args.directory_build,
            "binaries": {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                         for name, path in binaries.items()}, "checks": {}, "samples": {}, "summary": {}}
    data["ai_binaries"] = {name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                           for name, path in ai_binaries.items()}
    with tempfile.TemporaryDirectory(prefix="pytte-http-") as temp:
        temp = Path(temp)
        cert, key = temp / "cert.pem", temp / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(key), "-out", str(cert)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        bundle = temp / "bundle.pem"
        bundle.write_bytes(ca.read_bytes() + b"\n" + cert.read_bytes())
        system_dir = temp / "system-certs"
        system_dir.mkdir()
        roots = list(re.finditer(r"-----BEGIN (TRUSTED CERTIFICATE|CERTIFICATE)-----.*?-----END \1-----",
                                 bundle.read_text(), re.S))
        if not roots:
            raise RuntimeError("CA bundle contains no certificates")
        for index, root in enumerate(roots):
            (system_dir / f"root-{index}.pem").write_text(root.group(0) + "\n")
        subprocess.run(["openssl", "rehash", str(system_dir)], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        data["trust_certificates"] = len(roots)
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LANG": "C.UTF-8",
               "SSL_CERT_FILE": str(bundle)}
        # Check both supported TLS versions and verify that certificate
        # failures cannot silently turn into successful benchmark responses.
        for name, binary in binaries.items():
            checks = []
            for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                with server(cert, key, tls_version=version) as (srv, url):
                    measure(binary, url, 1, 512, env, temp, transport=True)
                    checks.append(f"{version.name}: trusted")
            with server(cert, key) as (srv, url):
                if name in ai_binaries:
                    verify_cli(ai_binaries[name], url, env)
                    checks.append("actual CLI: complete HTTP/1.1 HTTPS response")
                measure(binary, url.replace("localhost", "127.0.0.1"), 1, 512, env, temp,
                        transport=True, error=True)
                checks.append("wrong hostname rejected")
                measure(binary, url, 1, 512, {**env, "SSL_CERT_FILE": str(ca.resolve())}, temp,
                        transport=True, error=True)
                checks.append("untrusted issuer rejected")
                measure(binary, url, 1, 512, {**env, "SSL_CERT_FILE": str(temp / "missing.pem")}, temp,
                        transport=True, error=True)
                checks.append("missing CA bundle rejected")
                ca_dir = temp / "certs"
                ca_dir.mkdir(exist_ok=True)
                (ca_dir / "local.pem").write_bytes(cert.read_bytes())
                subprocess.run(["openssl", "rehash", str(ca_dir)], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                measure(binary, url, 1, 512, {**env, "SSL_CERT_FILE": str(ca.resolve()),
                        "SSL_CERT_DIR": str(ca_dir)}, temp, transport=True)
                checks.append("CA directory override trusted")
            with server(cert, key, http2=True) as (srv, url):
                measure(binary, url, 2, 512, env, temp, transport=True)
                if srv.h2_responses != 2:
                    raise RuntimeError(f"{name}: HTTP/2 was not negotiated")
                checks.append("HTTP/2 negotiated, two complete responses")
                if name in ai_binaries:
                    verify_cli(ai_binaries[name], url, env)
                    if srv.h2_responses != 3:
                        raise RuntimeError(f"{name}: actual CLI did not negotiate HTTP/2")
                    checks.append("actual CLI: complete HTTP/2 HTTPS response")
            data["checks"][name] = checks
            print(name, checks, flush=True)
            # Exercise the optimized path as well as explicit file overrides.
            directory_env = {"PATH": env["PATH"], "LANG": env["LANG"],
                             "SSL_CERT_DIR": str(system_dir)}
            for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                with server(cert, key, tls_version=version) as (srv, url):
                    measure(binary, url, 1, 512, directory_env, temp, transport=True)
                    if srv.requests != 1:
                        raise RuntimeError("directory verification replayed an HTTP request")
                    checks.append(f"{version.name}: hashed directory trusted, exactly one POST")
            with server(cert, key) as (srv, url):
                measure(binary, url.replace("localhost", "127.0.0.1"), 1, 512,
                        directory_env, temp, transport=True, error=True)
                if srv.requests:
                    raise RuntimeError("wrong-host directory check sent request bytes")
                checks.append("hashed directory: wrong hostname rejected before POST")
            print(name, "directory checks:", checks[-3:], flush=True)
        if args.defaults_bin:
            binary = args.defaults_bin.resolve()
            data["defaults_binary"] = {"path": str(binary),
                                       "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
            checks = []
            for mode in ("directory", "incomplete", "malformed", "removed", "empty", "missing", "unhashed",
                         "proxy", "later_proxy", "captured_override"):
                path = temp / f"default-{mode}"
                path.mkdir()
                if mode == "directory":
                    path = system_dir
                elif mode in ("incomplete", "removed"):
                    source = next(p for p in system_dir.iterdir() if re.fullmatch(r"[0-9a-f]{8}\.0", p.name)
                                  and p.read_bytes() != cert.read_bytes())
                    (path / source.name).write_bytes(source.read_bytes())
                elif mode in ("malformed", "proxy", "later_proxy", "captured_override"):
                    (path / "00000000.0").write_text("not a certificate\n")
                elif mode == "unhashed":
                    (path / "local.pem").write_bytes(cert.read_bytes())
                elif mode == "missing":
                    path.rmdir()
                defaults_env = {"PATH": env["PATH"], "LANG": env["LANG"],
                                "PYTTE_TEST_CA_FILE": str(bundle), "PYTTE_TEST_CA_PATH": str(path)}
                if mode == "removed":
                    defaults_env["PYTTE_TEST_REMOVE_CA"] = str(next(path.iterdir()))
                if mode == "proxy":
                    defaults_env.update(https_proxy="http://127.0.0.1:9", NO_PROXY="localhost")
                if mode == "later_proxy":
                    defaults_env["PYTTE_TEST_SET_PROXY"] = "1"
                if mode == "captured_override":
                    defaults_env.update(PYTTE_TEST_CA_FILE=str(ca.resolve()), SSL_CERT_FILE=str(cert),
                                        SSL_CERT_DIR=str(path), PYTTE_TEST_CLEAR_OVERRIDE="1")
                turns = 3 if mode == "incomplete" else 1
                with server(cert, key, reconnect=mode == "incomplete") as (srv, url):
                    measure(binary, url, turns, 512, defaults_env, temp, transport=True)
                    if srv.requests != turns:
                        raise RuntimeError(f"default {mode}: HTTP request was replayed")
                    connections = turns + (mode in ("incomplete", "malformed", "removed"))
                    if srv.connections != connections:
                        raise RuntimeError(f"default {mode}: unexpected TLS attempts ({srv.connections}, expected {connections})")
                    checks.append(f"default {mode}: trusted, {turns} POST(s), {connections} TLS attempt(s)")
            for sent in ("header", "body"):
                guard_env = {"PATH": env["PATH"], "LANG": env["LANG"],
                             "PYTTE_TEST_CA_FILE": str(bundle),
                             "PYTTE_TEST_CA_PATH": str(temp / "default-incomplete"),
                             "PYTTE_TEST_SENT_REQUEST": sent}
                with server(cert, key) as (srv, url):
                    measure(binary, url, 1, 512, guard_env, temp, transport=True, error=True)
                    if srv.connections != 1 or srv.requests:
                        raise RuntimeError(f"sent-{sent} guard retried a transfer")
                    checks.append(f"sent-{sent} guard: no retry after request bytes")
            with server(cert, key) as (srv, url):
                measure(binary, url.replace("localhost", "127.0.0.1"), 1, 512,
                        {"PATH": env["PATH"], "LANG": env["LANG"], "PYTTE_TEST_CA_FILE": str(bundle),
                         "PYTTE_TEST_CA_PATH": str(system_dir)}, temp, transport=True, error=True)
                if srv.requests:
                    raise RuntimeError("default wrong-host check sent request bytes")
                if srv.connections != 2:
                    raise RuntimeError("default wrong-host check did not stop after one fallback")
                checks.append("default directory + bundle: wrong hostname rejected before POST")
            data["checks"]["trust_defaults"] = checks
            print("trust_defaults", checks, flush=True)
        if args.verify_only:
            # A client TLS configuration must retain both certificate families
            # and modern key exchanges, not just the RSA replay's default suite.
            before = {name: len(data["checks"][name]) for name in binaries}
            for curve in ("prime256v1", "secp384r1", "secp521r1"):
                ec_cert, ec_key = temp / f"{curve}.pem", temp / f"{curve}.key"
                subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt",
                                f"ec_paramgen_curve:{curve}", "-nodes", "-days", "2",
                                "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                                "-keyout", str(ec_key), "-out", str(ec_cert)], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                ec_bundle = temp / f"{curve}-bundle.pem"
                ec_bundle.write_bytes(ca.read_bytes() + b"\n" + ec_cert.read_bytes())
                for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
                    with server(ec_cert, ec_key, tls_version=version) as (_, url):
                        for name, binary in binaries.items():
                            measure(binary, url, 1, 512, {**env, "SSL_CERT_FILE": str(ec_bundle)},
                                    temp, transport=True)
                            data["checks"][name].append(f"{version.name}: ECDSA {curve} trusted")
            for cipher in ("ECDHE-RSA-AES128-GCM-SHA256", "ECDHE-RSA-AES256-GCM-SHA384",
                           "ECDHE-RSA-CHACHA20-POLY1305"):
                with server(cert, key, tls_version=ssl.TLSVersion.TLSv1_2, cipher=cipher) as (_, url):
                    for name, binary in binaries.items():
                        measure(binary, url, 1, 512, env, temp, transport=True)
                        data["checks"][name].append(f"TLSv1_2: {cipher}")
            for curve in ("X25519", "prime256v1", "secp384r1", "secp521r1"):
                with server(cert, key, tls_version=ssl.TLSVersion.TLSv1_3, curve=curve) as (_, url):
                    for name, binary in binaries.items():
                        measure(binary, url, 1, 512, env, temp, transport=True)
                        data["checks"][name].append(f"TLSv1_3: {curve} key exchange")
            for name in binaries:
                print(name, "TLS algorithm checks:", data["checks"][name][before[name]:], flush=True)
        if not args.verify_only:
            batches = 1 if args.profile else 3
            for case in args.cases:
                tls, reconnect, size, turns = cases[case]
                data["samples"][case] = {name: [] for name in binaries}
                with server(cert if tls else None, key if tls else None, reconnect, http2=case == "http2") as (srv, url):
                    srv.reply_bytes = size
                    for batch in range(batches):
                        count = args.runs if args.profile else args.runs // 3 + 1
                        for index in range(count):
                            # Alternate order to avoid systematically giving
                            # one build the warmer CPU/filesystem state.
                            order = list(binaries.items())
                            if (batch + index) % 2:
                                order.reverse()
                            for name, binary in order:
                                run_env = env
                                if name in args.directory_build:
                                    run_env = {"PATH": env["PATH"], "LANG": env["LANG"],
                                               "SSL_CERT_DIR": str(system_dir)}
                                value = measure(binary, url, turns, size, run_env, work, args.profile,
                                                transport=args.transport_only)
                                if args.profile or index:
                                    data["samples"][case][name].append(value)
                data["summary"][case] = {}
                for name, runs in data["samples"][case].items():
                    stages = {}
                    for phase, turn in (("startup", 0), ("curl_init", 0), ("response", 1), ("response", turns)):
                        values = [s for run in runs for s in run if s["phase"] == phase and s["turn"] == turn]
                        stages[f"{phase}_{turn}"] = {key: summary([s[key] for s in values]) for key in
                            ("elapsed_ms", "first_text_ms", "rss_kib", "pss_kib", "uss_kib", "anonymous_kib",
                             "core_allocations", "core_reallocations", "core_peak_bytes", "core_live_bytes",
                             "retained_arena_capacity", "retained_buffer_capacity") if key in values[0]}
                    data["summary"][case][name] = stages
                    print(case, name, "first response:", stages["response_1"]["elapsed_ms"],
                          "RSS:", stages["response_1"].get("rss_kib"), flush=True)
                output.write_text(json.dumps(data, indent=2) + "\n")
    output.write_text(json.dumps(data, indent=2) + "\n")


if __name__ == "__main__":
    main()
