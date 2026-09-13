# The port's oracle: the original Fortran, and how to measure a port against it

**What this directory is.** The tracked half of the oracle arrangement. The original Fortran lives **outside version
control**, in `external/geopack/` -- that directory is git-ignored in this tree, which is where downloaded dependencies
go, and it suits these files for a second reason: they are third-party research code **with no licence statement**, and
a repository should not redistribute what it has no permission to redistribute. What *is* tracked here is the record:
what each file is, its hash, how to compile it, what it proved, and the tests that use it (which **skip** when the
sources are absent, so a fresh clone builds and passes without them).

**The arrangement.** `external/geopack/` holds the sources, byte-for-byte as they were distributed, together with the
file `TA16_RBF.f` needs at run time (`TA16_RBF.par`). Compiled here, on this machine, they are the **oracle**: a model
ported into this tree is measured against *these* routines, call by call -- rather than against numbers copied out of a
paper or a port of a port.

They are **not part of the shipped application**. Nothing in `core/`, `plugins/`, `views/` or `runtime/` includes them;
the only consumer is the test build, behind a CMake option, and the shipped binary is built without it. That is what
keeps a research code with no formal licence statement out of the product while still letting the port be verified
against it (see "Terms" below).

## Contents

| File | What it is | Entry points that matter here |
| --- | --- | --- |
| `Geopack-2008_dp.for` | The utility layer, double precision: date and solar-wind state, coordinate systems, the IGRF, and the field-line tracer | `RECALC_08`, `IGRF_GSW_08`, `IGRF_GEO_08`, `DIP_08`, `SUN_08`, `GSWGSE_08`, `GEOMAG_08`, `MAGSM_08`, `SMGSW_08`, `GEOGSW_08`, `GEIGEO_08`, `GEODGEO_08`, `SPHCAR_08`, `BSPCAR_08`, `BCARSP_08`, `RHAND_08`, `STEP_08`, `TRACE_08`, `SHUETAL_MGNP_08`, `T96_MGNP_08` |
| `TS04c.for` | The storm-time model TS04, whose coefficients TS05 shares; a ring current, a tail, a Birkeland system and a shielding field | `T04_s(IOPT, PARMOD, PS, X, Y, Z, BX, BY, BZ)` |
| `TA16_RBF.f` | The 2016 data-based RBF model (Tsyganenko and Andreeva 2016); reads its coefficients from `TA16_RBF.par` | `RBF_MODEL_2016(IOPT, PARMOD, PS, X, Y, Z, BX, BY, BZ)` |
| `TA16_RBF.par` | 23328 linear coefficients, fixed-format, read by the routine above | data, not code |
| `parabmod.for` | The dynamic paraboloid model (Alexeev, Moscow State University, 2000/2002) | its own driver routines |
| `MS_field_model.for` | The magnetosheath field model (bow shock after Lu et al. 2019, magnetopause after Lin et al. 2010) | `MAGNETOSHEATH_B` |

**The three classic models are not here.** `T89`, `T96` and `T01` are distributed as separate files and were not among
the sources provided; the MIT-licensed Python package `geopack` 1.0.13 (a transliteration of the same Fortran, present
in the reference implementation's virtual environment) covers `t89`, `t96`, `t01` and `t04`, and is the oracle for
those four until the Fortran arrives.

## Verbatim

These files must not be edited -- an oracle that has been touched is not an oracle. Their hashes, so that a later
reader can prove the copies here are the copies that were handed over:

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `Geopack-2008_dp.for` | 85218 | `e9341bf216f9a45aeb9ea79b7841673993f223203233e2c28c4e60c9835eda2a` |
| `TS04c.for` | 85005 | `8b3962c8cdd8be6d16deedf618c51380d7b69322bb22a943a60fd60b2856d30d` |
| `TA16_RBF.f` | 12597 | `a697a8fb997845bb40354f0441061b22fd722881bb6b6672d4f2cae15c3720cc` |
| `TA16_RBF.par` | 396675 | `d970229b301e2989d1d905915f127e15eb6b8186f4c626848e7b35ec73cae00c` |
| `parabmod.for` | 72751 | `1def066678a85ad3ecb5b68211c44b58c7c9afed303939c00de425c3e8eb79aa` |
| `MS_field_model.for` | 43787 | `6ec4ae303565b20680fa6b67ee874fddf065c1a6ef24e09a65a1e2a8b0018aef` |

## Compiling it here

    gfortran -std=legacy -fno-automatic -cpp -O2 -c <file>.for

The path convention is `external/geopack/<file>`, and the sources were placed there by the project owner; the hashes
below are what a caller can check them against.

Five things are measured rather than assumed, and each of them cost a build to find:

1. **`-fno-automatic` is required.** This generation of Fortran keeps its state in `COMMON` and in static local
   variables; an automatic-local build silently resets that state between calls, and the fields come out wrong in a
   way that looks like a physics bug.
2. **Leave the fixed-form line length at its default.** `-ffixed-line-length-none` looks like a convenience and breaks
   `TS04c.for` at line 1852, which carries text past column 72 that the original build ignored. The original sources
   are compiled as the original build compiled them.
3. **Drivers written here go in free form** (`.f90`). A three-line experiment then does not have to respect column 7
   or column 72, and the port's own test drivers are easier to read.
4. **Fields come back in nanotesla, positions go in earth radii.** `IGRF_GSW_08(3,0,0)` returns about 1065, 38 and 1017
   -- nanotesla, not tesla. This kit works in tesla throughout, so the port converts once, at its own boundary, and the
   conversion is asserted rather than remembered.
5. **The state block is `COMMON /GEOPACK1/ AA(10),SPS,CPS,BB(22)`** -- declared exactly as `RECALC_08` declares it
   (line 318 of the source). It holds the **sine** and **cosine** of the dipole tilt, not the angle: a reader who
   assumes an angle, or a longer layout, gets a plausible-looking wrong number. The first version of the smoke driver
   in this directory did exactly that and read a tilt of zero for every date.

A worked proof that the oracle links and runs -- `oracle_smoke.f90`, in this directory -- prints the IGRF at three
earth radii, the tilt Geopack derives for a given date, and the TS04 field in the tail for a storm-time driver set.
Compiled and run from the repository root:

    gfortran -std=legacy -fno-automatic -cpp -O2 -o build/oracle_smoke.exe \
        external/geopack/Geopack-2008_dp.for external/geopack/TS04c.for tests/oracle/oracle_smoke.f90
    ./build/oracle_smoke.exe

Its output on 2024-01-01 was a tilt of **-25.38 degrees** (the real tilt for that date) and a tail field of
**(59.8, 4.0, -32.5) nT** at six earth radii down the tail -- a magnetotail of the right magnitude, and a field that
**changes with the tilt**, which is what distinguishes an oracle from a table.

## Terms

**The sources carry no licence statement.** Searching all six for "licence", "copyright", "permission" or an
acknowledgement finds nothing; what they do carry is the authorship and the papers to cite (Tsyganenko 2008 for
Geopack; Tsyganenko and Andreeva 2016 for the RBF model; Alexeev for the paraboloid model; Lu et al. 2019 and Lin et
al. 2010 for the magnetosheath model). The terms of use are therefore **not established by these files**, and this
directory records that rather than inventing a permission: the files were provided by the project owner, they are used
here to verify a port, and whether a distribution may ship them alongside the ported code is a decision for the owner
-- recorded as open, in `docs/plan-tree.md`.

What this directory does establish is the boundary: the oracle is **test-only**. The port itself is written from
scratch in C++, has no Fortran dependency, and ships without any of this.
