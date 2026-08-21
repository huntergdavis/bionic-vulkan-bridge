#!/usr/bin/env python3
import hashlib
import fcntl
import os
import pathlib
import stat
import subprocess
import sys
import tempfile


def write(path: pathlib.Path, content: bytes, mode: int = 0o700) -> None:
    path.write_bytes(content)
    path.chmod(mode)


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_installer(script: pathlib.Path, env: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["bash", str(script)],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10.0,
        check=False,
    )


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_install_steam_icd.py INSTALLER")
    installer = pathlib.Path(sys.argv[1]).resolve()
    real_mv = subprocess.run(
        ["sh", "-c", "command -v mv"],
        text=True,
        stdout=subprocess.PIPE,
        check=True,
    ).stdout.strip()

    with tempfile.TemporaryDirectory(prefix="bvb-installer-") as temporary:
        root = pathlib.Path(temporary)
        steam = root / "steam-arm64"
        source = root / "source"
        tools = root / "tools"
        steam.mkdir()
        source.mkdir()
        tools.mkdir()
        library = source / "libvulkan-bvb-glibc.so"
        service = source / "bvb-bridge-service"
        write(library, b"library-v1\n")
        write(service, b"service-v1\n")
        readelf = tools / "readelf"
        write(
            readelf,
            b"#!/bin/sh\n"
            b"case \"$1\" in\n"
            b"  -h) echo 'Machine: AArch64';;\n"
            b"  -l) case \"$2\" in *bvb-bridge-service) echo '/system/bin/linker64';; esac;;\n"
            b"esac\n",
        )
        env = os.environ.copy()
        env.update(
            {
                "HOME": str(root),
                "STEAM_ARM64_BASE": str(steam),
                "BVB_GLIBC_OUTPUT_DIR": str(source),
                "BVB_SERVICE_BINARY": str(service),
                "PATH": str(tools) + os.pathsep + env["PATH"],
            }
        )

        first = run_installer(installer, env)
        assert first.returncode == 0, first.stderr
        installed = steam / "bvb"
        targets = {
            "library": installed / "lib" / library.name,
            "service": installed / "bin" / service.name,
            "manifest": installed / "icd.d" / "bvb_icd.aarch64.json",
            "stamp": installed / "install.sha256",
        }
        assert targets["library"].read_bytes() == b"library-v1\n"
        assert targets["service"].read_bytes() == b"service-v1\n"
        assert stat.S_IMODE(targets["library"].stat().st_mode) == 0o700
        assert stat.S_IMODE(targets["manifest"].stat().st_mode) == 0o600
        subprocess.run(
            ["sha256sum", "-c", str(targets["stamp"])],
            stdout=subprocess.DEVNULL,
            check=True,
        )

        write(library, b"library-v2\n")
        write(service, b"service-v2\n")
        second = run_installer(installer, env)
        assert second.returncode == 0, second.stderr
        assert targets["library"].read_bytes() == b"library-v2\n"
        assert targets["service"].read_bytes() == b"service-v2\n"
        backups = sorted((installed / "backups").glob("install-pre-*"))
        assert len(backups) == 2
        previous = next(
            path for path in backups
            if (path / "old-libvulkan-bvb-glibc.so").is_file()
        )
        assert (previous / "old-libvulkan-bvb-glibc.so").read_bytes() == b"library-v1\n"
        assert (previous / "old-bvb-bridge-service").read_bytes() == b"service-v1\n"

        baseline = {name: digest(path) for name, path in targets.items()}

        lock_fd = os.open(installed / "backups" / "install.lock", os.O_WRONLY)
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        locked = run_installer(installer, env)
        assert locked.returncode != 0
        assert "another BVB installer is already running" in locked.stderr
        assert {name: digest(path) for name, path in targets.items()} == baseline
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        os.close(lock_fd)

        write(library, b"library-v3-fault\n")
        write(service, b"service-v3-fault\n")
        state = root / "mv-state"
        write(
            tools / "mv",
            (
                "#!/bin/sh\n"
                f"state='{state}'\n"
                "for last do :; done\n"
                f"case \"$last\" in */state) exec '{real_mv}' \"$@\";; esac\n"
                "count=0\n"
                "test ! -f \"$state\" || count=$(cat \"$state\")\n"
                "count=$((count + 1))\n"
                "printf '%s\\n' \"$count\" >\"$state\"\n"
                "if test \"$count\" -eq 2; then exit 73; fi\n"
                f"exec '{real_mv}' \"$@\"\n"
            ).encode(),
        )
        failed = run_installer(installer, env)
        assert failed.returncode != 0
        assert "rollback complete" in failed.stderr
        assert {name: digest(path) for name, path in targets.items()} == baseline
        assert not list(installed.rglob("*.rollback.*"))
        failed_backups = sorted((installed / "backups").glob("install-pre-*"))
        assert len(failed_backups) == 3
        assert all((path / "transaction.txt").is_file() for path in failed_backups)
        assert sorted((path / "state").read_text().strip() for path in failed_backups) == [
            "committed", "committed", "rolled-back",
        ]

        (tools / "mv").unlink()
        state.unlink()
        write(library, b"library-v4-crash\n")
        write(service, b"service-v4-crash\n")
        write(
            tools / "mv",
            (
                "#!/bin/sh\n"
                f"state='{state}'\n"
                "for last do :; done\n"
                f"case \"$last\" in */state) exec '{real_mv}' \"$@\";; esac\n"
                "count=0\n"
                "test ! -f \"$state\" || count=$(cat \"$state\")\n"
                "count=$((count + 1))\n"
                "printf '%s\\n' \"$count\" >\"$state\"\n"
                "if test \"$count\" -eq 1; then\n"
                f"  '{real_mv}' \"$@\" || exit $?\n"
                "  kill -KILL \"$PPID\"\n"
                "  exit 99\n"
                "fi\n"
                f"exec '{real_mv}' \"$@\"\n"
            ).encode(),
        )
        crashed = run_installer(installer, env)
        assert crashed.returncode != 0
        assert targets["library"].read_bytes() == b"library-v4-crash\n"
        assert targets["service"].read_bytes() == b"service-v2\n"

        (tools / "mv").unlink()
        prepared_upgrade = next(
            path
            for path in (installed / "backups").glob("install-pre-*")
            if (path / "state").read_text().strip() == "prepared"
        )
        old_upgrade_library = prepared_upgrade / "old-libvulkan-bvb-glibc.so"
        expected_old_upgrade_library = old_upgrade_library.read_bytes()
        old_upgrade_library.write_bytes(b"tampered-upgrade-backup\n")
        backup_tamper = run_installer(installer, env)
        assert backup_tamper.returncode != 0
        assert "rollback backup identity changed" in backup_tamper.stderr
        assert (prepared_upgrade / "state").read_text().strip() == "prepared"
        old_upgrade_library.write_bytes(expected_old_upgrade_library)
        service.chmod(0o600)
        recovered = run_installer(installer, env)
        assert recovered.returncode != 0
        assert "recovering interrupted transaction" in recovered.stderr
        assert "Bionic service is unavailable or unsafe" in recovered.stderr
        assert {name: digest(path) for name, path in targets.items()} == baseline
        recovered_backups = sorted((installed / "backups").glob("install-pre-*"))
        assert len(recovered_backups) == 4
        assert sum(
            (path / "state").read_text().strip() == "rolled-back"
            for path in recovered_backups
        ) == 2
        service.chmod(0o700)

        first_install_steam = root / "first-install-crash"
        first_install_steam.mkdir()
        first_install_env = dict(env, STEAM_ARM64_BASE=str(first_install_steam))
        state.unlink()
        write(
            tools / "mv",
            (
                "#!/bin/sh\n"
                f"state='{state}'\n"
                "for last do :; done\n"
                f"case \"$last\" in */state) exec '{real_mv}' \"$@\";; esac\n"
                "count=0\n"
                "test ! -f \"$state\" || count=$(cat \"$state\")\n"
                "count=$((count + 1))\n"
                "printf '%s\\n' \"$count\" >\"$state\"\n"
                "if test \"$count\" -eq 2; then exit 73; fi\n"
                "if test \"$count\" -eq 3; then\n"
                f"  '{real_mv}' \"$@\" || exit $?\n"
                "  kill -KILL \"$PPID\"\n"
                "  exit 99\n"
                "fi\n"
                f"exec '{real_mv}' \"$@\"\n"
            ).encode(),
        )
        first_install_crash = run_installer(installer, first_install_env)
        assert first_install_crash.returncode != 0
        first_install_root = first_install_steam / "bvb"
        assert not (first_install_root / "lib" / library.name).exists()
        crash_transactions = list(
            (first_install_root / "backups").glob("install-pre-*")
        )
        assert len(crash_transactions) == 1
        assert (crash_transactions[0] / "state").read_text().strip() == "prepared"
        assert (
            crash_transactions[0] / "failed-libvulkan-bvb-glibc.so"
        ).is_file()

        (tools / "mv").unlink()
        service.chmod(0o600)
        unexpected_service = first_install_root / "bin" / service.name
        unexpected_service.write_bytes(b"unexpected-staged-present-target\n")
        unexpected_service.chmod(0o700)
        staged_present_recovery = run_installer(installer, first_install_env)
        assert staged_present_recovery.returncode != 0
        assert "staged first-install rollback state is invalid" in (
            staged_present_recovery.stderr
        )
        assert (crash_transactions[0] / "state").read_text().strip() == "prepared"
        unexpected_service.unlink()
        failed_library = crash_transactions[0] / "failed-libvulkan-bvb-glibc.so"
        expected_failed_library = failed_library.read_bytes()
        failed_library.write_bytes(b"tampered-between-recovery-attempts\n")
        tampered_recovery = run_installer(installer, first_install_env)
        assert tampered_recovery.returncode != 0
        assert "failed-artifact identity changed" in tampered_recovery.stderr
        assert (crash_transactions[0] / "state").read_text().strip() == "prepared"
        failed_library.write_bytes(expected_failed_library)
        first_install_recovery = run_installer(installer, first_install_env)
        assert first_install_recovery.returncode != 0
        assert "recovering interrupted transaction" in first_install_recovery.stderr
        assert (crash_transactions[0] / "state").read_text().strip() == "rolled-back"
        for relative in (
            pathlib.Path("lib") / library.name,
            pathlib.Path("bin") / service.name,
            pathlib.Path("icd.d") / "bvb_icd.aarch64.json",
            pathlib.Path("install.sha256"),
        ):
            assert not (first_install_root / relative).exists()
        service.chmod(0o700)

        partial = root / "partial-steam"
        partial.mkdir()
        partial_library = partial / "bvb" / "lib"
        partial_library.mkdir(parents=True)
        write(partial_library / library.name, b"partial\n")
        partial_env = dict(env, STEAM_ARM64_BASE=str(partial))
        rejected = run_installer(installer, partial_env)
        assert rejected.returncode != 0
        assert "partial prior BVB installation" in rejected.stderr

    print("PASS: BVB installer stages, backs up, commits, and rolls back exactly")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
