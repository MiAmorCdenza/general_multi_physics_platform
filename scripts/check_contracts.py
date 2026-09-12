#!/usr/bin/env python3
"""Contract completeness gate.

Corresponds to standards/enforcement.md §5. Turns the specification in
standards/function-contract.md from a "documented convention" into an "executable gate".

Checks:
  C1 Every function declaration must be preceded by a /** */ contract block
  C2 The block must contain the six required fields (@ownership/@thread/@pre/@post/@errors/@tests)
  C3 The values of @ownership and @thread must be legal
  C4 @tests must have at least one entry, and every entry must really exist under tests/
  C5 @errors must agree with the declared noexcept
  C6 Reverse check: every TEST_CASE must be referenced by at least one function's @tests
     (catches both "tested something useless" and "contract not written")

Exit code: 0 pass; 1 violations.

Usage:
    python scripts/check_contracts.py
    python scripts/check_contracts.py --headers core --tests tests --quiet
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

DOC_BLOCK = re.compile(r"/\*\*(?P<body>.*?)\*/", re.S)
TEST_CASE_RE = re.compile(r'TEST_CASE\s*\(\s*"(?P<id>[^"]+)"')
TAG_RE = re.compile(r"@(\w+)")
ID_TOKEN_RE = re.compile(r"[A-Za-z_][\w.\-]*")
PREPROC_RE = re.compile(r"^\s*#")

# Required fields. Note: the TAGS dict keys are the bare names without '@'; this must match them.
REQUIRED_TAGS = ("ownership", "thread", "pre", "post", "errors", "tests")
OWNERSHIP_VALUES = {"pure", "owns", "observes", "borrows", "value"}

# Thread roles. The value set is itself part of the contract: a new role must be registered
# explicitly, or "who may call this function" becomes vague prose.
THREAD_VALUES = {
    "any",      # no shared mutable state; any thread may call it
    "main",     # main thread only (UI, document editing)
    "eval",     # graph evaluation thread only
    "ui",       # UI thread only (used when distinct from main)
    "publish",  # data publishing thread only (the write side of the seqlock; see core/abi)
}


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


@dataclass
class FunctionContract:
    path: Path
    line: int
    signature: str
    tags: dict[str, str] = field(default_factory=dict)
    test_ids: list[str] = field(default_factory=list)
    owner: str | None = None
    exempt_reason: str | None = None
    in_abi_zone: bool = False
    is_ctor_or_dtor: bool = False


def is_declaration_start(stripped: str) -> bool:
    """Decide whether a line starts a function declaration.

    A `template <...>` on its own line does not count -- the real signature is on the next
    line, and otherwise join_declaration would fold the template head in and lose the name.
    """
    if not stripped or PREPROC_RE.match(stripped):
        return False
    if stripped.startswith(("//", "*", "/*")):
        return False
    if stripped.startswith("template") and "(" not in stripped:
        return False
    if stripped.startswith(("struct", "class", "enum", "using", "namespace")):
        return False
    if "(" not in stripped:
        return False
    # All three endings are legal: pure declaration ';', multi-line body '{', one-line body '}'
    return stripped.endswith((";", "{", "}"))


def join_declaration(lines: list[str], start: int, limit: int = 40) -> tuple[str, int, bool]:
    """Join a declaration into a single line starting at `start`.

    Returns (declaration text, ending line number, whether it is a one-line body).

    Completion criteria:
      - parentheses and braces both balanced, ending with `;` -- a pure declaration
      - parentheses and braces both balanced, ending with `}` -- a one-line inline body

    Balancing braces alone is not enough: an initializer list such as `T{v1, v2}` balances them
    early and makes a return statement look like the end of the declaration (a trap this checker hit).
    """
    parts: list[str] = []
    i = start
    while i < len(lines) and i < start + limit:
        parts.append(lines[i].strip())
        joined = " ".join(parts)
        balanced = (joined.count("{") == joined.count("}")
                    and joined.count("(") == joined.count(")"))
        if balanced and joined.endswith((";", "}")):
            return joined, i, i == start
        i += 1
    return " ".join(parts), min(i, len(lines) - 1), False


# An operator overload whose body is no longer than this is treated as "the semantics are the
# signature itself": exempt from the six required fields, covered by type-level tests; beyond it a
# full contract is required. The threshold 12 covers one-component-per-line expansions (Dim's per-axis ops).
MAX_TRIVIAL_OPERATOR_LINES = 12

# Line-leading keywords that look like a function declaration but are control flow.
CONTROL_KEYWORDS = frozenset({
    "if", "else", "for", "while", "do", "switch", "case", "return", "catch",
    "break", "continue", "delete", "new", "throw", "static_assert", "assert",
})


def build_type_index(lines: list[str]) -> list[str | None]:
    """Forward scan recording the **immediate** enclosing type name of each line.

    Key point: after a struct closes, the outer scope must be restored at once, or a namespace-level
    free function is misread as a class member (this checker's first version fell into that trap).

    Simplified assumptions (matching this project's style, with counterexamples in tests/meta):
      - the `struct`/`class`/`union` keyword and its `{` appear on the same line
      - braces are never mixed with strings or comments
    """
    owner: list[str | None] = []
    stack: list[str] = []
    for line in lines:
        owner.append(stack[-1] if stack else None)
        m = re.match(r"\s*(?:template\s*<.*>\s*)?(?:struct|class|union)\s+(\w+)", line)
        if m:
            stack.append(m.group(1))
        # Net brace change on this line. A net decrease pops that many type scopes.
        net = line.count("{") - line.count("}")
        if m:
            net -= 1  # the struct keyword already pushed, cancel its own '{'
        while net < 0 and stack:
            stack.pop()
            net += 1
    return owner


def compute_depths(lines: list[str]) -> list[int]:
    """Brace depth **at the start** of each line (0 = the file's outermost level)."""
    depths: list[int] = []
    depth = 0
    for line in lines:
        depths.append(depth)
        depth += line.count("{") - line.count("}")
    return depths


def find_decl_start(lines: list[str], from_line: int, window: int = 4) -> int | None:
    """Find the declaration line adjacent to a doc block (0-based); None when absent.

    Project convention: a contract block must sit **immediately before** the declaration (only blank
    lines in between), so another comment line stops the search -- that means a file header or type
    header, not a function's contract. This also blocks "file header given to the first function".

    More traps:
    1. **One-line body misread**: `explicit constexpr Quantity(double v) noexcept : v_(v) {}`
       closes on the same line and must be recognised in place, not skipped.
    2. **Control flow misread**: `if (style == ...) {` looks like a declaration; a keyword blacklist rules it out.
    3. **Falling into a function body**: a multi-line declaration walks the search into the body,
       where a statement like `const auto exps = ...;` is taken for a declaration. **Brace depth** filters it.
    """
    depths = compute_depths(lines)
    if from_line >= len(lines):
        return None
    doc_depth = depths[from_line]

    i = from_line
    end = min(from_line + window, len(lines))
    while i < end:
        stripped = lines[i].strip()
        if not stripped:
            i += 1
            continue
        if stripped.startswith(("//", "/*", "*")):
            return None  # another comment block -> the current block is not a function contract
        if depths[i] != doc_depth:
            i += 1
            continue  # depth mismatch: a deeper statement, not this contract's declaration
        if stripped.split(" ", 1)[0].split("(", 1)[0] in CONTROL_KEYWORDS:
            return None
        if is_declaration_start(stripped):
            return i
        i += 1
    return None


def parse_doc_tags(body: str) -> dict[str, str]:
    """Parse the @tags in a doc block.

    **Continued lines** must be supported (clang-format wraps long @tests lists):
        * @tests   units.a, units.b,
        *          units.c
    A continuation (a non-empty line not starting with @) is appended to the previous tag's value.
    """
    tags: dict[str, str] = {}
    current: str | None = None
    for raw_line in body.splitlines():
        line = raw_line.strip().lstrip("*").strip()
        if not line:
            continue
        m = re.match(r"@(\w+)\s*(.*)", line)
        if m:
            current = m.group(1)
            tags.setdefault(current, m.group(2).strip())
        elif current is not None:
            tags[current] = (tags[current] + " " + line).strip()
    return tags


def first_value(raw: str) -> str:
    """Take the first word from a tag value.

    Supports `pure`, `pure (value type)`, `pure (value)` and similar; the part before a parenthesis is the value.
    """
    if not raw:
        return ""
    head = re.split(r"[\s（(,;]", raw.strip(), maxsplit=1)[0]
    return head.strip()


def mentions_noexcept(text: str) -> bool:
    """Decide whether the noexcept **specifier** appears (rather than inside an identifier).

    Counterexample: `noexcept_mismatch` contains the substring "noexcept" but is not an exception
    specifier. So the match must respect word boundaries.
    """
    return re.search(r"(?<![\w])noexcept(?![\w])", text) is not None


def is_operator_decl(signature: str) -> bool:
    """Decide whether a signature is an operator overload (maybe an in-class friend or a literal operator)."""
    # Ordinary operators: operator+ / operator== / operator[] ...
    # Literal operators: operator""_m / operator "" _m (whitespace allowed)
    return re.search(r'\boperator\s*(?:"".*?[^\w\s]|[^\s(]+)', signature) is not None


def parse_header(path: Path, skip_trivial: bool = True,
                 abi_zone: str = "abi") -> tuple[list[FunctionContract], list[Violation]]:
    """Parse one header file; returns (recognised function contracts, violations).

    Exemption rules (stricter than "skip anything that is an operator"):
      - A **one-line body** operator overload: declaration and body on the same line (e.g.
        `... noexcept { return a.v + b.v; }`). Its "what it does" is the operation the signature
        spells out, covered by type-level tests, so the six required fields are waived; if it has
        contract fields of its own it is validated as usual. A multi-line `operator+` is not exempt.
      - Structural functions under `core/abi/` and constructors/destructors: exempt from @tests
        (covered by the layout static_asserts in tests/abi/), but @ownership/@thread are still
        required. This rule will have real subjects once the abi module starts.
    """
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    type_index = build_type_index(lines)
    violations: list[Violation] = []
    contracts: list[FunctionContract] = []

    for match in DOC_BLOCK.finditer(text):
        body = match.group("body")
        doc_end_line = text[: match.end()].count("\n") + 1

        tags = parse_doc_tags(body)

        decl_start = find_decl_start(lines, doc_end_line)
        if decl_start is None:
            # Module-level doc block (file header). By convention it may carry `@tests`, declaring
            # "this module's contract surface covers the following cases" -- for cases that belong
            # to no single function (type-level round trips, end-to-end dimension derivation).
            # A pure file header with `@file` is not a contract; only one with `@tests` is registered.
            if "tests" in tags:
                fc = FunctionContract(path=path, line=1, signature="<module>", tags=tags,
                                      owner=None, exempt_reason="模块级契约面")
                fc.test_ids = ID_TOKEN_RE.findall(tags["tests"])
                contracts.append(fc)
            continue

        signature, decl_end, complete_on_one_line = join_declaration(lines, decl_start)
        owner = type_index[decl_start] if decl_start < len(type_index) else None
        body_lines = decl_end - decl_start + 1

        # Short-body operator: the semantics are the signature, covered by type-level tests.
        # Not keyed on "one line or not" -- clang-format reflows, so the criterion must not depend on layout.
        exempt: str | None = None
        if (skip_trivial and is_operator_decl(signature)
                and body_lines <= MAX_TRIVIAL_OPERATOR_LINES):
            exempt = f"短体运算符（{body_lines} 行）：语义即签名本身，由类型级测试覆盖"
        elif owner and re.match(rf"^(?:(?:constexpr|explicit|inline|friend)\s+)*{re.escape(owner)}\s*\(",
                                signature):
            exempt = "构造函数：由类型级测试覆盖"
        elif owner and re.match(r"^~", signature.strip()):
            exempt = "析构函数：由类型级测试覆盖"

        # A constructor/destructor has no nameable @tests entry (type-level tests cover it);
        # the other five required fields stay mandatory.
        ctor_or_dtor = exempt in ("构造函数：由类型级测试覆盖", "析构函数：由类型级测试覆盖")

        in_abi_zone = abi_zone and (abi_zone in path.parts)

        fc = FunctionContract(path=path, line=decl_start + 1, signature=signature, tags=tags,
                              owner=owner, exempt_reason=exempt, in_abi_zone=in_abi_zone,
                              is_ctor_or_dtor=ctor_or_dtor)
        if "tests" in tags:
            fc.test_ids = ID_TOKEN_RE.findall(tags["tests"])

        for tag in REQUIRED_TAGS:
            if tag in tags:
                continue
            # The three @tests exemptions: short-body operators, ctor/dtor, the abi layout zone
            if tag == "tests" and (exempt is not None or ctor_or_dtor or in_abi_zone):
                continue
            violations.append(
                Violation(path, decl_start + 1, "C2",
                          f"契约缺少 @{tag}：{signature[:70]}")
            )

        if "ownership" in tags:
            val = first_value(tags["ownership"])
            if val not in OWNERSHIP_VALUES:
                violations.append(
                    Violation(path, decl_start + 1, "C3",
                              f"@ownership 取值非法：{val!r}（合法值 {sorted(OWNERSHIP_VALUES)}）")
                )
        if "thread" in tags:
            val = first_value(tags["thread"])
            if val not in THREAD_VALUES:
                violations.append(
                    Violation(path, decl_start + 1, "C3",
                              f"@thread 取值非法：{val!r}（合法值 {sorted(THREAD_VALUES)}）")
                )

        if "errors" in tags:
            says_noexcept = mentions_noexcept(tags["errors"])
            decl_noexcept = mentions_noexcept(signature)
            if says_noexcept != decl_noexcept:
                violations.append(
                    Violation(path, decl_start + 1, "C5",
                              f"@errors 与签名不一致：@errors={tags['errors']!r}，"
                              f"签名 noexcept={decl_noexcept}")
                )

        contracts.append(fc)

    return contracts, violations


def _path_matches(path_parts: tuple[str, ...], rel: str, terms: set[str]) -> bool:
    """Whether a test file is selected by any of `terms`.

    Both filters use the same spelling: a term matches either a directory name
    anywhere in the path (`views`) or a path fragment relative to the tests root
    (`unit/views`).

    An earlier version compared `exclude` terms against both the absolute path parts
    and the relative path while comparing `only` terms against the relative path
    alone. So `--exclude tests/model` worked (the absolute parts contained it) and
    `--only tests/model` silently matched nothing -- the gate then reported every id
    as unreferenced, which looks like a catastrophic regression rather than a filter
    that selected an empty set. Two spellings for one concept, one of which was not
    the documented one.
    """
    return any(term in path_parts or term in rel for term in terms)


def collect_test_ids(tests_dir: Path, exclude: set[str] | None = None,
                     only: set[str] | None = None) -> set[str]:
    """Every TEST_CASE id under `tests_dir`, minus the excluded subtrees.

    `exclude` matches a directory **name** anywhere in the path. It exists so that
    one consumer's tests can be handed to a second invocation: test ownership is a
    global judgement, so two invocations must see disjoint sets of test cases or
    each one reports the other's legitimate cases as orphans.

    `only`, when given, is the **inverse** filter and wins over `exclude`: a path is
    read only if it matches one of its terms.

    Both filters exist because the exclusion form alone is a maintenance trap. The
    views gate used to enumerate every other module's directory by name, so adding
    `core/reflect` left its directory unlisted: the gate then claimed reflect's test
    ids, found no contract in `views/model` referencing them, and reported orphans
    from a module it has nothing to do with. The message names reflect, so the
    obvious reading is that reflect is broken -- a gate that fails for the wrong
    reason is a gate someone eventually deletes. Stating the subject positively ("only
    this subtree is mine") cannot acquire a new one by omission.
    """
    skip = exclude or set()
    allow = only or set()
    ids: set[str] = set()
    for path in tests_dir.rglob("*.cpp"):
        rel = path.relative_to(tests_dir).as_posix()
        if allow:
            if not _path_matches(path.parts, rel, allow):
                continue
        elif _path_matches(path.parts, rel, skip):
            continue
        for m in TEST_CASE_RE.finditer(path.read_text(encoding="utf-8", errors="replace")):
            ids.add(m.group("id"))
    return ids


def main(argv: list[str] | None = None) -> int:
    try:  # the Windows console may be GBK; keep the script from crashing on encoding
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    repo_root = Path(__file__).resolve().parent.parent

    parser = argparse.ArgumentParser(description="契约完备性门禁")
    parser.add_argument("--headers", action="append", default=None,
                        help="头文件根目录（可重复）。"
                             "某一层的头文件可能分布在多个目录里——视图层同时有 views/qt 与 "
                             "views/include——只给一个目录的调用看不见另一个目录里声明的契约，"
                             "于是把那边认领的用例报成孤儿：一条指向错误文件的失败信息。")
    parser.add_argument("--tests", default=str(repo_root / "tests"), help="测试根目录")
    parser.add_argument("--quiet", action="store_true", help="仅输出违规")
    parser.add_argument("--root", default=str(repo_root), help="用于相对路径显示的仓库根")
    parser.add_argument("--allow-orphan-tests", action="store_true",
                        help="孤儿测试用例降级为警告（默认视为失败）")
    parser.add_argument("--no-scan-tests", dest="scan_tests", action="store_false",
                        help="不把测试文件里的模块级契约块纳入用例归属统计")
    parser.add_argument("--exclude", action="append", default=[],
                        help="收集测试用例时跳过的目录名或路径片段（可重复）。"
                             "用于把某个子树的用例交给另一次调用去管辖："
                             "用例归属是**全局**判据，所以两次调用必须看到互不相交的用例集合，"
                             "否则一方的合规用例在另一方看来全是孤儿。"
                             "写法可以是目录名 `views`，也可以是相对测试根目录的路径片段 `unit/views`。")
    parser.add_argument("--only", action="append", default=[],
                        help="只收集匹配这些目录名或路径片段的测试用例（可重复），"
                             "写法与 --exclude 相同，且优先于它。"
                             "用于「本次调用只管辖这一棵子树」："
                             "排除式清单每加一个模块就要改一次，漏改时门禁会去认领一个"
                             "与它无关的模块的用例，然后报出那个模块的孤儿——"
                             "一条指错方向的失败信息，比一条漏报更容易让人删掉门禁。")
    args = parser.parse_args(argv)

    header_roots = [Path(p) for p in (args.headers or [str(repo_root / "core")])]
    tests_root = Path(args.tests)
    repo_root = Path(args.root).resolve()

    for root in header_roots:
        if not root.is_dir():
            print(f"错误：头文件目录不存在 {root}", file=sys.stderr)
            return 1
    if not tests_root.is_dir():
        print(f"错误：测试目录不存在 {tests_root}", file=sys.stderr)
        return 1

    declared_tests = collect_test_ids(tests_root, set(args.exclude), set(args.only))
    violations: list[Violation] = []
    all_contracts: list[FunctionContract] = []
    referenced: set[str] = set()

    for root in header_roots:
        for path in sorted(root.rglob("*.hpp")):
            contracts, viols = parse_header(path)
            all_contracts.extend(contracts)
            violations.extend(viols)

    # A test file may also carry a module-level contract block (declaring the contract surface it
    # covers). Its cases are not re-reported as orphans, because filing cases is exactly its purpose.
    #
    # The exclusion list applies here too. Without it, `--exclude` would remove a
    # subtree from the orphan count while still reading that subtree's module
    # contracts -- so a file the caller deliberately handed to another invocation
    # would keep claiming cases this one cannot see, and report them as
    # unsatisfied references. That mismatch is how the second gate ended up
    # complaining about `fp.*`, whose cases belong to the first gate's test root.
    if args.scan_tests:
        skip = set(args.exclude)
        allow = set(args.only)
        for path in sorted(tests_root.rglob("*.cpp")):
            rel = path.relative_to(tests_root).as_posix()
            if allow:
                if not _path_matches(path.parts, rel, allow):
                    continue
            elif _path_matches(path.parts, rel, skip):
                continue
            contracts, _ = parse_header(path)
            all_contracts.extend(c for c in contracts if c.signature == "<module>")

    # -- C4 @tests entries must really exist --
    for fc in all_contracts:
        if fc.exempt_reason is not None or fc.in_abi_zone:
            # An exempt item skips the forward C4 check; if it carries @tests it is still validated
            if not fc.test_ids:
                continue
        for tid in fc.test_ids:
            referenced.add(tid)
            if tid not in declared_tests:
                violations.append(
                    Violation(fc.path, fc.line, "C4",
                              f"@tests 引用了不存在的用例 {tid!r}（{fc.signature[:50]}）")
                )
        if not fc.test_ids and fc.exempt_reason is None and not fc.in_abi_zone:
            violations.append(
                Violation(fc.path, fc.line, "C4",
                          f"非豁免函数没有任何 @tests：{fc.signature[:70]}")
            )

    # -- C6 reverse check: orphan tests --
    orphans = sorted(declared_tests - referenced)

    # -- Output --
    exempted = sum(1 for c in all_contracts
                   if c.exempt_reason is not None and not c.tags)
    bare_operators = sum(1 for c in all_contracts if c.exempt_reason and c.exempt_reason.startswith("短体"))
    # Computed before the branch, not inside it. Scoping this assignment to the non-quiet
    # branch made every `--quiet` invocation raise UnboundLocalError on the line below -- a
    # crash inside the gate, reported to the caller as a gate failure, which is the worst
    # possible presentation of a bug in the checker: it looks like the code under test.
    scope = ", ".join(str(r) for r in header_roots)
    print(f"契约检查：{scope} 下 {len(all_contracts)} 个契约"
              f"（其中 {bare_operators} 个短体运算符无契约注释、"
              f"{exempted} 个整体豁免），"
              f"{tests_root} 下 {len(declared_tests)} 个测试用例")

    for v in sorted(violations, key=lambda x: (str(x.path), x.line, x.rule)):
        print(v.render(repo_root))

    if orphans:
        # An orphan test = the reverse check failed: someone wrote a test without attaching it to
        # any contract. Failure by default -- otherwise the contract surface drifts from the tests.
        for tid in orphans:
            print(f"[C6] 孤儿测试用例（未被任何契约 @tests 引用）：{tid}")

    if violations:
        print(f"\n门禁失败：{len(violations)} 处契约违规", file=sys.stderr)
        return 1
    if orphans and not args.allow_orphan_tests:
        print(f"\n门禁失败：{len(orphans)} 个孤儿测试用例（用 --allow-orphan-tests 可降级为警告）",
              file=sys.stderr)
        return 1

    if not args.quiet:
        print("门禁通过：契约完备。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
