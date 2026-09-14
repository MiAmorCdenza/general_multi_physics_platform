#!/usr/bin/env python3
"""Generate the IGRF coefficient tables in C++ from the Fortran that defines them.

**Why a generator rather than a transcription.** The tables are 24 arrays of 105 numbers plus two of 45 -- about 2700
values, each of which the field depends on exactly. Typing them by hand is a way to introduce a wrong digit that no
compiler, no linter and no review catches, and the failure it produces is a field that is subtly wrong everywhere.
Reading them out of the source that the port is measured against removes the class of mistake, and the script is
committed so the result can be regenerated and compared.

The input is `external/geopack/Geopack-2008_dp.for`, which is **not** in the repository (third-party research code
with no licence statement; see `tests/oracle/README.md`). When it is absent the script says so and exits non-zero --
the generated file is committed, so a build never needs the Fortran.

    python scripts/generate_igrf_table.py            # writes the header
    python scripts/generate_igrf_table.py --check     # fails if the header is out of date
"""
from __future__ import annotations

import argparse
import io
import re
import sys
from pathlib import Path

SOURCE = Path("external/geopack/Geopack-2008_dp.for")
OUTPUT = Path("plugins/magnetosphere/include/qp/plugins/magnetosphere/igrf_table.hpp")

# The epochs, in the order the Fortran's own interpolation blocks use them. The names are the Fortran's.
EPOCHS = ["65", "70", "75", "80", "85", "90", "95", "00", "05", "10", "15", "20"]
# The secular-variation arrays, and how long each is: degrees up to 8 reach 45 coefficients.
#
# **`DG15`/`DH15` are deliberately absent.** They appear in the Fortran's `DIMENSION` list and nowhere else -- no
# `DATA` statement defines them, and no interpolation block reads them; the 2015-2020 span is interpolated between
# the 2015 and 2020 epochs like every span before it. They are vestigial declarations, and a generator that demanded
# data for them would be inventing a table the model does not have.
VARIATIONS = {"DG20": 45, "DH20": 45}


def _parse_fortran_data(text: str, name: str) -> list[float]:
    """Read one `DATA <name>/.../` statement, following its continuation lines."""
    lines = text.split("\n")
    values: list[float] = []
    collecting = False
    for line in lines:
        # Fixed form: a continuation line carries a non-blank, non-zero in column 6.
        body = line[6:] if len(line) > 6 else ""
        if not collecting:
            # `DATA G65/0.D0,-30334.D0,...` -- the statement may also be `DATA G65 /.../` with a space.
            match = re.match(rf"\s*DATA\s+{re.escape(name)}\s*/(.*)", line, re.IGNORECASE)
            if match is None:
                # A bare continuation marker in the statement field means the previous statement continues.
                continue
            collecting = True
            body = match.group(1)
        else:
            if not line[:6].strip() and line[:1] in ("", " ", "\t") and not line.strip().startswith(("+", "*", "&")):
                break  # the continuation ended
        closed = body.find("/")
        if closed >= 0:
            body = body[:closed]
            collecting = False
        for token in body.split(","):
            token = token.strip()
            if not token:
                continue
            # Fortran's repeat syntax: `39*0.E0` is thirty-nine zeros, and the IGRF tables use it heavily for the
            # coefficients that are identically zero (every `H` term with m = 0, and the whole m > n triangle).
            repeat = 1
            counted = re.match(r"^(\d+)\*(.+)$", token)
            if counted is not None:
                repeat = int(counted.group(1))
                token = counted.group(2).strip()
            token = token.replace("D", "E").replace("d", "e")
            # Fixed-form DATA separates fields with commas, so a space inside a number is layout: `- 8.4E0` is
            # minus eight point four, and the tables are full of it because the columns were lined up in 1970.
            token = token.replace(" ", "")
            token = token.rstrip("Ee") if token.endswith(("E", "e")) else token
            if token in ("+", "-"):
                continue
            try:
                value = float(token)
            except ValueError:
                raise SystemExit(f"cannot read {token!r} in the DATA statement for {name}")
            values.extend([value] * repeat)
        if not collecting:
            break
    return values


def _table(name: str, values: list[float], width: int, cpp_name: str) -> str:
    out = [f"/// @brief The {name} epoch table, read out of the Fortran's own DATA statement."]
    out.append(f"inline constexpr std::array<double, {width}> {cpp_name}{{")
    for index in range(0, len(values), 6):
        chunk = ", ".join(f"{value:.1f}" for value in values[index:index + 6])
        out.append(f"    {chunk},")
    out.append("};")
    return "\n".join(out)


def build(text: str) -> str:
    parts: list[str] = []
    for epoch in EPOCHS:
        for letter in ("G", "H"):
            name = f"{letter}{epoch}"
            values = _parse_fortran_data(text, name)
            expected = 105
            if len(values) != expected:
                raise SystemExit(f"{name}: read {len(values)} values, expected {expected}")
            parts.append(_table(name, values, expected, f"k{name}"))
    for name, width in VARIATIONS.items():
        values = _parse_fortran_data(text, name)
        if len(values) != width:
            raise SystemExit(f"{name}: read {len(values)} values, expected {width}")
        parts.append(_table(name, values, width, f"k{name}"))

    body = "\n\n".join(parts)
    return f'''/**
 * @file igrf_table.hpp
 * @brief The IGRF coefficient tables, **generated** from the defining Fortran by `scripts/generate_igrf_table.py`.
 *
 * Do not edit this file by hand. Every number here is a value the field depends on exactly, and a transcription
 * mistake in one of them produces a field that is subtly wrong everywhere -- a failure no compiler and no review
 * catches. The generator reads the `DATA` statements of `external/geopack/Geopack-2008_dp.for` (the source the port
 * is measured against) and this file is regenerated and compared by `--check`.
 *
 * The epochs are the ones the Fortran's own interpolation uses, 1965 through 2020, with the two secular-variation
 * sets that cover the years after each: 2015-2020 through `DG15`/`DH15`, and everything after 2020 by extrapolating
 * with `DG20`/`DH20`. Both variation sets stop at degree 8, which is why they hold 45 values where an epoch holds 105.
 *
 * Units are nanotesla, as the IGRF is published and as Geopack carries them; the port converts to tesla at its own
 * boundary.
 *
 * @ownership   owns
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every table holds the values of its `DATA` statement, in the Fortran's own order
 * @errors      none
 * @frozen      no
 * @tests       geopack.igrf.the_generated_table_holds_the_published_epochs
 */
#pragma once

#include <array>

namespace qp::plugins::magnetosphere::igrf {{

{body}

}}  // namespace qp::plugins::magnetosphere::igrf
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="verify the committed header instead of writing it")
    args = parser.parse_args()

    if not SOURCE.exists():
        print(f"the Fortran source is not present at {SOURCE}; see tests/oracle/README.md", file=sys.stderr)
        return 2
    text = io.open(SOURCE, encoding="utf-8", errors="replace", newline="").read()
    generated = build(text)

    if args.check:
        current = io.open(OUTPUT, encoding="utf-8", newline="").read() if OUTPUT.exists() else ""
        if current != generated:
            print(f"{OUTPUT} is out of date; run scripts/generate_igrf_table.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT} matches the Fortran")
        return 0

    io.open(OUTPUT, "w", encoding="utf-8", newline="\n").write(generated)
    print(f"wrote {OUTPUT}: {len(EPOCHS) * 2} epoch tables and {len(VARIATIONS)} variation tables")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
