#!/usr/bin/env python3
"""Run one macOS app through LaunchServices with bounded, exact cleanup.

``open -W`` waits for an application that LaunchServices starts outside the
caller's process tree.  Killing ``open`` therefore does not stop a hung app.
This helper records processes for the exact bundle executable before launch,
tracks only new processes for that same executable, and owns their shutdown on
timeout, interruption, launch failure, or an unexpected survivor.
"""

from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


POLL_SECONDS = 0.05
TERM_GRACE_SECONDS = 5.0
KILL_GRACE_SECONDS = 2.0


class ProbeInterrupted(Exception):
    """Raised in the main thread when the controlling process is interrupted."""

    def __init__(self, signum: int) -> None:
        super().__init__(signum)
        self.signum = signum


def process_commands() -> dict[int, str]:
    """Return PID -> executable command from the system process table."""
    result = subprocess.run(
        ["/bin/ps", "-ww", "-axo", "pid=,comm="],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    commands: dict[int, str] = {}
    for raw_line in result.stdout.splitlines():
        fields = raw_line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        try:
            commands[int(fields[0])] = fields[1]
        except ValueError:
            continue
    return commands


def processes_for_executable(executable: Path) -> set[int]:
    expected = str(executable)
    return {
        pid for pid, command in process_commands().items()
        if command == expected
    }


def still_exact_process(pid: int, executable: Path) -> bool:
    """Revalidate identity immediately before signalling a PID."""
    return process_commands().get(pid) == str(executable)


def wait_until_gone(pids: set[int], executable: Path, seconds: float) -> set[int]:
    deadline = time.monotonic() + seconds
    remaining = {pid for pid in pids if still_exact_process(pid, executable)}
    while remaining and time.monotonic() < deadline:
        time.sleep(POLL_SECONDS)
        live = processes_for_executable(executable)
        remaining &= live
    return remaining


def signal_exact_processes(
    pids: set[int], executable: Path, signum: signal.Signals
) -> set[int]:
    signalled: set[int] = set()
    for pid in sorted(pids):
        if not still_exact_process(pid, executable):
            continue
        try:
            os.kill(pid, signum)
            signalled.add(pid)
        except ProcessLookupError:
            pass
    return signalled


def terminate_app_processes(pids: set[int], executable: Path) -> set[int]:
    """Terminate exact app processes, escalating only after a bounded grace."""
    term_pids = signal_exact_processes(pids, executable, signal.SIGTERM)
    remaining = wait_until_gone(term_pids, executable, TERM_GRACE_SECONDS)
    if remaining:
        kill_pids = signal_exact_processes(remaining, executable, signal.SIGKILL)
        remaining = wait_until_gone(kill_pids, executable, KILL_GRACE_SECONDS)
    return remaining


def stop_open_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=TERM_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=KILL_GRACE_SECONDS)


def parse_environment(values: list[str]) -> dict[str, str]:
    environment: dict[str, str] = {}
    for value in values:
        name, separator, setting = value.partition("=")
        if not separator or not name or "\0" in value:
            raise ValueError(f"invalid --env value: {value!r}")
        environment[name] = setting
    return environment


def launch(args: argparse.Namespace) -> int:
    app = args.app.resolve(strict=True)
    executable = args.executable.resolve(strict=True)
    if app.suffix != ".app" or not app.is_dir():
        raise ValueError(f"not an app bundle: {app}")
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError(f"bundle executable is not executable: {executable}")
    if app not in executable.parents:
        raise ValueError(f"executable is outside the app bundle: {executable}")

    launch_environment = {
        key: value for key, value in os.environ.items()
        if not key.startswith(("MDKR", "GE007_"))
    }
    requested_environment = parse_environment(args.env)
    launch_environment.update(requested_environment)

    before = processes_for_executable(executable)
    command = [
        "/usr/bin/open", "-n", "-F", "-W",
        "--stdout", str(args.stdout),
        "--stderr", str(args.stderr),
    ]
    for name, value in requested_environment.items():
        command.extend(("--env", f"{name}={value}"))
    command.append(str(app))

    open_process: subprocess.Popen[str] | None = None
    observed: set[int] = set()
    deadline = time.monotonic() + args.timeout
    interrupted_signal: int | None = None
    unexpected_survivors = False

    def interrupt(signum: int, _frame: object) -> None:
        raise ProbeInterrupted(signum)

    previous_handlers = {
        signum: signal.signal(signum, interrupt)
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    }
    try:
        open_process = subprocess.Popen(
            command,
            cwd=args.work_dir,
            env=launch_environment,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        while open_process.poll() is None:
            observed |= processes_for_executable(executable) - before
            if time.monotonic() >= deadline:
                print(
                    f"launchservices_probe: timed out after {args.timeout}s",
                    file=sys.stderr,
                )
                return_code = 124
                break
            time.sleep(POLL_SECONDS)
        else:
            return_code = open_process.returncode or 0
        observed |= processes_for_executable(executable) - before
    except ProbeInterrupted as error:
        interrupted_signal = error.signum
        return_code = 128 + error.signum
    finally:
        # Once teardown starts, finish it even if the job runner repeats a
        # termination signal. Restoring the caller's handlers before cleanup
        # could strand the LaunchServices-owned app between TERM and KILL.
        for signum in previous_handlers:
            signal.signal(signum, signal.SIG_IGN)

        abnormal = interrupted_signal is not None or (
            open_process is not None and open_process.poll() is None
        )
        if abnormal:
            observed |= processes_for_executable(executable) - before
            survivors = terminate_app_processes(observed, executable)
            if open_process is not None:
                stop_open_process(open_process)
        else:
            survivors = processes_for_executable(executable) - before
            if survivors:
                unexpected_survivors = True
                survivors = terminate_app_processes(survivors, executable)

        if survivors:
            print(
                "launchservices_probe: launched app process survived cleanup: "
                + ",".join(str(pid) for pid in sorted(survivors)),
                file=sys.stderr,
            )
            return_code = 1
        elif unexpected_survivors:
            print(
                "launchservices_probe: open returned while the launched app "
                "was still running; cleanup succeeded",
                file=sys.stderr,
            )
            return_code = 1

        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)

    if interrupted_signal is not None:
        print(
            f"launchservices_probe: interrupted by signal {interrupted_signal}",
            file=sys.stderr,
        )
    if not observed:
        print(
            "launchservices_probe: LaunchServices app process was never observed",
            file=sys.stderr,
        )
        return 1
    if open_process is not None and open_process.stdout is not None:
        launcher_output = open_process.stdout.read()
        if launcher_output:
            sys.stderr.write(launcher_output)
    return return_code


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=int, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--stdout", type=Path, required=True)
    parser.add_argument("--stderr", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("app", type=Path)
    args = parser.parse_args()
    if args.timeout < 1 or args.timeout > 600:
        parser.error("--timeout must be between 1 and 600 seconds")
    if not args.work_dir.is_dir():
        parser.error(f"work directory not found: {args.work_dir}")
    try:
        return launch(args)
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"launchservices_probe: FAIL — {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
