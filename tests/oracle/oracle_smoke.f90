! SMOKE TEST FOR THE VENDORED ORACLE SOURCES (free form; the vendored sources stay in default fixed form).
!
! Two things are being established, and neither is about the physics yet:
!
!   1. that Geopack-2008 and the model routines **link and run** when compiled with gfortran in this tree, so a
!      ported model can be checked against the original on this machine rather than against published numbers;
!   2. that the calling convention the port has to reproduce is the one the sources actually use -- `RECALC_08`
!      establishes the state block (date, solar wind), the coordinate transforms read it, and a model takes
!      `(IOPT, PARMOD, PS, X, Y, Z, ...)` with `PS` the dipole tilt and `PARMOD(1..10)` the drivers.
!
! The numbers are checked for being finite and of the right order, not for being right: rightness is what the port
! will be measured against, call by call.
program oracle_smoke
  implicit none
  double precision :: parmod(10), bx, by, bz, hx, hy, hz, ps, tilt
  double precision, external :: ps_tilt_deg

  ! 2024, day 1, 00:00 UT, solar wind 400 km/s along -x in GSE.
  call recalc_08(2024, 1, 0, 0, 0, -400.d0, 0.d0, 0.d0)

  ! The internal field three earth radii out on the equator, in GSW: the IGRF, which is what the dipole-only
  ! internal field in this kit approximates.
  call igrf_gsw_08(3.d0, 0.d0, 0.d0, hx, hy, hz)
  write (*, '(A,3ES16.6)') ' IGRF_GSW_08(3,0,0)  = ', hx, hy, hz
  write (*, '(A,F10.2)') '   magnitude (nT)    = ', sqrt(hx*hx + hy*hy + hz*hz) * 1.d9

  ! The dipole tilt Geopack derived from the date: what every external model is parameterised by.
  tilt = ps_tilt_deg()
  write (*, '(A,F10.4)') '   tilt from RECALC  = ', tilt

  ! TS04 -- the model whose coefficients TS05 shares -- in a storm-time state: Pdyn 2 nPa, Dst -100 nT, By 5 nT,
  ! Bz -10 nT, and the remaining drivers as its own documentation prescribes.
  parmod = (/ 2.d0, -100.d0, 5.d0, -10.d0, 0.5d0, 0.5d0, 0.5d0, 0.5d0, 0.5d0, 0.5d0 /)
  ps = tilt / 57.29577951308232d0
  call t04_s(0, parmod, ps, -6.d0, 0.d0, 0.d0, bx, by, bz)
  write (*, '(A,3ES16.6)') ' T04_s(-6,0,0)       = ', bx, by, bz
  write (*, '(A,F10.2)') '   magnitude (nT)    = ', sqrt(bx*bx + by*by + bz*bz)

  write (*, '(A)') ' oracle smoke test ran'
end program oracle_smoke

! Geopack's tilt lives in its state block. **The declaration is copied from `RECALC_08` itself** (line 318 of the
! vendored source: `COMMON /GEOPACK1/ AA(10),SPS,CPS,BB(22)`), because the first version of this function guessed a
! longer layout from memory and read a zero -- the state block holds the **sine** of the tilt and its cosine, not the
! angle, and a reader who assumes otherwise gets a plausible-looking wrong number.
double precision function ps_tilt_deg()
  implicit none
  double precision :: aa(10), sps, cps, bb(22)
  common /geopack1/ aa, sps, cps, bb
  ps_tilt_deg = asin(max(-1.d0, min(1.d0, sps))) * 57.29577951308232d0
end function ps_tilt_deg
