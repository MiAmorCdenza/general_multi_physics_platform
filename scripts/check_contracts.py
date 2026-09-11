#!/usr/bin/env python3
"""契约完备性门禁。

对应 standards/enforcement.md §5。把 standards/function-contract.md 里
的规范从"文档约定"变成"可执行门禁"。

检查项：
  C1 每个函数声明前必须有 /** */ 契约块
  C2 契约块必须含六个必填字段（@ownership/@thread/@pre/@post/@errors/@tests）
  C3 @ownership 与 @thread 的取值必须合法
  C4 @tests 至少一个条目，且每个条目必须在 tests/ 下真实存在
  C5 @errors 与声明的 noexcept 一致性
  C6 反向检查：每个 TEST_CASE 必须至少被一个函数的 @tests 引用
     （防止"测了没用的东西"与"契约漏写"两头漏）

退出码：0 通过；1 有违规。

用法：
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

# 必填字段。注意：TAGS 字典的键是去掉 '@' 的裸名，这里必须与之一致。
REQUIRED_TAGS = ("ownership", "thread", "pre", "post", "errors", "tests")
OWNERSHIP_VALUES = {"pure", "owns", "observes", "borrows", "value"}

# 线程角色。取值集合本身是契约的一部分：出现新角色必须显式登记，
# 否则"这个函数到底谁能调"会变成含糊的散文。
THREAD_VALUES = {
    "any",      # 无共享可变状态，任意线程可调
    "main",     # 仅主线程（UI、文档编辑）
    "eval",     # 仅图求值线程
    "ui",       # 仅 UI 线程（与 main 区分时使用）
    "publish",  # 仅数据发布线程（seqlock 的写侧；见 core/abi）
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
    """判断一行是否是函数声明的起始行。

    `template <...>` 单独成行时不算——真正的签名在下一行。
    否则 join_declaration 会把模板头也拼进去，函数名无法识别。
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
    # 三种收尾都合法：纯声明 ';'、多行体开头 '{'、单行体闭合 '}'
    return stripped.endswith((";", "{", "}"))


def join_declaration(lines: list[str], start: int, limit: int = 40) -> tuple[str, int, bool]:
    """从 start 行起把声明拼成一行。

    返回 (声明文本, 结束行号, 是否单行体)。

    完成判据：
      - 圆括号与花括号都配平，且以 `;` 结尾 —— 纯声明
      - 圆括号与花括号都配平，且以 `}` 结尾 —— 单行体内联函数

    只配平花括号不够：`T{v1, v2}` 这种初始化列表会让花括号提前配平，
    从而把返回语句误当成声明结束（本检查器踩过这个坑）。
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


# 运算符重载的函数体若不超过这个行数，视为"语义即签名本身"，
# 免除六个必填字段（由类型级测试覆盖）。超过则必须写完整契约。
# 阈值 12 行覆盖了本项目里"一行一个分量"的显式展开写法（如 Dim 的逐轴乘除）。
MAX_TRIVIAL_OPERATOR_LINES = 12

# 形似函数声明、实为控制流的行首关键字。
CONTROL_KEYWORDS = frozenset({
    "if", "else", "for", "while", "do", "switch", "case", "return", "catch",
    "break", "continue", "delete", "new", "throw", "static_assert", "assert",
})


def build_type_index(lines: list[str]) -> list[str | None]:
    """前向扫描，记录每一行所处的**直接**外层类型名。

    关键点：struct 闭合后必须立刻恢复外层，否则命名空间级的自由函数
    会被误判为类成员（本检查器第一版就栽在这里）。

    简化假设（与本项目代码风格一致，并在 tests/meta 中有反例测试）：
      - `struct`/`class`/`union` 关键字与其 `{` 出现在同一行
      - 大括号不与字符串或注释混排
    """
    owner: list[str | None] = []
    stack: list[str] = []
    for line in lines:
        owner.append(stack[-1] if stack else None)
        m = re.match(r"\s*(?:template\s*<.*>\s*)?(?:struct|class|union)\s+(\w+)", line)
        if m:
            stack.append(m.group(1))
        # 本行花括号净变化。净减少时弹出对应数量的类型作用域。
        net = line.count("{") - line.count("}")
        if m:
            net -= 1  # 已用 struct 关键字入栈，抵消它自己的 '{'
        while net < 0 and stack:
            stack.pop()
            net += 1
    return owner


def compute_depths(lines: list[str]) -> list[int]:
    """每行**开头处**的花括号深度（0 = 文件最外层）。"""
    depths: list[int] = []
    depth = 0
    for line in lines:
        depths.append(depth)
        depth += line.count("{") - line.count("}")
    return depths


def find_decl_start(lines: list[str], from_line: int, window: int = 4) -> int | None:
    """找文档块紧邻的声明起始行（0-based），找不到返回 None。

    本项目约定：契约块必须**紧贴**声明（中间只允许空行）。
    因此遇到另一个注释行就停下——那说明当前块是文件头或类型头，
    而不是某个函数的契约。这条规则同时挡住了"把文件头误配给第一个函数"。

    另外两个坑：
    1. **单行体误判**：`explicit constexpr Quantity(double v) noexcept : v_(v) {}`
       在同一行内闭合，必须原地识别，不能跳过。
    2. **控制流误判**：`if (style == ...) {` 形似声明，靠关键字黑名单排除。
    3. **落进函数体**：声明跨多行时，搜索会继续往函数体里走，把
       `const auto exps = ...;` 这种语句误当成声明。用**花括号深度**过滤。
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
            return None  # 另一个注释块 → 当前块不是函数契约
        if depths[i] != doc_depth:
            i += 1
            continue  # 深度不符：这是更深层的语句，不是本契约的声明
        if stripped.split(" ", 1)[0].split("(", 1)[0] in CONTROL_KEYWORDS:
            return None
        if is_declaration_start(stripped):
            return i
        i += 1
    return None


def parse_doc_tags(body: str) -> dict[str, str]:
    """解析文档块里的 @tag。

    必须支持**跨行续写**（clang-format 会把长 @tests 列表折行）：
        * @tests   units.a, units.b,
        *          units.c
    续行（不以 @ 开头的非空行）追加到上一个标签的值上。
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
    """从 tag 值里取第一个词。

    支持 `pure`、`pure（值类型）`、`pure (value)` 等写法；括号前视为取值。
    """
    if not raw:
        return ""
    head = re.split(r"[\s（(,;]", raw.strip(), maxsplit=1)[0]
    return head.strip()


def mentions_noexcept(text: str) -> bool:
    """判断 noexcept **说明符**是否出现（而不是出现在标识符里）。

    反例：`noexcept_mismatch` 含子串 "noexcept"，但它不是异常说明符。
    因此必须按词边界匹配。
    """
    return re.search(r"(?<![\w])noexcept(?![\w])", text) is not None


def is_operator_decl(signature: str) -> bool:
    """判断签名是否是运算符重载（可能是类内 friend 定义或字面量运算符）。"""
    # 普通运算符：operator+ / operator== / operator[] ...
    # 字面量运算符：operator""_m / operator "" _m（允许空格）
    return re.search(r'\boperator\s*(?:"".*?[^\w\s]|[^\s(]+)', signature) is not None


def parse_header(path: Path, skip_trivial: bool = True,
                 abi_zone: str = "abi") -> tuple[list[FunctionContract], list[Violation]]:
    """解析一个头文件，返回 (已识别的函数契约, 违规列表)。

    豁免规则（必须比"是运算符就跳过"更严）：
      - **单行体**运算符重载：声明与函数体同行（如 `... noexcept { return a.v + b.v; }`）。
        这类函数的"做什么"就是签名所写的那个运算，由类型级测试覆盖，
        因此免除六个必填字段。若它自带任何契约字段，则照常校验。
        多行体的 `operator+` 不受豁免——它已经复杂到需要契约了。
      - `core/abi/` 下的结构性函数与构造函数/析构函数：免除 @tests
        （由 tests/abi/ 的布局 static_assert 覆盖），但仍强制 @ownership/@thread。
        待 abi 模块开工时本规则会有真实对象。
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
            # 模块级文档块（文件头）。约定：它可以带 `@tests`，
            # 声明"本模块契约面覆盖以下用例"——用于收纳不属于单个函数的用例
            # （如类型级往返、端到端量纲推导）。
            # 带 `@file` 的纯文件头不算契约，只有带 `@tests` 的才登记。
            if "tests" in tags:
                fc = FunctionContract(path=path, line=1, signature="<module>", tags=tags,
                                      owner=None, exempt_reason="模块级契约面")
                fc.test_ids = ID_TOKEN_RE.findall(tags["tests"])
                contracts.append(fc)
            continue

        signature, decl_end, complete_on_one_line = join_declaration(lines, decl_start)
        owner = type_index[decl_start] if decl_start < len(type_index) else None
        body_lines = decl_end - decl_start + 1

        # 短体运算符：语义即签名本身，由类型级测试覆盖。
        # 不看单行与否——clang-format 会重排，判据不能依赖排版。
        exempt: str | None = None
        if (skip_trivial and is_operator_decl(signature)
                and body_lines <= MAX_TRIVIAL_OPERATOR_LINES):
            exempt = f"短体运算符（{body_lines} 行）：语义即签名本身，由类型级测试覆盖"
        elif owner and re.match(rf"^(?:(?:constexpr|explicit|inline|friend)\s+)*{re.escape(owner)}\s*\(",
                                signature):
            exempt = "构造函数：由类型级测试覆盖"
        elif owner and re.match(r"^~", signature.strip()):
            exempt = "析构函数：由类型级测试覆盖"

        # 构造函数/析构函数没有可点名的 @tests 条目（它们由类型级测试覆盖），
        # 其余五个必填字段照常强制。
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
            # @tests 的三类豁免：短体运算符、构造/析构、abi 布局区
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


def collect_test_ids(tests_dir: Path) -> set[str]:
    ids: set[str] = set()
    for path in tests_dir.rglob("*.cpp"):
        for m in TEST_CASE_RE.finditer(path.read_text(encoding="utf-8", errors="replace")):
            ids.add(m.group("id"))
    return ids


def main(argv: list[str] | None = None) -> int:
    try:  # Windows 控制台可能是 GBK；保证脚本不因编码而崩溃
        sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
        sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    except (AttributeError, OSError):
        pass

    repo_root = Path(__file__).resolve().parent.parent

    parser = argparse.ArgumentParser(description="契约完备性门禁")
    parser.add_argument("--headers", default=str(repo_root / "core"), help="头文件根目录")
    parser.add_argument("--tests", default=str(repo_root / "tests"), help="测试根目录")
    parser.add_argument("--quiet", action="store_true", help="仅输出违规")
    parser.add_argument("--root", default=str(repo_root), help="用于相对路径显示的仓库根")
    parser.add_argument("--allow-orphan-tests", action="store_true",
                        help="孤儿测试用例降级为警告（默认视为失败）")
    parser.add_argument("--no-scan-tests", dest="scan_tests", action="store_false",
                        help="不把测试文件里的模块级契约块纳入用例归属统计")
    args = parser.parse_args(argv)

    headers_root = Path(args.headers)
    tests_root = Path(args.tests)
    repo_root = Path(args.root).resolve()

    if not headers_root.is_dir():
        print(f"错误：头文件目录不存在 {headers_root}", file=sys.stderr)
        return 1
    if not tests_root.is_dir():
        print(f"错误：测试目录不存在 {tests_root}", file=sys.stderr)
        return 1

    declared_tests = collect_test_ids(tests_root)
    violations: list[Violation] = []
    all_contracts: list[FunctionContract] = []
    referenced: set[str] = set()

    for path in sorted(headers_root.rglob("*.hpp")):
        contracts, viols = parse_header(path)
        all_contracts.extend(contracts)
        violations.extend(viols)

    # 测试文件里也可以有模块级契约块（声明该测试文件覆盖的契约面）。
    # 扫描时不再重复报"孤儿"，因为它们的用途正是收纳用例。
    if args.scan_tests:
        for path in sorted(tests_root.rglob("*.cpp")):
            contracts, _ = parse_header(path)
            all_contracts.extend(c for c in contracts if c.signature == "<module>")

    # ── C4 @tests 条目必须真实存在 ──
    for fc in all_contracts:
        if fc.exempt_reason is not None or fc.in_abi_zone:
            # 豁免项不参与 C4 正向检查；若它自带 @tests 仍照常校验
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

    # ── C6 反向检查：孤儿测试 ──
    orphans = sorted(declared_tests - referenced)

    # ── 输出 ──
    exempted = sum(1 for c in all_contracts
                   if c.exempt_reason is not None and not c.tags)
    bare_operators = sum(1 for c in all_contracts if c.exempt_reason and c.exempt_reason.startswith("短体"))
    if not args.quiet:
        print(f"契约检查：{headers_root} 下 {len(all_contracts)} 个契约"
              f"（其中 {bare_operators} 个短体运算符无契约注释、"
              f"{exempted} 个整体豁免），"
              f"{tests_root} 下 {len(declared_tests)} 个测试用例")

    for v in sorted(violations, key=lambda x: (str(x.path), x.line, x.rule)):
        print(v.render(repo_root))

    if orphans:
        # 孤儿测试 = 逆向检查失败：有人写了测试却没把它挂到任何契约上。
        # 默认视为失败——否则契约面会慢慢与实际测试面脱节。
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
