#!/usr/bin/env python3
"""Source gate: a check that scrubs MDKR* out of its engine environment must
put MDKR_SAVE_DIR back before it hands that environment to a process.

MECHANISM. Most behavioural checks build a "hermetic" engine environment by
dropping every inherited MDKR* variable. That scrub also drops the two
variables tools/run_checks.py exports per task: MDKR_SAVE_DIR and
MDKR_VIDEO_CONFIG_PATH. Since issue #54 unified save resolution, a
non-packaged build that sees no MDKR_SAVE_DIR no longer resolves saves to
$CWD/save -- it resolves them to the SHARED per-user directory
(~/Library/Application Support/mdkr64/mdkr64/save on macOS,
$XDG_DATA_HOME/mdkr64/save on Linux). Any adventure-in-progress EEPROM sitting
there -- left by a developer playing the game, or by an earlier suite task --
then re-routes the boot flow of every such check: FILE SELECT resumes instead
of starting a new game, the intro cutscene is skipped, levels load thousands of
frames early, frame-timed track-select inputs miss, and seeded unlock state is
ignored because the engine never reads the seed the check wrote. The failures
read as product regressions ("level never loaded ... no [PVEH]", "0 flap cues",
"trophies 0x0", "capture-window positions=0") and are entirely an artefact of
the host's shared directory.

The 1.6.0 fixture-remints wave fixed 22 lanes of this by hand and recorded that
the class is systemic: an aggregate like check_full_ubsan or check_native_layout
scrubs once and then hands that environment to a dozen SUB-checks
(check_vehicle_sweep, check_track_sweep, ...) which themselves inherit
os.environ faithfully -- so a sub-check that is correct on its own still lands
on the shared directory when its parent scrubbed. This gate is the audit that
wave asked for.

WHAT IT ACCEPTS. Inheriting an explicit MDKR_SAVE_DIR (dict(os.environ)) is
fine -- that is how the suite's per-task pin reaches a check. What is not fine
is destroying the pin and then launching. A scrubbed environment is repaired by
harness_utils.save_env(env, run_dir) (preferred: it pins the video config,
which check_harness_isolation.py requires, and the mods folder in one breath),
by an explicit env["MDKR_SAVE_DIR"] = ... assignment, or by passing
MDKR_SAVE_DIR into the call that builds it.

DIRECT RUNS. Inheritance is only safe when there is a pin to inherit. Started
by hand, a check that launches with dict(os.environ) -- or with no env= at all
-- hands the engine no MDKR_SAVE_DIR, and the engine writes the owner's real
per-user save. That is how check_camera_projection_fallback_runtime.py and
check_camera_probe_invalid_recovery.py wrote it: this gate only followed
environments that SCRUB MDKR*, and theirs never did, so it had nothing to
follow. Importing harness_utils from a check that was started with no
MDKR_SAVE_DIR now exports private saves, prefs, video config and mods into
os.environ (harness_utils.isolate_direct_run), so the rule here is: a check
that launches with an inherited environment must import harness_utils,
directly or through another tests/ module. The isolation itself, the save_env
mods pin, and tools/run_checks.py's per-task mods pin and pack-leak refusal
are exercised behaviourally below, so the rule cannot pass on a promise.

Usage: test_check_save_dir_hermeticity.py [--list]
"""

from __future__ import annotations

import ast
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent
TOOLS_DIR = TESTS_DIR.parent / "tools"

# Statuses an environment value can carry through a function body.
SCRUBBED = "scrubbed"
PINNED = "pinned"
# os.environ itself, or a copy of it: whatever pin the caller had, if any.
INHERITED = "inherited"

SAVE_DIR = "MDKR_SAVE_DIR"

# subprocess entry points. A call to one of these with a scrubbed environment
# is the launch itself.
SUBPROCESS_CALLS = {"run", "Popen", "call", "check_call", "check_output"}

# Calls that only derive or mutate a dict. Handing a scrubbed environment to
# one of these is not a handoff to a process, so they never raise a finding.
DICT_ONLY_CALLS = {
    "dict", "save_env", "len", "print", "repr", "sorted", "str", "list",
    "update", "setdefault", "copy", "get", "items", "keys", "values", "pop",
    "join", "format", "deepcopy", "sorted", "any", "all",
}

# Checks that legitimately reach a process with no MDKR_SAVE_DIR of their own.
# Every entry states why; a new entry needs one too.
EXEMPT = {
    # The subject of the check IS save resolution with nothing pinned: it pops
    # MDKR_SAVE_DIR (and MDKR_VIDEO_CONFIG_PATH) on purpose to prove that a
    # portable.txt marker beside the executable, and otherwise the per-user
    # fallback, resolve the way the shipped launchers do -- so a pin here would
    # test nothing. It redirects HOME to an empty scratch directory per arm, so
    # the fallback it exercises lands inside the check's own temporary tree and
    # never in the developer's real save directory.
    "check_portable_paths.py",
}


def _name_of(func: ast.expr) -> str | None:
    return getattr(func, "id", None) or getattr(func, "attr", None)


def _is_environ(node: ast.expr) -> bool:
    return isinstance(node, ast.Attribute) and node.attr == "environ"


def _mentions_mdkr(node: ast.AST) -> bool:
    """Does this expression name any MDKR* variable, as a string or a kwarg?"""

    for sub in ast.walk(node):
        if isinstance(sub, ast.Constant) and isinstance(sub.value, str):
            if sub.value.startswith("MDKR"):
                return True
        if isinstance(sub, ast.keyword) and (sub.arg or "").startswith("MDKR"):
            return True
    return False


def _mentions_save_dir(node: ast.AST) -> bool:
    """MDKR_SAVE_DIR named either as a dict key/string or as a kwarg."""

    for sub in ast.walk(node):
        if isinstance(sub, ast.Constant) and sub.value == SAVE_DIR:
            return True
        if isinstance(sub, ast.keyword) and sub.arg == SAVE_DIR:
            return True
    return False


def _is_environ_scrub(node: ast.expr) -> bool:
    """A comprehension over os.environ that filters MDKR keys away."""

    if not isinstance(node, (ast.DictComp, ast.GeneratorExp)):
        return False
    text = ast.unparse(node)
    return "environ" in text and "MDKR" in text and "startswith" in text


def _pins_via_keywords(call: ast.Call, state: dict[str, str]) -> bool:
    """MDKR_SAVE_DIR supplied at the call -- named, or inside a ** unpacking."""

    for keyword in call.keywords:
        if keyword.arg == SAVE_DIR:
            return True
        if keyword.arg is not None:
            continue
        if _mentions_save_dir(keyword.value):
            return True
        # f(**overrides), where `overrides` was built with a save dir in it.
        if (isinstance(keyword.value, ast.Name)
                and state.get(keyword.value.id) == PINNED):
            return True
    return False


# Fields that hold a nested block rather than an expression of the statement
# itself. They are walked separately, and only after the statement's own
# expressions, so a pin written inside a `with` body is seen before the launch
# that same body performs.
BLOCK_FIELDS = ("body", "orelse", "finalbody", "handlers")


def _own_expressions(statement: ast.stmt):
    """The expressions a statement evaluates itself, excluding nested blocks."""

    for field, value in ast.iter_fields(statement):
        if field in BLOCK_FIELDS:
            continue
        for item in (value if isinstance(value, list) else [value]):
            if isinstance(item, ast.withitem):
                yield item.context_expr
                if item.optional_vars is not None:
                    yield item.optional_vars
            elif isinstance(item, ast.AST):
                yield item


class ModuleAudit:
    """Track where a scrubbed environment goes inside one check script."""

    def __init__(self, tree: ast.Module, filename: str,
                 isolates_direct_runs: bool = False):
        self.filename = filename
        self.tree = tree
        # Whether importing this module imports harness_utils, whose import
        # isolates a check that was started by hand (see the docstring).
        self.isolates_direct_runs = isolates_direct_runs
        self.inherited_launches: list[str] = []
        self.functions = {
            node.name: node
            for node in ast.walk(tree)
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        }
        # Factory status: what a local function RETURNS. Resolved to a fixed
        # point so a factory built on another factory is classified correctly.
        self._consumed: dict[tuple[str, str], bool] = {}
        self.factories: dict[str, str | None] = {}
        for _ in range(3):
            before = dict(self.factories)
            for name, node in self.functions.items():
                self.factories[name] = self._returned_status(node)
            if before == self.factories:
                break

    # -- expression classification ----------------------------------------- #

    def _classify(self, node: ast.expr | None,
                  state: dict[str, str]) -> str | None:
        if node is None:
            return None
        if _is_environ_scrub(node):
            return SCRUBBED
        if _is_environ(node):
            return INHERITED
        if isinstance(node, ast.Name):
            return state.get(node.id)
        if isinstance(node, ast.Dict):
            return self._classify_dict(node, state)
        if isinstance(node, ast.Call):
            return self._classify_call(node, state)
        if isinstance(node, ast.IfExp):
            for branch in (node.body, node.orelse):
                status = self._classify(branch, state)
                if status == SCRUBBED:
                    return SCRUBBED
            return None
        return None

    def _classify_dict(self, node: ast.Dict,
                       state: dict[str, str]) -> str | None:
        base: str | None = None
        derived = False
        for key, value in zip(node.keys, node.values):
            if key is None:  # {**other, ...}
                derived = True
                base = self._classify(value, state) or base
            elif isinstance(key, ast.Constant) and key.value == SAVE_DIR:
                return PINNED
        # A bare literal is left unclassified here. Most of them in this corpus
        # are OVERLAY fragments -- an extra_env the callee merges onto a base
        # it owns -- and calling those "scrubbed" would flag the caller for a
        # pin the callee already holds. The one place a bare literal really is
        # the whole environment is the env= argument of a launch, and
        # _visit_call() judges it there.
        return base if derived else None

    def _classify_call(self, node: ast.Call,
                       state: dict[str, str]) -> str | None:
        name = _name_of(node.func)
        if name == "save_env":
            return PINNED
        if _pins_via_keywords(node, state):
            return PINNED
        if name == "dict":
            if not node.args:
                # dict(MDKR_AUDIO="0", ...) -- see _classify_dict().
                return None
            return self._classify(node.args[0], state)
        if name in ("copy", "deepcopy"):
            if isinstance(node.func, ast.Attribute):
                return self._classify(node.func.value, state)
            if node.args:
                return self._classify(node.args[0], state)
        if name in self.factories:
            return self.factories[name]
        return None

    # -- statement walk ----------------------------------------------------- #

    def _walk_body(self, body: list[ast.stmt], state: dict[str, str],
                   findings: list[str] | None, scope: str) -> list[str | None]:
        """Run the flow over ``body``. Returns the statuses of returned values;
        appends handoff findings to ``findings`` when one is supplied."""

        returned: list[str | None] = []
        for statement in body:
            returned.extend(
                self._walk_statement(statement, state, findings, scope))
        return returned

    def _walk_statement(self, statement: ast.stmt, state: dict[str, str],
                        findings: list[str] | None,
                        scope: str) -> list[str | None]:
        returned: list[str | None] = []
        if isinstance(statement, (ast.FunctionDef, ast.AsyncFunctionDef,
                                  ast.ClassDef)):
            return returned
        if isinstance(statement, ast.Assign):
            status = self._classify(statement.value, state)
            for target in statement.targets:
                if isinstance(target, ast.Name):
                    if status is None:
                        state.pop(target.id, None)
                    else:
                        state[target.id] = status
                elif (isinstance(target, ast.Subscript)
                      and isinstance(target.value, ast.Name)
                      and isinstance(target.slice, ast.Constant)
                      and target.slice.value == SAVE_DIR):
                    state[target.value.id] = PINNED
        elif isinstance(statement, ast.AnnAssign):
            if isinstance(statement.target, ast.Name):
                status = self._classify(statement.value, state)
                if status is None:
                    state.pop(statement.target.id, None)
                else:
                    state[statement.target.id] = status
        elif isinstance(statement, ast.Delete):
            for target in statement.targets:
                if (isinstance(target, ast.Subscript)
                        and isinstance(target.value, ast.Name)
                        and isinstance(target.slice, ast.Constant)
                        and target.slice.value == SAVE_DIR):
                    state[target.value.id] = SCRUBBED
        elif isinstance(statement, ast.Return):
            returned.append(self._classify(statement.value, state))
        elif isinstance(statement, ast.With):
            for item in statement.items:
                if isinstance(item.optional_vars, ast.Name):
                    status = self._classify(item.context_expr, state)
                    if status is not None:
                        state[item.optional_vars.id] = status

        # Mutations and handoffs are found on every call the statement
        # evaluates, not only on assignment right-hand sides.
        for expression in _own_expressions(statement):
            for node in ast.walk(expression):
                if isinstance(node, ast.Call):
                    self._visit_call(node, state, findings, scope)

        # Nested blocks last, so their statements see the state this one left.
        for field in ("body", "orelse", "finalbody"):
            block = getattr(statement, field, None)
            if isinstance(block, list):
                returned.extend(self._walk_body(block, state, findings, scope))
        for handler in getattr(statement, "handlers", []) or []:
            returned.extend(
                self._walk_body(handler.body, state, findings, scope))
        return returned

    def _visit_call(self, node: ast.Call, state: dict[str, str],
                    findings: list[str] | None, scope: str) -> None:
        name = _name_of(node.func)

        # save_env(env, run_dir) pins in place, so the return value is often
        # discarded; the environment it was handed is pinned either way.
        if name == "save_env" and node.args and isinstance(node.args[0],
                                                           ast.Name):
            state[node.args[0].id] = PINNED
            return

        # env.update(MDKR_SAVE_DIR=...) / env.setdefault("MDKR_SAVE_DIR", ...)
        if (isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)):
            owner = node.func.value.id
            if name in ("update", "setdefault") and (
                    _mentions_save_dir(node)
                    or _pins_via_keywords(node, state)
                    or any(isinstance(argument, ast.Name)
                           and state.get(argument.id) == PINNED
                           for argument in node.args)):
                state[owner] = PINNED
                return
            if (name == "pop" and node.args
                    and isinstance(node.args[0], ast.Constant)
                    and node.args[0].value == SAVE_DIR):
                state[owner] = SCRUBBED
                return

        if findings is None:
            return

        if (name in SUBPROCESS_CALLS and isinstance(node.func, ast.Attribute)
                and isinstance(node.func.value, ast.Name)
                and node.func.value.id == "subprocess"):
            environment = next((k.value for k in node.keywords
                                if k.arg in ("env", "environment")), None)
            if (environment is None
                    or self._classify(environment, state) == INHERITED):
                self.inherited_launches.append(
                    f"{scope}: line {node.lineno}: subprocess.{name}(...)")

        if name in SUBPROCESS_CALLS:
            for keyword in node.keywords:
                if keyword.arg not in ("env", "environment"):
                    continue
                value = keyword.value
                if self._classify(value, state) == SCRUBBED:
                    findings.append(
                        f"{scope}: line {node.lineno}: {name}(...) launches "
                        f"with a scrubbed environment that has no {SAVE_DIR}")
                    return
                # A whole environment written out at the launch inherits
                # nothing at all, so it is missing MDKR_SAVE_DIR for the same
                # reason a scrub is. An engine environment always configures
                # the engine, which is what tells it apart from the env= of
                # some ordinary tool.
                if (isinstance(value, ast.Dict) and None not in value.keys
                        and _mentions_mdkr(value)
                        and not _mentions_save_dir(value)):
                    findings.append(
                        f"{scope}: line {node.lineno}: {name}(...) launches "
                        f"with an inline environment that inherits nothing "
                        f"and sets no {SAVE_DIR}")
                    return

        if name in DICT_ONLY_CALLS:
            return
        arguments = list(node.args) + [k.value for k in node.keywords]
        for argument in arguments:
            if (isinstance(argument, ast.Name)
                    and state.get(argument.id) == SCRUBBED):
                parameter = self._bound_parameter(node, argument)
                if parameter is not None and self._consumes_safely(name,
                                                                   parameter):
                    continue
                findings.append(
                    f"{scope}: line {node.lineno}: {name}() receives "
                    f"'{argument.id}', a scrubbed environment with no "
                    f"{SAVE_DIR}")
                return

    def _parameters(self, node: ast.AST) -> list[str]:
        arguments = node.args
        names = [argument.arg for argument in
                 (list(getattr(arguments, "posonlyargs", []))
                  + list(arguments.args) + list(arguments.kwonlyargs))]
        return names

    def _consumes_safely(self, function: str, parameter: str) -> bool:
        """Would this function repair a scrubbed environment handed to it?

        A caller is entitled to leave the pin to the callee -- run_case(env)
        that does save_env(dict(env), run_dir) before it launches is correct,
        and flagging its caller would push the pin to the wrong place. The
        answer is computed by replaying the callee's own flow with that
        parameter seeded as scrubbed: if nothing reaches a process unpinned,
        the callee is holding up its end.
        """

        key = (function, parameter)
        if key in self._consumed:
            return self._consumed[key]
        # An optimistic seed terminates recursion through a cycle; a real
        # unpinned launch inside the cycle still flips it below.
        self._consumed[key] = True
        node = self.functions.get(function)
        if node is None:
            return True
        findings: list[str] = []
        self._walk_body(node.body, {parameter: SCRUBBED}, findings, function)
        self._consumed[key] = not findings
        return self._consumed[key]

    def _bound_parameter(self, node: ast.Call, argument: ast.expr) -> str | None:
        """The callee parameter this argument lands on, for a local callee."""

        name = _name_of(node.func)
        if name not in self.functions:
            return None
        for keyword in node.keywords:
            if keyword.value is argument:
                return keyword.arg
        names = self._parameters(self.functions[name])
        for index, positional in enumerate(node.args):
            if positional is argument:
                return names[index] if index < len(names) else None
        return None

    def _returned_status(self, node: ast.AST) -> str | None:
        state: dict[str, str] = {}
        returned = self._walk_body(node.body, state, None, node.name)
        if SCRUBBED in returned:
            return SCRUBBED
        if PINNED in returned:
            return PINNED
        if INHERITED in returned:
            return INHERITED
        return None

    def findings(self) -> list[str]:
        found: list[str] = []
        module_body = [
            statement for statement in self.tree.body
            if not isinstance(statement, (ast.FunctionDef,
                                          ast.AsyncFunctionDef, ast.ClassDef))
        ]
        self._walk_body(module_body, {}, found, "<module>")
        for name, node in sorted(self.functions.items(),
                                 key=lambda item: item[1].lineno):
            self._walk_body(node.body, {}, found, name)
        if self.inherited_launches and not self.isolates_direct_runs:
            for launch in dict.fromkeys(self.inherited_launches):
                found.append(
                    f"{launch} inherits the caller's environment, and "
                    "nothing gives a direct run a private MDKR_SAVE_DIR to "
                    "inherit: import harness_utils (its import isolates a "
                    "check started by hand), or pin the save dir here")
        return found


ISOLATING_MODULE = "harness_utils"


def _imported_modules(tree: ast.Module) -> set[str]:
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            names.update(alias.name.split(".")[0] for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.add(node.module.split(".")[0])
    return names


def isolating_modules(directory: Path) -> set[str]:
    """Module names in ``directory`` whose import imports harness_utils.

    Followed through sibling modules, because a check often reaches it only
    through another check it borrows a helper from.
    """

    imports: dict[str, set[str]] = {}
    for path in directory.glob("*.py"):
        try:
            imports[path.stem] = _imported_modules(
                ast.parse(path.read_text(encoding="utf-8"), str(path)))
        except SyntaxError:
            imports[path.stem] = set()
    reached = {ISOLATING_MODULE}
    changed = True
    while changed:
        changed = False
        for name, imported in imports.items():
            if name not in reached and imported & reached:
                reached.add(name)
                changed = True
    return reached


def scan(directory: Path) -> dict[str, list[str]]:
    offenders: dict[str, list[str]] = {}
    isolating = isolating_modules(directory)
    for path in sorted(directory.glob("check_*.py")):
        if path.name in EXEMPT:
            continue
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), str(path))
        except SyntaxError as error:  # a broken check is its own failure
            offenders[path.name] = [f"unparseable: {error}"]
            continue
        found = ModuleAudit(tree, path.name,
                            path.stem in isolating).findings()
        if found:
            offenders[path.name] = found
    return offenders


# --------------------------------------------------------------------------- #
#  Controls -- the gate must reject the defect and accept every repair shape.
# --------------------------------------------------------------------------- #

OFFENDING = '''
import os
import subprocess


def clean_environment(**updates):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR")}
    env.update(updates)
    return env


def run(binary):
    env = clean_environment(MDKR_AUDIO="0")
    return subprocess.run([binary], env=env)
'''

OFFENDING_INLINE = '''
import os
import subprocess


def run(binary):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR_")}
    env["MDKR_AUDIO"] = "0"
    subprocess.run([binary], env=env)
'''

OFFENDING_HANDOFF = '''
import os
import subprocess


def run_route(command, env):
    subprocess.run(command, env=env)


def main(binary):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR")}
    run_route([binary], env)
'''

OFFENDING_FRESH = '''
import subprocess


def run(binary):
    subprocess.run([binary], env={"MDKR_AUDIO": "0"})
'''

OFFENDING_POP = '''
import os
import subprocess


def run(binary):
    env = dict(os.environ)
    env.pop("MDKR_SAVE_DIR", None)
    subprocess.run([binary], env=env)
'''

PINNED_HELPER = '''
import os
import subprocess

from harness_utils import save_env


def run(binary, run_dir):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR")}
    env = save_env(env, run_dir)
    subprocess.run([binary], env=env)
'''

PINNED_INLINE = '''
import os
import subprocess


def run(binary, run_dir):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR")}
    env["MDKR_SAVE_DIR"] = run_dir
    env["MDKR_VIDEO_CONFIG_PATH"] = os.devnull
    subprocess.run([binary], env=env)
'''

PINNED_AT_FACTORY_CALL = '''
import os
import subprocess


def clean_environment(**updates):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MDKR")}
    env.update(updates)
    return env


def run(binary, run_dir):
    env = clean_environment(MDKR_AUDIO="0", MDKR_SAVE_DIR=run_dir)
    subprocess.run([binary], env=env)
'''

# Inheriting is right under the suite and wrong by hand, unless the import of
# harness_utils gave the hand-started run a pin to inherit.
OFFENDING_INHERITED = '''
import os
import subprocess


def run(binary):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"
    subprocess.run([binary], env=env)
'''

OFFENDING_IMPLICIT = '''
import subprocess


def run(binary):
    subprocess.run([binary, "--headless-frames", "30"])
'''

OFFENDING_COPY = '''
import os
import subprocess


def run(binary):
    env = os.environ.copy()
    subprocess.Popen([binary], env={**env, "MDKR_AUDIO": "0"})
'''

INHERITED = '''
import os
import subprocess

from harness_utils import resolve_binary


def run(build):
    env = dict(os.environ)
    env["MDKR_AUDIO"] = "0"
    subprocess.run([resolve_binary(build)], env=env)
'''

INHERITED_THROUGH_HELPER = '''
import subprocess

import ctl_helper


def run(binary):
    subprocess.run([binary])
'''

# Modules the controls import; written beside them, never scanned themselves.
HELPERS = {
    "ctl_helper.py": "from harness_utils import DEFAULT_BUILD_DIR\n",
}

CONTROLS = (
    ("check_ctl_factory.py", OFFENDING, True),
    ("check_ctl_inline.py", OFFENDING_INLINE, True),
    ("check_ctl_handoff.py", OFFENDING_HANDOFF, True),
    ("check_ctl_fresh.py", OFFENDING_FRESH, True),
    ("check_ctl_pop.py", OFFENDING_POP, True),
    ("check_ctl_save_env.py", PINNED_HELPER, False),
    ("check_ctl_assign.py", PINNED_INLINE, False),
    ("check_ctl_kwarg.py", PINNED_AT_FACTORY_CALL, False),
    ("check_ctl_inherited.py", OFFENDING_INHERITED, True),
    ("check_ctl_implicit.py", OFFENDING_IMPLICIT, True),
    ("check_ctl_copy.py", OFFENDING_COPY, True),
    ("check_ctl_inherit.py", INHERITED, False),
    ("check_ctl_helper.py", INHERITED_THROUGH_HELPER, False),
)


def self_test() -> list[str]:
    """A synthetic corpus the scanner must split exactly one way."""

    problems: list[str] = []
    with tempfile.TemporaryDirectory(prefix="mdkr-save-hermeticity-") as raw:
        fixture = Path(raw)
        for name, source, _ in CONTROLS:
            (fixture / name).write_text(source, encoding="utf-8")
        for name, source in HELPERS.items():
            (fixture / name).write_text(source, encoding="utf-8")
        found = scan(fixture)
        expected = {name for name, _, offends in CONTROLS if offends}
        actual = set(found)
        for missed in sorted(expected - actual):
            problems.append(
                f"positive control not caught: {missed} scrubs MDKR* and "
                "launches with no save-dir pin, but the scanner passed it")
        for spurious in sorted(actual - expected):
            problems.append(
                f"negative control rejected: {spurious} pins its save dir, "
                f"but the scanner flagged it ({found[spurious]})")
    return problems


# What a check started by hand reports about its own environment.
DIRECT_RUN_PROBE = '''
import json, os, sys
sys.path.insert(0, sys.argv[1])
import harness_utils
names = ("MDKR_SAVE_DIR", "MDKR_TEST_SAVE_DIR", "MDKR_VIDEO_CONFIG_PATH",
         "MDKR_MODS_DIR", "MDKR_APP_PREFS_DIR")
seen = {name: os.environ.get(name) for name in names}
seen["save_exists"] = bool(seen["MDKR_SAVE_DIR"]) and os.path.isdir(seen["MDKR_SAVE_DIR"])
seen["mods_exists"] = bool(seen["MDKR_MODS_DIR"]) and os.path.exists(seen["MDKR_MODS_DIR"])
print(json.dumps(seen))
'''


def isolation_problems() -> list[str]:
    """The mechanisms the static rule relies on, exercised for real."""

    problems: list[str] = []
    base = {key: value for key, value in os.environ.items()
            if not key.startswith("MDKR")}
    with tempfile.TemporaryDirectory(prefix="mdkr-direct-run-") as raw:
        fixture = Path(raw)
        for name in ("check_ctl_probe.py", "test_ctl_probe.py"):
            (fixture / name).write_text(DIRECT_RUN_PROBE, encoding="utf-8")

        def probe(name: str, env: dict[str, str]) -> dict:
            result = subprocess.run(
                [sys.executable, str(fixture / name), str(TESTS_DIR)],
                env=env, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, timeout=60, check=False)
            if result.returncode:
                raise RuntimeError(f"{name} exited {result.returncode}: "
                                   f"{result.stderr.strip()}")
            return json.loads(result.stdout)

        try:
            direct = probe("check_ctl_probe.py", dict(base))
            aimed = probe("check_ctl_probe.py",
                          dict(base, MDKR_SAVE_DIR=str(fixture / "aimed")))
            unit = probe("test_ctl_probe.py", dict(base))
        except (RuntimeError, ValueError, subprocess.SubprocessError) as error:
            return [f"direct-run probe failed: {error}"]
        save = direct["MDKR_SAVE_DIR"] or ""
        root = Path(save).parent
        temp = Path(tempfile.gettempdir()).resolve()
        if not (save and direct["save_exists"]
                and Path(save).resolve().is_relative_to(temp)
                and root.name.startswith("mdkr64-direct-run-")):
            problems.append("a check started by hand did not get a private "
                            f"MDKR_SAVE_DIR: {direct}")
        else:
            if direct["MDKR_TEST_SAVE_DIR"] != save:
                problems.append("a direct run's MDKR_TEST_SAVE_DIR is not its "
                                f"MDKR_SAVE_DIR: {direct}")
            if direct["MDKR_VIDEO_CONFIG_PATH"] != os.devnull:
                problems.append("a direct run reads a video config: "
                                f"{direct['MDKR_VIDEO_CONFIG_PATH']}")
            for name in ("MDKR_MODS_DIR", "MDKR_APP_PREFS_DIR"):
                if not (direct[name] or "").startswith(str(root)):
                    problems.append(f"a direct run's {name} is not private: "
                                    f"{direct[name]}")
            if direct["mods_exists"]:
                problems.append("a direct run's mods folder exists; it must "
                                "be named, never created")
            if root.exists():
                problems.append(f"a direct run left {root} behind")
        if aimed["MDKR_SAVE_DIR"] != str(fixture / "aimed") or aimed["MDKR_MODS_DIR"]:
            problems.append("a run that already has MDKR_SAVE_DIR (a suite task "
                            f"or an explicit aim) was rewritten: {aimed}")
        if unit["MDKR_SAVE_DIR"] is not None:
            problems.append("importing harness_utils from a unit test changed "
                            f"its environment: {unit}")

    sys.path.insert(0, str(TESTS_DIR))
    import harness_utils  # noqa: E402 -- after the probes, which import it fresh
    with tempfile.TemporaryDirectory(prefix="mdkr-save-env-") as raw:
        env = harness_utils.save_env({}, raw)
        mods = env.get("MDKR_MODS_DIR", "")
        if not mods.startswith(os.path.abspath(raw)) or os.path.exists(mods):
            problems.append(f"save_env pinned no private mods folder: {env}")
        chosen = harness_utils.save_env({"MDKR_MODS_DIR": "fixture"}, raw)
        if chosen["MDKR_MODS_DIR"] != "fixture":
            problems.append("save_env overrode a caller's own mods folder")

    sys.path.insert(0, str(TOOLS_DIR))
    import run_checks  # noqa: E402
    seen: list[str] = []
    for role in ("native", "asan"):
        check = run_checks.Check("ctl", "check_ctl.py", role, "control")
        with run_checks.task_environment(check, {"MDKR_MODS_DIR": "x"}) as env:
            mods, prefs = env.get("MDKR_MODS_DIR", ""), env.get("MDKR_APP_PREFS_DIR", "")
            if mods == "x" or not mods or os.path.exists(mods) or mods in seen:
                problems.append(f"run_checks gave a {role} task no fresh empty "
                                f"mods folder: {mods!r}")
            if not os.path.isdir(prefs):
                problems.append(f"run_checks gave a {role} task no private "
                                f"app-prefs folder: {prefs!r}")
            halting = "halt_on_error=1" in env.get("UBSAN_OPTIONS", "")
            if halting != (role == "asan"):
                problems.append(f"run_checks {role} task UBSAN_OPTIONS="
                                f"{env.get('UBSAN_OPTIONS')!r}")
            seen.append(mods)
    with tempfile.TemporaryDirectory(prefix="mdkr-pack-leak-") as raw:
        root = Path(raw)
        binary = root / "build" / "mdkr64"
        binary.parent.mkdir()
        (root / "mods").mkdir()
        if run_checks.pack_leaks((binary,), root):
            problems.append("an empty repo-root mods folder was reported")
        (root / "mods" / "SomePack").mkdir()
        if not run_checks.pack_leaks((binary,), root):
            problems.append("a pack in the repo-root mods folder was not "
                            "reported; scrubbing checks would load it")
        (root / "mods" / "SomePack").rmdir()
        (binary.parent / "portable.txt").write_text("portable\n")
        if not run_checks.pack_leaks((binary,), root):
            problems.append("a portable.txt beside a tested build was not "
                            "reported; it relocates that build's mods folder")
    return problems


def main() -> int:
    problems = self_test() + isolation_problems()
    if problems:
        print("FAIL: the save-dir hermeticity scanner is not trustworthy:",
              file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1
    print("PASS: a direct run is isolated, save_env pins mods, and run_checks "
          "pins a fresh mods/prefs folder per task and refuses packs in reach")
    print(f"PASS: scanner controls -- "
          f"{sum(1 for _, _, bad in CONTROLS if bad)} offending shapes "
          f"rejected, {sum(1 for _, _, bad in CONTROLS if not bad)} pinned "
          f"shapes accepted")

    for name in sorted(EXEMPT):
        if not (TESTS_DIR / name).is_file():
            print(f"FAIL: exempt file {name} no longer exists; prune the "
                  "exemption", file=sys.stderr)
            return 1

    offenders = scan(TESTS_DIR)
    if "--list" in sys.argv[1:]:
        for name, found in offenders.items():
            print(name)
            for line in found:
                print(f"    {line}")
        return 0
    if offenders:
        print(f"FAIL: {len(offenders)} check script"
              f"{'' if len(offenders) == 1 else 's'} scrub MDKR* out of an "
              "engine environment and then launch without pinning "
              f"{SAVE_DIR}. They fall back to the SHARED per-user save "
              "directory, so an unrelated EEPROM on the host re-routes them "
              "(see the module docstring). Pin a per-check temporary "
              "directory with harness_utils.save_env():", file=sys.stderr)
        for name, found in offenders.items():
            print(f"  {name}", file=sys.stderr)
            for line in found:
                print(f"      {line}", file=sys.stderr)
        return 1

    print(f"PASS: every check that scrubs MDKR* re-pins {SAVE_DIR} before it "
          "launches, and every check that launches with an inherited "
          "environment imports the direct-run isolation "
          f"({len(EXEMPT)} documented exemption"
          f"{'' if len(EXEMPT) == 1 else 's'})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
