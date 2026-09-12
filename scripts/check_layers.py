#!/usr/bin/env python3
"""Layer dependency gate (standards/enforcement.md section 4).

Turns the dependency law of `docs/plan-tree.md` section 8 from a "documented agreement" into a "CI refusal condition".

Checks:
  L1 allowed direction: an include inside core must match the explicit whitelist
  L2 no Qt: no Q* header may appear under core
  L3 Eigen restricted: allowed in eval / kernels only
  L4 no reverse dependency: core must not include headers from views / plugins
  L5 whitelist coverage: a module absent from the rules -> an error, forcing explicit registration

Design point: **the whitelist is explicit**. A new cross-module dependency must edit this file,
and that edit is itself the confirmation "I know I am widening coupling". The friction is intentional.

Exit codes: 0 pass; 1 violations found.
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')

# Layer directories: they are only **categories**, not modules. A module is one level below a layer.
# Example: `qp/graph/ir/node.hpp` belongs to module ir, not to module graph.
LAYER_DIRS = frozenset({"graph", "runtime", "authoring"})

# -- allowed dependency directions (module -> its allowed deps) --------------
#
# Kept in sync with the whitelist in docs/plan-tree.md section 8. A new module must be registered
# here, otherwise L5 reports an "unregistered module".
ALLOWED: dict[str, set[str]] = {
    # L0 foundation
    "units": set(),
    "diag": {"units"},
    "reflect": {"units"},
    "abi": {"units"},
    # ports needs abi's LatticeDesc as the field-handle payload of Value --
    # a port must carry fields, and a field's layout is defined in abi. This dependency is intentional.
    "ports": {"units", "diag", "abi"},
    # plugin judges a manifest and orders a plugin set. It does not parse a file
    # and does not load a shared library: the file format is a serialisation
    # choice and belongs to an adapter, and mapping code into the process is
    # platform-specific. What is here is everything decidable before any code
    # runs, which is the part that must not vary by OS.
    "plugin": {"units", "diag", "abi", "ports", "reflect"},
    # L1 graph and execution
    "ir": {"units", "diag", "ports"},
    "structure": {"units", "diag", "ir"},
    # A mutate command carries qp::ports::Value (the parameter payload of SetParam),
    # so mutate -> ports must be allowed. That follows from Value being defined in ports.
    "mutate": {"units", "diag", "ports", "ir", "structure"},
    "validate": {"units", "diag", "ports", "ir", "structure"},
    "eval": {"units", "diag", "ports", "abi", "ir", "structure"},
    # domain uses validate's Report to express a "stale declaration" and needs structure to read the graph.
    # The direction is domain -> validate (**one-way**): validate does not depend on domain, because
    # domain semantics must have exactly one definition site -- an early version defined the same
    # Domain enum in two modules, so any translation unit including both redefined it.
    "domain": {"units", "diag", "abi", "ir", "structure", "validate"},
    # kernels needs field because a batch is handed to an operator in the field
    # vocabulary: `field::FieldValue` is "a described span of samples", and using
    # it is what lets one operator run on a CPU array in a test and on a packed
    # buffer in production without a second implementation. field depends only on
    # abi, so the direction stays acyclic and field remains the more fundamental
    # of the two.
    #
    # This is a deliberate widening of the plan-tree table, which listed
    # kernels -> abi, domain. The alternative was to duplicate the batch
    # descriptor inside kernels, which would give the platform two incompatible
    # answers to "what is a described span".
    "field": {"units", "diag", "abi"},
    # execution owns the run loop's contracts: an operator, a state buffer, and the binder that
    # turns a node into an operator. It was written in the view layer first and did not compile --
    # the binder must live in the plugin that ships the operator, and views already links plugins,
    # so `plugins -> views -> plugins` was a cycle. That is the layering saying the interfaces were
    # in the wrong place, and they were: driving an operator over time is mechanism.
    #
    # It deliberately does not depend on `kernels`. An `IStateOperator` is two methods, and binding
    # this to `IBatchAdvancer` would give a run loop that can only drive batch advancers; the
    # adapter presenting one as the other belongs to the plugin, next to the code it adapts.
    "execution": {"units", "diag", "ir", "structure",
                  "run", "store", "trace"},
    "kernels": {"units", "diag", "abi", "field", "domain"},
    # L2 runs and data
    "run": {"units", "diag", "abi", "ports"},
    "store": {"units", "diag", "ports"},
    "trace": {"units", "diag", "run", "store"},
    # io needs trace, not just store: what a user exports is a **time series**,
    # and a series without its sample times is not the same artifact -- it is
    # a column of numbers whose spacing a reader has to guess. The plan-tree
    # table listed store and abi, which covers writing a single frozen dataset
    # but not the run that produced it.
    "io": {"units", "diag", "abi", "store", "trace"},
    # file sits below the format contracts rather than inside one of them: a trace
    # exporter and the document persistence contract both move bytes to and from a
    # path, and either hosting the helper would make the other depend on a module
    # whose subject is something else. Its dependencies are the standard library.
    "file": {"units", "diag"},
    # L3 view services
    "document": {"units", "diag", "ir", "abi"},
    # capability needs plugin, not ports: it negotiates what a plugin may
    # contribute, which is a question about the manifest. It never touches a
    # port type, and keeping it clear of ports means a capability query cannot
    # accidentally depend on the type registry being populated.
    "capability": {"units", "diag", "plugin"},
    "commands": {"units", "diag", "ir", "structure", "mutate"},
    # layout takes a whole Graph, not just the IR: a position is per node instance,
    # and the container that holds instances is structure.
    "layout": {"units", "diag", "ir", "structure", "document"},
    "portui": {"units", "diag", "ports", "capability"},
    # persist needs structure, which document deliberately does not: a saved
    # document holds a graph as well as the per-view layout slots, and a graph is
    # a container of instances. Adding structure to document instead would have
    # widened that module for a job its own file comment says it does not do --
    # "a document describes a graph; it does not hold a second one".
    "persist": {"units", "diag", "ir", "structure", "document"},
}

# Nothing outside core may be depended on by core
FORBIDDEN_PREFIXES = ("views", "plugins", "external")

# Eigen may appear in these two modules only
EIGEN_ALLOWED = {"eval", "kernels"}


@dataclass
class Violation:
    path: Path
    line: int
    rule: str
    message: str

    def render(self, root: Path) -> str:
        try:
            shown = self.path.relative_to(root)
        except ValueError:
            shown = self.path
        return f"{shown}:{self.line}: [{self.rule}] {self.message}"


def module_of(include: str, core_root: Path) -> str | None:
    """Maps an include path to a core module name; also used for source paths.

    Directory conventions (both historical spellings are accepted):
      `qp/units/dim.hpp`         -> units
      `qp/diag/result.hpp`       -> diag
      `qp/graph/ir/node.hpp`     -> ir      (the layer directory is skipped)
      `qp/abi/field_buffer.hpp`  -> abi
      `qp/units.hpp`             -> units   umbrella header
      `qp/graph/ir.hpp`          -> ir      umbrella header under a layer directory

    Implementation: after dropping directory-convention noise such as `include` / `src`,
    take the **first non-layer directory** below `qp/`; if everything was filtered out
    (that is, the path looks like `qp/<layer>/<file>`), take the file name stem.
    """
    parts = [p for p in Path(include).parts if p not in ("include", "src")]
    if len(parts) < 2 or parts[0] != "qp":
        return None

    dirs = list(parts[1:-1])          # directory segments between qp and the file name
    modules = [d for d in dirs if d not in LAYER_DIRS]
    if modules:
        return modules[0]

    # No module directory: the path is qp/<layer>/<file> or qp/<file>
    return Path(parts[-1]).stem or None


def module_of_relative(rel_parts: tuple[str, ...]) -> str | None:
    """Determines the owning module from a path **relative to the core root**.

    Two directory conventions are supported:
      - development: `<layer>/<mod>/include/qp/...` -> the segments after `qp/`
      - simplified:  `<mod>/include/qp/...`         -> segment 0

    Key: this logic must be **exactly consistent** with `module_of()`, or "which module a file
    belongs to" and "which module an include points at" would use two rule sets and contradict
    each other. An early version used only `rel_parts[0]`, so `graph/ir/...` was misread as
    module "graph", disagreeing with the "ir" parsed from the include -- caught by the layer gate.

    So this delegates to `module_of()` uniformly: locate the `qp` segment in the path,
    then hand the whole `qp/...` part to it.
    """
    if not rel_parts:
        return None
    rest = tuple(p for p in rel_parts if p not in ("include", "src"))
    # Path looks like <layer>/<mod>/qp/... or <mod>/qp/...: hand it to module_of starting at `qp`
    if "qp" in rest:
        i = rest.index("qp")
        return module_of("/".join(rest[i:]), Path("."))
    # Source path (no qp segment): looks like <layer>/<mod>/<file> or <mod>/<file>
    dirs = [d for d in rest[:-1] if d not in LAYER_DIRS]
    if dirs:
        return dirs[0]
    # Everything was filtered out (e.g. <layer>/<file>): fall back to the file name
    return Path(rest[-1]).stem if rest else None


def check_file(path: Path, core_root: Path, root: Path,
               allowed: dict[str, set[str]]) -> list[Violation]:
    violations: list[Violation] = []
    try:
        rel = path.relative_to(core_root)
    except ValueError:
        return violations
    own = module_of_relative(rel.parts)
    if own is None:
        return violations

    for lineno, raw in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
        m = INCLUDE_RE.match(raw)
        if not m:
            continue
        inc = m.group(1)

        # L2: no Qt
        if (re.match(r"^Q[A-Z]\w*$", inc)
                or inc.startswith(("QtCore/", "QtGui/", "QtQuick", "QtWidgets", "QtQml"))):
            violations.append(Violation(path, lineno, "L2",
                                        f"Qt must not be included inside core: {inc}"))
            continue

        # L4: no reverse dependency
        if inc.split("/")[0] in FORBIDDEN_PREFIXES:
            violations.append(Violation(path, lineno, "L4",
                                        f"core must not depend on {inc.split('/')[0]}/: {inc}"))
            continue

        # L3: Eigen restricted
        if inc.startswith("Eigen/") or inc == "Eigen":
            if own not in EIGEN_ALLOWED:
                violations.append(Violation(
                    path, lineno, "L3",
                    f"Eigen is allowed only in {sorted(EIGEN_ALLOWED)}, current module is {own!r}: {inc}"))
            continue

        # Only mutual dependencies inside core are checked
        dep = module_of(inc, core_root)
        if dep is None:
            continue

        # L5: unregistered module
        if dep not in allowed:
            violations.append(Violation(path, lineno, "L5",
                                        f"unregistered core module {dep!r} (add it explicitly to ALLOWED)"))
            continue
        if own not in allowed:
            violations.append(Violation(path, lineno, "L5",
                                        f"the current module {own!r} is not registered in ALLOWED"))
            continue

        # L1: allowed direction
        if dep == own:
            continue
        if dep not in allowed[own]:
            violations.append(Violation(
                path, lineno, "L1",
                f"disallowed dependency {own} -> {dep} ({own} allows: {sorted(allowed[own]) or 'none'})"))

    return violations


def main(argv: list[str] | None = None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="core layer dependency gate")
    parser.add_argument("--core", default=str(repo_root / "core"), help="core root directory")
    parser.add_argument("--root", default=str(repo_root), help="used for relative path display")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    core_root = Path(args.core)
    root = Path(args.root).resolve()
    if not core_root.is_dir():
        print(f"error: core directory does not exist: {core_root}", file=sys.stderr)
        return 1

    files = sorted(p for p in core_root.rglob("*") if p.suffix in (".hpp", ".h", ".cpp"))
    violations: list[Violation] = []
    for path in files:
        violations.extend(check_file(path, core_root, root, ALLOWED))

    if not args.quiet:
        print(f"layer check: scanned {len(files)} files, {len(ALLOWED)} modules registered")

    for v in violations:
        print(v.render(root))

    if violations:
        print(f"\ngate failed: {len(violations)} layer violations", file=sys.stderr)
        return 1
    if not args.quiet:
        print("gate passed: layer dependencies are legal.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
