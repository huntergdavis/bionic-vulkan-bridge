#!/usr/bin/env python3

"""Run the installed v40 Activity against real BVB virtual WSI frames.

This is intentionally independent of Steam and Termux:X11.  It verifies that
the installed APK is byte-for-byte the staged versionCode 40 APK containing the
E057 native consumer, proves lifecycle-token rejection, launches the Activity,
and drives the existing global-dispatch WSI smoke client through import and one
native present.  ``--animated-rgbw`` reuses that same v40 Activity and bounded
cleanup path for four E076 red/green/blue/white producer frames. Passing either
mode is not visual pixel, visible-game, or FPS evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import secrets
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any, Sequence


PACKAGE = "io.github.huntergdavis.bvb.visiblehost"
ACTIVITY = f"{PACKAGE}/.VisibleHostActivity"
FRAME_CLIENT = f"{PACKAGE}.FrameTransportClient"
EXPECTED_VERSION_CODE = 40
EXPECTED_BRIDGE_CLIENT_SHA256 = "c0b3dbf36f45bad941a8579bf37bcc8d5773ac7b4d3c0e10a601b58fc4aee3eb"
EXPECTED_BRIDGE_SERVICE_SHA256 = "0917ef33209b0ea32a337de48646908057854f829387671d0a832ec707371241"
EXPECTED_PRIVATE_TURNIP_SHA256 = "8ac6ef78c3c92998aa46c59fd0081edcba82756f5bad561d1b24a57684874a45"
ANDROID_NAMESPACE = "{http://schemas.android.com/apk/res/android}"
LIFECYCLE_MAGIC = 0x314C5642
LIFECYCLE_VERSION = 1
LIFECYCLE_RECORD = struct.Struct("<IHHIIIIQ32s")
LIFECYCLE_ACK = struct.Struct("<IHHIi")
E057_IMPORT_MARKER = "E057_FRAME_TRANSPORT_IMPORTED"
E057_PRESENT_MARKER = "E057_FRAME_PRESENTED"
E076_EXPECTED_MARKER = "E076_FRAME_EXPECTED"
NATIVE_LIBRARY = "lib/arm64-v8a/libbvb-visible-host.so"


class GateFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class ApkIdentity:
    path: pathlib.Path
    package: str
    version_code: int
    version_name: str
    sha256: str
    certificate_sha256: str


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def resolve_executable(command: str) -> str:
    candidate = pathlib.Path(command)
    resolved = str(candidate.resolve(strict=True)) if candidate.is_absolute() else shutil.which(command)
    if resolved is None or not os.access(resolved, os.X_OK):
        raise GateFailure(f"required executable is unavailable: {command}")
    return resolved


def resolve_regular_file(path: pathlib.Path, *, executable: bool = False) -> pathlib.Path:
    if path.is_symlink():
        raise GateFailure(f"path must not be a symlink: {path}")
    resolved = path.resolve(strict=True)
    if not resolved.is_file() or (executable and not os.access(resolved, os.X_OK)):
        qualification = "executable" if executable else "regular"
        raise GateFailure(f"path is not a {qualification} file: {path}")
    return resolved


def run_text(
    command: Sequence[str],
    *,
    timeout: float,
    environment: dict[str, str] | None = None,
    check: bool = True,
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        list(command),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        env=environment,
        check=False,
    )
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip() or "no diagnostic"
        raise GateFailure(f"command failed ({result.returncode}): {detail}")
    return result


def parse_badging(output: str) -> tuple[str, int, str]:
    first_line = output.splitlines()[0] if output.splitlines() else ""
    package_match = re.search(r"\bname='([^']+)'", first_line)
    version_match = re.search(r"\bversionCode='([0-9]+)'", first_line)
    name_match = re.search(r"\bversionName='([^']*)'", first_line)
    if package_match is None or version_match is None or name_match is None:
        raise GateFailure("aapt did not return a complete APK package identity")
    return package_match.group(1), int(version_match.group(1)), name_match.group(1)


def parse_certificate(output: str) -> str:
    match = re.search(
        r"certificate SHA-256 digest:\s*([0-9a-fA-F]{64})", output
    )
    if match is None:
        raise GateFailure("apksigner did not return a certificate SHA-256 digest")
    return match.group(1).lower()


def require_e057_payload(apk: pathlib.Path) -> None:
    try:
        with zipfile.ZipFile(apk) as archive:
            names = set(archive.namelist())
            if NATIVE_LIBRARY not in names or "classes.dex" not in names:
                raise GateFailure("APK is missing the native host or FrameTransportClient dex")
            native = archive.read(NATIVE_LIBRARY)
    except (OSError, zipfile.BadZipFile) as error:
        raise GateFailure(f"could not inspect APK payload: {error}") from error
    for marker in (E057_IMPORT_MARKER, E057_PRESENT_MARKER):
        if marker.encode("ascii") not in native:
            raise GateFailure(f"APK native library is missing {marker}; refusing pre-v40 payload")


def inspect_apk(
    apk: pathlib.Path, aapt: str, apksigner: str, timeout: float
) -> ApkIdentity:
    resolved = apk.resolve(strict=True)
    if not resolved.is_file() or apk.is_symlink() or not os.access(resolved, os.R_OK):
        raise GateFailure(f"APK path is not a readable regular non-symlink: {apk}")
    package, version_code, version_name = parse_badging(
        run_text([aapt, "dump", "badging", str(resolved)], timeout=timeout).stdout
    )
    certificate = parse_certificate(
        run_text(
            [apksigner, "verify", "--verbose", "--print-certs", str(resolved)],
            timeout=timeout,
        ).stdout
    )
    return ApkIdentity(
        path=resolved,
        package=package,
        version_code=version_code,
        version_name=version_name,
        sha256=sha256_file(resolved),
        certificate_sha256=certificate,
    )


def validate_apk_pair(
    manifest: pathlib.Path,
    staged_apk: pathlib.Path,
    installed_apk: pathlib.Path,
    aapt: str,
    apksigner: str,
    timeout: float,
) -> tuple[ApkIdentity, ApkIdentity]:
    try:
        root = ET.parse(manifest.resolve(strict=True)).getroot()
        manifest_package = root.attrib["package"]
        manifest_version = int(root.attrib[f"{ANDROID_NAMESPACE}versionCode"])
    except (OSError, ET.ParseError, KeyError, ValueError) as error:
        raise GateFailure(f"could not read source manifest identity: {error}") from error
    if manifest_package != PACKAGE or manifest_version != EXPECTED_VERSION_CODE:
        raise GateFailure(
            f"source manifest is not exact v40: package={manifest_package} "
            f"versionCode={manifest_version}; versionCode 39 is explicitly refused"
        )
    staged = inspect_apk(staged_apk, aapt, apksigner, timeout)
    installed = inspect_apk(installed_apk, aapt, apksigner, timeout)
    for label, identity in (("staged", staged), ("installed", installed)):
        if identity.package != PACKAGE or identity.version_code != EXPECTED_VERSION_CODE:
            raise GateFailure(
                f"{label} APK is not exact v40: package={identity.package} "
                f"versionCode={identity.version_code}; versionCode 39 is explicitly refused"
            )
    require_e057_payload(staged.path)
    if staged.sha256 != installed.sha256:
        raise GateFailure("installed APK is not byte-identical to the staged v40 APK")
    if staged.certificate_sha256 != installed.certificate_sha256:
        raise GateFailure("installed and staged APK certificates do not match")
    return staged, installed


def resolve_installed_apk(pm: str, timeout: float) -> pathlib.Path:
    output = run_text([pm, "path", PACKAGE], timeout=timeout).stdout
    paths = [line.removeprefix("package:") for line in output.splitlines() if line.startswith("package:")]
    if len(paths) != 1 or not paths[0].startswith("/"):
        raise GateFailure(f"expected exactly one installed base APK, found {len(paths)}")
    return pathlib.Path(paths[0])


def receive_exact(channel: socket.socket, length: int) -> bytes:
    output = bytearray()
    while len(output) < length:
        chunk = channel.recv(length - len(output))
        if not chunk:
            raise GateFailure("lifecycle ingress closed before its fixed ACK")
        output.extend(chunk)
    return bytes(output)


def prove_wrong_token_rejection(port: int, timeout: float) -> int:
    record = LIFECYCLE_RECORD.pack(
        LIFECYCLE_MAGIC,
        LIFECYCLE_VERSION,
        1,
        1,
        0,
        0,
        os.getpid(),
        time.monotonic_ns(),
        bytes.fromhex("ff" * 32),
    )
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as channel:
        channel.sendall(record)
        acknowledgement = receive_exact(channel, LIFECYCLE_ACK.size)
    magic, version, reserved, sequence, status_code = LIFECYCLE_ACK.unpack(acknowledgement)
    if (magic, version, reserved, sequence, status_code) != (
        LIFECYCLE_MAGIC,
        LIFECYCLE_VERSION,
        0,
        1,
        -13,
    ):
        raise GateFailure(
            "wrong lifecycle capability was not rejected with authenticated EACCES"
        )
    return status_code


def parse_activity_events(service_log: pathlib.Path) -> list[dict[str, int]]:
    pattern = re.compile(
        r"activity_event=(\d+) sequence=(\d+) pid=(\d+) "
        r"width=(\d+) height=(\d+)"
    )
    text = service_log.read_text(errors="replace") if service_log.exists() else ""
    return [
        {
            "event": int(match.group(1)),
            "sequence": int(match.group(2)),
            "pid": int(match.group(3)),
            "width": int(match.group(4)),
            "height": int(match.group(5)),
        }
        for match in pattern.finditer(text)
    ]


def wait_for_service(
    process: subprocess.Popen[Any],
    service_log: pathlib.Path,
    control_socket: pathlib.Path,
    timeout: float,
) -> int:
    deadline = time.monotonic() + timeout
    pattern = re.compile(r"activity_port=([0-9]+)$", re.MULTILINE)
    while time.monotonic() < deadline:
        text = service_log.read_text(errors="replace") if service_log.exists() else ""
        matches = pattern.findall(text)
        if matches and control_socket.exists():
            port = int(matches[-1])
            if not 1 <= port <= 65535 or not stat.S_ISSOCK(control_socket.stat().st_mode):
                raise GateFailure("service published an invalid Activity port or control path")
            return port
        if process.poll() is not None:
            raise GateFailure(f"bridge service exited before readiness: {process.returncode}")
        time.sleep(0.02)
    raise GateFailure("timed out waiting for bridge service readiness")


def wait_for_renderer(
    process: subprocess.Popen[Any], service_log: pathlib.Path, timeout: float
) -> tuple[int, int, int, list[dict[str, int]]]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        events = parse_activity_events(service_log)
        if any(record["event"] == 12 for record in events):
            raise GateFailure("Activity reported renderer failure")
        ready = [
            record
            for record in events
            if record["event"] == 11 and record["width"] > 0 and record["height"] > 0
        ]
        codes = {record["event"] for record in events}
        if ready and {1, 2, 3, 7, 11}.issubset(codes):
            record = ready[-1]
            return record["pid"], record["width"], record["height"], events
        if process.poll() is not None:
            raise GateFailure("bridge service exited before renderer readiness")
        time.sleep(0.02)
    raise GateFailure("timed out waiting for authenticated Activity renderer readiness")


def wait_for_abstract_socket(
    process: subprocess.Popen[Any], socket_name: str, proc_net_unix: pathlib.Path, timeout: float
) -> None:
    deadline = time.monotonic() + timeout
    marker = f"@{socket_name}"
    while time.monotonic() < deadline:
        text = proc_net_unix.read_text(errors="replace")
        if marker in text:
            return
        if process.poll() is not None:
            raise GateFailure(
                f"FrameTransportClient exited before setup listener readiness: {process.returncode}"
            )
        time.sleep(0.02)
    raise GateFailure("timed out waiting for same-UID frame setup listener")


def wait_process(process: subprocess.Popen[Any], label: str, timeout: float) -> int:
    try:
        status = process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise GateFailure(f"{label} timed out") from error
    if status != 0:
        raise GateFailure(f"{label} exited {status}")
    return status


def stop_process(process: subprocess.Popen[Any] | None, timeout: float = 2.0) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=timeout)


def artifact(path: pathlib.Path) -> dict[str, Any]:
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": sha256_file(path)}


def require_artifact_sha256(
    path: pathlib.Path, expected: str, label: str, *, executable: bool = False
) -> pathlib.Path:
    resolved = resolve_regular_file(path, executable=executable)
    actual = sha256_file(resolved)
    if actual != expected:
        raise GateFailure(
            f"{label} SHA-256 mismatch: actual={actual} expected={expected}"
        )
    return resolved


def prepare_artifacts(
    project: pathlib.Path,
    build_log: pathlib.Path,
    timeout: float,
) -> pathlib.Path:
    build_dir = project / "build"
    output = project / "out" / "triangle-dispatch-glibc"
    library = output / "libvulkan-bvb-glibc.so"
    client = output / "bvb-global-dispatch-test-glibc"
    headers = build_dir / "_deps" / "vulkanheaders-src" / "include"
    commands = [
        [str(project / "scripts" / "test-triangle-dispatch-glibc-termux.sh")],
        [
            "grun", "-s", "gcc", "-std=c17", "-O3", "-DNDEBUG",
            "-Wall", "-Wextra", "-Werror", f"-I{project / 'include'}",
            f"-I{headers}", str(project / "tests" / "global_dispatch.c"),
            str(project / "src" / "handle.c"), f"-L{output}",
            f"-Wl,-rpath,{output}", "-lvulkan-bvb-glibc", "-o", str(client),
        ],
    ]
    with build_log.open("w") as log:
        for command in commands:
            resolved = [resolve_executable(command[0]), *command[1:]]
            result = subprocess.run(
                resolved, stdout=log, stderr=subprocess.STDOUT,
                timeout=max(timeout, 300.0), check=False,
            )
            if result.returncode != 0:
                raise GateFailure(
                    f"runtime artifact preparation failed ({result.returncode}); see {build_log}"
                )
    for path in (library, client):
        if not path.is_file() or path.is_symlink():
            raise GateFailure(f"runtime artifact is unavailable or unsafe: {path}")
    return client.resolve(strict=True)


def validate_runtime_client(client: pathlib.Path, animated_rgbw: bool = False) -> None:
    if not client.is_file() or client.is_symlink() or not os.access(client, os.X_OK):
        raise GateFailure(f"global WSI client is unavailable or unsafe: {client}")
    content = client.read_bytes()
    for variable in (
        b"BVB_GLOBAL_DISPATCH_WSI_WIDTH",
        b"BVB_GLOBAL_DISPATCH_WSI_HEIGHT",
        b"BVB_GLOBAL_DISPATCH_PRESENT_HOLD_MS",
    ):
        if variable not in content:
            raise GateFailure("global WSI client is stale; rerun without --skip-build")
    if animated_rgbw:
        for marker in (b"BVB_TEST_ANIMATED_WSI", E076_EXPECTED_MARKER.encode()):
            if marker not in content:
                raise GateFailure(
                    "global WSI client lacks E076 animation support; rerun without --skip-build"
                )


def parse_arguments() -> argparse.Namespace:
    project = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--staged-apk",
        default=str(project / "out" / "visible-host" / "bvb-visible-host-debug.apk"),
    )
    parser.add_argument("--service-loader", default=str(pathlib.Path.home() / "steam-arm64" / "bvb" / "driver" / "libvulkan_freedreno.so"))
    parser.add_argument("--service", default=str(pathlib.Path.home() / "steam-arm64" / "bvb" / "bin" / "bvb-bridge-service"))
    parser.add_argument("--bridge-client", default=str(pathlib.Path.home() / "steam-arm64" / "bvb" / "bin" / "bvb-bridge-client"))
    parser.add_argument("--client")
    parser.add_argument("--output-root", default=str(project / "out" / "activity-frame-v40"))
    parser.add_argument("--runtime-parent", default=os.environ.get("TMPDIR", "/tmp"))
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--present-hold-ms", type=int, default=5000)
    parser.add_argument("--animated-rgbw", action="store_true")
    parser.add_argument(
        "--expected-service-sha256",
        help=(
            "required with --animated-rgbw; exact SHA-256 of the deployed "
            "E076-or-newer bridge service selected for this isolated run"
        ),
    )
    parser.add_argument(
        "--expected-client-sha256",
        help=(
            "required with --animated-rgbw; exact SHA-256 of the E076-or-newer "
            "global-dispatch producer client"
        ),
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--preflight-only", action="store_true")
    parser.add_argument("--am", default=os.environ.get("BVB_ACTIVITY_LAUNCHER", "am"))
    parser.add_argument("--pm", default=os.environ.get("BVB_PACKAGE_MANAGER", "pm"))
    parser.add_argument("--aapt", default=os.environ.get("BVB_AAPT", "aapt"))
    parser.add_argument("--apksigner", default=os.environ.get("BVB_APKSIGNER", "apksigner"))
    parser.add_argument("--app-process", default=os.environ.get("BVB_APP_PROCESS", "/system/bin/app_process"))
    parser.add_argument("--logcat", default=os.environ.get("BVB_LOGCAT", "logcat"))
    parser.add_argument("--pidof", default=os.environ.get("BVB_PIDOF", "pidof"))
    parser.add_argument("--grun", default=os.environ.get("BVB_GRUN", "grun"))
    parser.add_argument("--proc-net-unix", default="/proc/net/unix")
    arguments = parser.parse_args()
    if arguments.timeout <= 0:
        parser.error("--timeout must be positive")
    if not 1 <= arguments.present_hold_ms <= 30000:
        parser.error("--present-hold-ms must be between 1 and 30000")
    for label, value in (
        ("--expected-service-sha256", arguments.expected_service_sha256),
        ("--expected-client-sha256", arguments.expected_client_sha256),
    ):
        if value is not None and re.fullmatch(r"[0-9a-f]{64}", value) is None:
            parser.error(f"{label} must be 64 lowercase hex digits")
    if arguments.animated_rgbw and (
        arguments.expected_service_sha256 is None
        or arguments.expected_client_sha256 is None
    ):
        parser.error(
            "--animated-rgbw requires --expected-service-sha256 and "
            "--expected-client-sha256"
        )
    arguments.project = project
    return arguments


def run(arguments: argparse.Namespace) -> int:
    output_candidate = pathlib.Path(arguments.output_root)
    if output_candidate.is_symlink():
        raise GateFailure(f"output root must not be a symlink: {output_candidate}")
    output_root = output_candidate.resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-{os.getpid()}"
    run_directory = output_root / run_id
    run_directory.mkdir(mode=0o700)
    evidence_path = run_directory / "evidence.json"
    service_stdout = run_directory / "service.stdout"
    service_stderr = run_directory / "service.stderr"
    activity_launch = run_directory / "activity-launch.txt"
    activity_log = run_directory / "activity.logcat"
    helper_stdout = run_directory / "frame-helper.stdout"
    helper_stderr = run_directory / "frame-helper.stderr"
    helper_result = run_directory / "frame-helper.json"
    client_stdout = run_directory / "global-client.stdout"
    client_stderr = run_directory / "global-client.stderr"
    build_log = run_directory / "build.log"
    cleanup_log = run_directory / "cleanup.log"
    result: dict[str, Any] = {
        "schema_version": 1,
        "gate": (
            "E076-rich-frame-animation-v40-runtime"
            if arguments.animated_rgbw
            else "E074-activity-frame-import-v40-runtime"
        ),
        "result": "fail",
        "expected_version_code": EXPECTED_VERSION_CODE,
        "steam_started": False,
        "termux_x11_started": False,
        "visible_game_claim": False,
        "fps_claim": False,
        "visual_confirmation": {
            "required": arguments.animated_rgbw,
            "status": "pending" if arguments.animated_rgbw else "not_applicable",
            "screenshot": None,
        },
        "visibility_boundary": (
            "pending: requires authenticated Activity import and native present markers; "
            "never claims visually inspected or game-produced pixels"
        ),
        "logs": {"run_directory": str(run_directory)},
    }
    service_process: subprocess.Popen[Any] | None = None
    helper_process: subprocess.Popen[Any] | None = None
    client_process: subprocess.Popen[Any] | None = None
    logcat_process: subprocess.Popen[Any] | None = None
    activity_started = False
    runtime_directory: pathlib.Path | None = None
    open_handles: list[Any] = []
    failure: BaseException | None = None
    sensitive_values: list[str] = []
    am = pm = aapt = apksigner = app_process = logcat = pidof = grun = ""

    try:
        pm = resolve_executable(arguments.pm)
        aapt = resolve_executable(arguments.aapt)
        apksigner = resolve_executable(arguments.apksigner)
        installed_apk = resolve_installed_apk(pm, arguments.timeout)
        staged, installed = validate_apk_pair(
            arguments.project / "android" / "visible-host" / "AndroidManifest.xml",
            pathlib.Path(arguments.staged_apk), installed_apk,
            aapt, apksigner, arguments.timeout,
        )
        result["apk_identity"] = {
            "package": PACKAGE,
            "version_code": EXPECTED_VERSION_CODE,
            "version_name": staged.version_name,
            "staged": artifact(staged.path),
            "installed": artifact(installed.path),
            "byte_identical": True,
            "certificate_sha256": staged.certificate_sha256,
            "e057_markers_embedded": [E057_IMPORT_MARKER, E057_PRESENT_MARKER],
        }
        if arguments.preflight_only:
            result["result"] = "preflight_pass"
            result["visibility_boundary"] = "APK identity only; Activity was not launched"
            print(
                f"activity_frame_import_v40=preflight_pass evidence={evidence_path} "
                "visible_game_claim=false fps_claim=false"
            )
            return 0

        am = resolve_executable(arguments.am)
        app_process = resolve_executable(arguments.app_process)
        logcat = resolve_executable(arguments.logcat)
        pidof = resolve_executable(arguments.pidof)
        grun = resolve_executable(arguments.grun)

        running = run_text([pidof, PACKAGE], timeout=5.0, check=False)
        if running.stdout.strip():
            raise GateFailure(
                f"refusing to disturb an existing {PACKAGE} process: {running.stdout.strip()}"
            )
        loader = require_artifact_sha256(
            pathlib.Path(arguments.service_loader), EXPECTED_PRIVATE_TURNIP_SHA256,
            "installed private Turnip",
        )
        service = require_artifact_sha256(
            pathlib.Path(arguments.service),
            arguments.expected_service_sha256 or EXPECTED_BRIDGE_SERVICE_SHA256,
            "selected bridge service", executable=True,
        )
        installed_bridge_client = require_artifact_sha256(
            pathlib.Path(arguments.bridge_client), EXPECTED_BRIDGE_CLIENT_SHA256,
            "installed bridge client", executable=True,
        )

        if arguments.skip_build:
            if arguments.client is None:
                raise GateFailure("--skip-build requires explicit --client")
            client = resolve_regular_file(
                pathlib.Path(arguments.client), executable=True
            )
        else:
            client = prepare_artifacts(
                arguments.project, build_log, arguments.timeout
            )
        if not service.is_file() or service.is_symlink() or not os.access(service, os.X_OK):
            raise GateFailure(f"bridge service is unavailable or unsafe: {service}")
        validate_runtime_client(client, arguments.animated_rgbw)
        if arguments.animated_rgbw:
            client = require_artifact_sha256(
                client, arguments.expected_client_sha256,
                "E076-or-newer global-dispatch producer client", executable=True,
            )
        result["artifacts"] = {
            "service": artifact(service),
            "installed_bridge_client": artifact(installed_bridge_client),
            "client": artifact(client),
            "service_loader": artifact(loader),
        }

        runtime_parent_candidate = pathlib.Path(arguments.runtime_parent)
        if runtime_parent_candidate.is_symlink():
            raise GateFailure(f"runtime parent must not be a symlink: {runtime_parent_candidate}")
        runtime_parent = runtime_parent_candidate.resolve(strict=True)
        if not runtime_parent.is_dir():
            raise GateFailure(f"unsafe runtime parent: {runtime_parent}")
        runtime_directory = pathlib.Path(tempfile.mkdtemp(prefix="bvb-v40-", dir=runtime_parent))
        control_socket = runtime_directory / "bridge.sock"
        if len(os.fsencode(control_socket)) >= 108:
            raise GateFailure("runtime control socket exceeds Unix sun_path limit")
        frame_socket = f"bvb-v40-frame-{os.getpid()}-{secrets.token_hex(8)}"
        token = secrets.token_hex(32)
        sensitive_values.append(token)

        service_out_handle = service_stdout.open("wb")
        service_err_handle = service_stderr.open("wb")
        open_handles.extend((service_out_handle, service_err_handle))
        service_process = subprocess.Popen(
            [
                str(service), "--socket", str(control_socket), "--once",
                "--loader", str(loader), "--activity-port", "0",
                "--activity-token", token,
                "--activity-frame-socket", frame_socket,
            ],
            stdout=service_out_handle,
            stderr=service_err_handle,
        )
        port = wait_for_service(
            service_process, service_stdout, control_socket, arguments.timeout
        )
        rejected_status = prove_wrong_token_rejection(port, min(arguments.timeout, 5.0))

        launch_result = run_text(
            [
                am, "start", "--user", "0", "-W", "-n", ACTIVITY,
                "--ei", "bvb_activity_port", str(port),
                "--es", "bvb_activity_token", token,
                "--ei", "bvb_retain_external_renderer", "1",
                "--ei", "bvb_visible_frames",
                "4" if arguments.animated_rgbw else "1",
            ],
            timeout=arguments.timeout,
        )
        launch_text = launch_result.stdout + launch_result.stderr
        for sensitive in sensitive_values:
            launch_text = launch_text.replace(sensitive, "<redacted>")
        activity_launch.write_text(launch_text)
        activity_started = True
        activity_pid, width, height, events = wait_for_renderer(
            service_process, service_stdout, arguments.timeout
        )
        running_after_launch = run_text([pidof, PACKAGE], timeout=5.0, check=False)
        if str(activity_pid) not in running_after_launch.stdout.split():
            raise GateFailure("authenticated Activity PID does not match installed package process")

        activity_log_handle = activity_log.open("wb")
        open_handles.append(activity_log_handle)
        logcat_process = subprocess.Popen(
            [logcat, "--pid", str(activity_pid), "-v", "threadtime"],
            stdout=activity_log_handle,
            stderr=subprocess.STDOUT,
        )
        helper_out_handle = helper_stdout.open("wb")
        helper_err_handle = helper_stderr.open("wb")
        open_handles.extend((helper_out_handle, helper_err_handle))
        helper_environment = os.environ.copy()
        helper_environment.pop("LD_LIBRARY_PATH", None)
        helper_environment.pop("LD_PRELOAD", None)
        helper_environment["CLASSPATH"] = str(installed.path)
        helper_process = subprocess.Popen(
            [
                app_process, "-Xnoimage-dex2oat", "/", FRAME_CLIENT,
                token, str(helper_result), frame_socket,
            ],
            stdout=helper_out_handle,
            stderr=helper_err_handle,
            env=helper_environment,
        )
        wait_for_abstract_socket(
            helper_process, frame_socket, pathlib.Path(arguments.proc_net_unix),
            min(arguments.timeout, 15.0),
        )

        client_out_handle = client_stdout.open("wb")
        client_err_handle = client_stderr.open("wb")
        open_handles.extend((client_out_handle, client_err_handle))
        client_environment = os.environ.copy()
        client_environment["BVB_BRIDGE_SOCKET"] = str(control_socket)
        client_environment["BVB_GLOBAL_DISPATCH_HARDWARE"] = "1"
        client_environment["BVB_GLOBAL_DISPATCH_WSI_WIDTH"] = str(width)
        client_environment["BVB_GLOBAL_DISPATCH_WSI_HEIGHT"] = str(height)
        client_environment["BVB_GLOBAL_DISPATCH_PRESENT_HOLD_MS"] = str(
            arguments.present_hold_ms
        )
        if arguments.animated_rgbw:
            client_environment["BVB_COMMAND_STREAM"] = "shared"
            client_environment["BVB_TEST_ANIMATED_WSI"] = "1"
        client_process = subprocess.Popen(
            [grun, str(client)],
            stdout=client_out_handle,
            stderr=client_err_handle,
            env=client_environment,
        )
        wait_process(client_process, "global WSI client", arguments.timeout)
        wait_process(helper_process, "FrameTransportClient", min(arguments.timeout, 20.0))
        wait_process(service_process, "bridge service", min(arguments.timeout, 20.0))
        time.sleep(0.1)
        stop_process(logcat_process)
        logcat_process = None
        for handle in open_handles:
            handle.flush()

        frame_document = json.loads(helper_result.read_text())
        if frame_document.get("result") != "pass":
            raise GateFailure(f"Activity frame helper did not import: {frame_document}")
        client_text = client_stdout.read_text(errors="replace")
        if not client_text.startswith("PASS: global Vulkan discovery validation_mode=hardware"):
            raise GateFailure("global WSI client did not report its hardware PASS record")
        for label, path in (
            ("bridge service", service_stderr),
            ("FrameTransportClient", helper_stderr),
            ("global WSI client", client_stderr),
        ):
            if path.stat().st_size != 0:
                raise GateFailure(f"{label} emitted stderr; see {path}")
        app_text = activity_log.read_text(errors="replace")
        import_match = re.search(
            rf"{E057_IMPORT_MARKER} generation=(\d+) images=(\d+) "
            rf"width=(\d+) height=(\d+) format=(-?\d+)", app_text,
        )
        present_matches = list(re.finditer(
            rf"{E057_PRESENT_MARKER} generation=(\d+) sequence=(\d+) slot=(\d+)",
            app_text,
        ))
        expected_matches = list(re.finditer(
            rf"{E076_EXPECTED_MARKER} sequence=(\d+) color=(\w+) slot=(\d+)",
            client_text,
        ))
        required_present_count = 4 if arguments.animated_rgbw else 1
        if import_match is None or len(present_matches) < required_present_count:
            raise GateFailure("Activity log is missing E057 import or present completion marker")
        import_generation = int(import_match.group(1))
        selected_presents = present_matches[:required_present_count]
        present_match = selected_presents[0]
        present_generation = int(present_match.group(1))
        if (
            import_generation != int(frame_document["generation"])
            or present_generation != import_generation
            or int(present_match.group(2)) < 1
            or int(import_match.group(3)) != width
            or int(import_match.group(4)) != height
        ):
            raise GateFailure("E057 import/present metadata does not match the setup bundle")
        correlations: list[dict[str, Any]] = []
        if arguments.animated_rgbw:
            expected_colors = ["red", "green", "blue", "white"]
            if len(expected_matches) != 4:
                raise GateFailure("producer log is missing four E076 expected-color markers")
            for index, (expected, presented) in enumerate(
                zip(expected_matches, selected_presents), start=1
            ):
                sequence = int(expected.group(1))
                expected_slot = int(expected.group(3))
                if (
                    sequence != index
                    or expected.group(2) != expected_colors[index - 1]
                    or int(presented.group(1)) != import_generation
                    or int(presented.group(2)) != sequence
                    or int(presented.group(3)) != expected_slot
                ):
                    raise GateFailure(
                        "E076 producer expectation does not correlate with "
                        "E057_FRAME_PRESENTED generation/sequence/slot"
                    )
                correlations.append(
                    {
                        "generation": import_generation,
                        "sequence": sequence,
                        "expected_color": expected.group(2),
                        "producer_slot": expected_slot,
                        "activity_presented_slot": int(presented.group(3)),
                        "producer_marker": expected.group(0),
                        "activity_marker": presented.group(0),
                    }
                )
        first_failure = app_text.find("E057_FRAME_CONSUMER_FAIL")
        first_present = app_text.find(E057_PRESENT_MARKER)
        if first_failure >= 0 and first_failure < first_present:
            raise GateFailure("E057 consumer failed before its first native present")

        result.update(
            {
                "result": "pass",
                "authentication": {
                    "wrong_token_status": rejected_status,
                    "valid_lifecycle_events": events,
                    "required_events": [1, 2, 3, 7, 11],
                    "activity_pid": activity_pid,
                },
                "activity_configuration": {
                    "retain_external_renderer": True,
                    "width": width,
                    "height": height,
                },
                "frame_transport": {
                    "helper": frame_document,
                    "import_marker": import_match.group(0),
                    "present_marker": present_match.group(0),
                    "present_markers": [
                        match.group(0) for match in selected_presents
                    ],
                    "expected_frame_correlations": correlations,
                    "native_import": True,
                    "native_present": True,
                    "per_frame_java_calls": 0,
                    "per_frame_binder_calls": 0,
                },
                "visibility_boundary": (
                    "the installed v40 Activity authenticated, imported the one-time "
                    "image/control FD bundle, and completed correlated native presents; "
                    "RGBW producer metadata still requires user visual confirmation and "
                    "a screenshot before any changing-pixel claim; no Tomb Raider output "
                    "or FPS is claimed"
                ),
            }
        )
    except BaseException as error:
        detail = str(error)
        for sensitive in sensitive_values:
            detail = detail.replace(sensitive, "<redacted>")
        failure = GateFailure(detail)
        result["error"] = detail
    finally:
        cleanup_errors: list[str] = []
        for label, process in (
            ("global client", client_process),
            ("frame helper", helper_process),
            ("bridge service", service_process),
            ("logcat", logcat_process),
        ):
            try:
                stop_process(process)
            except (OSError, subprocess.SubprocessError) as error:
                cleanup_errors.append(f"{label}: {error}")
        for handle in open_handles:
            try:
                handle.close()
            except OSError:
                pass
        if activity_started and am:
            try:
                cleanup = run_text(
                    [am, "force-stop", "--user", "0", PACKAGE],
                    timeout=5.0, check=False,
                )
                cleanup_log.write_text(cleanup.stdout + cleanup.stderr)
                if cleanup.returncode != 0:
                    cleanup_errors.append(
                        f"Activity force-stop exited {cleanup.returncode}"
                    )
            except (OSError, subprocess.SubprocessError) as error:
                cleanup_errors.append(f"Activity force-stop: {error}")
        if runtime_directory is not None and runtime_directory.exists():
            for child in list(runtime_directory.iterdir()):
                if child.is_socket():
                    child.unlink()
                else:
                    failure = failure or GateFailure(
                        f"refusing to remove unexpected runtime path: {child}"
                    )
            try:
                runtime_directory.rmdir()
            except OSError as error:
                failure = failure or GateFailure(f"runtime cleanup failed: {error}")
        if cleanup_errors:
            cleanup_detail = "; ".join(cleanup_errors)
            failure = failure or GateFailure(f"bounded cleanup failed: {cleanup_detail}")
        result["logs"].update(
            {
                "service_stdout": str(service_stdout),
                "service_stderr": str(service_stderr),
                "activity_launch": str(activity_launch),
                "activity_logcat": str(activity_log),
                "frame_helper_stdout": str(helper_stdout),
                "frame_helper_stderr": str(helper_stderr),
                "global_client_stdout": str(client_stdout),
                "global_client_stderr": str(client_stderr),
                "build": str(build_log),
                "cleanup": str(cleanup_log),
            }
        )
        if failure is not None:
            result["result"] = "fail"
            result["error"] = str(failure)
        evidence_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")

    print(
        f"activity_frame_import_v40={result['result']} evidence={evidence_path} "
        "visible_game_claim=false fps_claim=false"
    )
    if arguments.animated_rgbw and failure is None:
        print(
            "E076_VISUAL_CONFIRMATION_REQUIRED: verify red/green/blue/white "
            "on the tablet and capture a screenshot; metadata correlation "
            "alone is not pixel proof"
        )
    if failure is not None:
        print(f"activity frame import v40 gate: {failure}", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    try:
        return run(parse_arguments())
    except (GateFailure, OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        print(f"activity frame import v40 gate: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
