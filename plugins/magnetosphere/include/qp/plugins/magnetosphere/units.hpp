/**
 * @file units.hpp
 * @brief The normalized system the pusher runs in, and how it is derived from SI.
 *
 * ## The three systems a particle step touches, and why they are three
 *
 *   1. **SI.** The platform's boundary: `particles::ParticleState` holds metres and metres per second, a field
 *      node's `abi::LatticeDesc` says its data is in tesla, and every report quotes SI. A buffer whose
 *      description is a lie is worse than no description, because `field::is_valid_field` is entitled to be
 *      asked what a buffer holds and to get a true answer.
 *   2. **The normalized system.** What the inner loop computes in. A dipole field at the Earth's surface is
 *      3e-5 T and a particle there moves at 4e5 m/s; both are many orders from unity, and a double has the most
 *      room near 1. So the loop works in lengths of one earth radius, times of `R_E / c`, and fields of the
 *      equatorial surface value -- which is what the reference implementation does, and the reason its numbers
 *      are all of order one.
 *   3. **Dipole coordinates.** `L`, the magnetic latitude, and the local field magnitude. Not a system so much
 *      as the language the physics is *reported* in, and the language a student's lab manual uses.
 *
 * ## What each conversion is, written as the expression it stands for
 *
 * | normalized quantity | SI expression | value |
 * |---|---|---|
 * | length | `r / R_E` | one earth radius is 1 |
 * | time | `t / (R_E / c)` | one light crossing is 1 |
 * | velocity | `v / c` | the speed of light is 1 |
 * | magnetic field | `B / B_eq` | the equatorial surface field is 1 |
 * | electric field | `E / (c * B_eq)` | the field that matches the magnetic force at `v = c` |
 * | gravity | `GM / (R_E * c^2)` | the Earth's potential well, in units of `c^2` |
 * | charge per mass | `(q/m) * B_eq * (R_E / c)` | the angle turned per normalized time, at one field unit |
 *
 * ## Why these are `inline const` values rather than `constexpr`
 *
 * Because they are divisions and square roots of the constants in `geomagnetic.hpp`, and a reader benefits more
 * from each one being the expression above than from it being usable in a constant expression. A change to
 * `geomagnetic.hpp` moves every derived number with it, which is the property that matters.
 *
 * ## The one number here that is not a conversion
 *
 * `kNondipoleFraction` is how much of the surface field a dipole does not describe -- about 11%. It is here so a
 * report can print **how wrong the field model is** beside a model that is known to be an approximation. A model
 * whose error is stated is a model; one whose error is implied is a trap.
 *
 * @ownership   pure (constants)
 * @thread      any
 * @pre         none
 * @post        none
 * @invariant   Every value equals the SI expression in the table above
 * @errors      noexcept
 * @frozen      no
 * @tests       magnetosphere.units.each_conversion_is_its_own_expression,
 *              magnetosphere.units.the_speed_of_light_is_one,
 *              magnetosphere.units.the_gyrofrequency_matches_the_textbook
 */
#pragma once

#include <qp/plugins/magnetosphere/geomagnetic.hpp>

#include <cmath>

namespace qp::plugins::magnetosphere {

/// @brief The elementary charge, in coulombs. Exact, by the SI redefinition.
inline constexpr double kElementaryChargeC = 1.602176634e-19;

/// @brief The proton rest mass, in kilograms. CODATA 2022.
inline constexpr double kProtonMassKg = 1.67262192369e-27;

/// @brief The electron rest mass, in kilograms. CODATA 2022.
inline constexpr double kElectronMassKg = 9.1093837139e-31;

/**
 * @brief The proton's charge-to-mass ratio, in coulombs per kilogram.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 9.58e7 C/kg
 * @invariant   Equals `kElementaryChargeC / kProtonMassKg`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.the_gyrofrequency_matches_the_textbook
 */
inline const double kProtonChargeMassSI = kElementaryChargeC / kProtonMassKg;

/**
 * @brief The electron's charge-to-mass ratio, in coulombs per kilogram. Negative.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About -1.76e11 C/kg
 * @invariant   Equals `-kElementaryChargeC / kElectronMassKg`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.the_gyrofrequency_matches_the_textbook
 */
inline const double kElectronChargeMassSI = -kElementaryChargeC / kElectronMassKg;

/**
 * @brief An alpha particle's charge-to-mass ratio, in coulombs per kilogram: `2e / 4m_p`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About half a proton's
 * @invariant   Equals `2 * kElementaryChargeC / (4 * kProtonMassKg)`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.the_gyrofrequency_matches_the_textbook
 */
inline const double kAlphaChargeMassSI = 2.0 * kElementaryChargeC / (4.0 * kProtonMassKg);

/**
 * @brief Normalized length per metre: `1 / R_E`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 1.6e-7
 * @invariant   `kEarthRadiusM * kNormalizedPerMetre == 1`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.each_conversion_is_its_own_expression
 */
inline const double kNormalizedPerMetre = 1.0 / kEarthRadiusM;

/**
 * @brief Normalized velocity per metre per second: `1 / c`.
 *
 * One normalized velocity unit is the speed of light, which is the choice that makes a relativistic pusher's
 * `gamma` a function of a number near one rather than of `1e-9`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 3.3e-9
 * @invariant   `kSpeedOfLightSI * kNormalizedPerMetrePerSecond == 1`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.the_speed_of_light_is_one
 */
inline const double kNormalizedPerMetrePerSecond = 1.0 / kSpeedOfLightSI;

/**
 * @brief Normalized time per second: `c / R_E`.
 *
 * One normalized time unit is `R_E / c`, about 21 milliseconds.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 47 per second
 * @invariant   `kLightCrossingTimeS * kNormalizedPerSecond == 1`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.the_speed_of_light_is_one
 */
inline const double kNormalizedPerSecond = 1.0 / kLightCrossingTimeS;

/**
 * @brief Normalized magnetic field per tesla: `1 / B_eq`.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 1.0e5 per tesla
 * @invariant   `kEquatorialSurfaceFieldT * kNormalizedPerTesla == 1`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.each_conversion_is_its_own_expression
 */
inline const double kNormalizedPerTesla = 1.0 / kEquatorialSurfaceFieldT;

/**
 * @brief Normalized electric field per volt per metre: `1 / (c * B_eq)`.
 *
 * The product `c * B_eq` is the electric field whose force on a relativistic particle equals the magnetic force
 * it feels where the field is one normalized unit, which is what makes the two terms of a Lorentz push
 * comparable without a second scaling.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 3.0e3 per volt per metre
 * @invariant   Equals `1 / (kSpeedOfLightSI * kEquatorialSurfaceFieldT)`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.each_conversion_is_its_own_expression
 */
inline const double kNormalizedPerVoltPerMetre = 1.0 / (kSpeedOfLightSI * kEquatorialSurfaceFieldT);

/**
 * @brief The Earth's gravitational parameter in normalized units: `GM / (R_E * c^2)`.
 *
 * A dimensionless potential, about 7e-10. The reference implementation writes `1.5398e-6`, which is the same
 * physical quantity **in different units** -- `R_E^3 / s^2` with the time unit absorbed rather than removed. The
 * two differ by the time unit squared, so the test asserts that **relation** rather than asserting the digits
 * match. A port that copied `1.5398e-6` into a dimensionless slot would be wrong by `4.5e-4`, and the symptom
 * would be a gravitational deflection that no test of the magnetic field would catch.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        About 7.0e-10
 * @invariant   Equals `kEarthGravityParameterSI / (kEarthRadiusM * kSpeedOfLightSI^2)`
 * @errors      Cannot fail: this is an initialised constant, computed once from the constants above, and
 *              there is no code path that reports anything
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.each_conversion_is_its_own_expression
 */
inline const double kNormalizedGravity =
    kEarthGravityParameterSI / (kEarthRadiusM * kSpeedOfLightSI * kSpeedOfLightSI);

/**
 * @brief A charge-to-mass ratio in normalized units, which is an angular frequency times the time unit.
 *
 * **The derivation, from the equation of motion, because the first two versions of this function were wrong and
 * the second one was wrong in a way a test agreed with.**
 *
 * The pusher integrates `du/dtau = q_prime * (E_norm + u x B_norm)` with `u = gamma v / c`, `tau = t / T` and
 * `T = R_E / c`. The physical equation is `d(gamma v)/dt = (q/m)(E + v x B)`. Matching the two term by term, with
 * `v = c u`, `B = B_eq B_norm`, `E = c B_eq E_norm` and `dt = T dtau`:
 *
 *     (c / T) du/dtau = (q/m) c B_eq (E_norm + u x B_norm)
 *     du/dtau         = (q/m) B_eq T (E_norm + u x B_norm)
 *
 * so the conversion is `q_prime = (q/m) * B_eq * T`, and `q_prime` is **dimensionless**: it is the angle a
 * particle turns through per normalized time where the field is one normalized unit, which is also the physical
 * gyrofrequency `q B_eq / m` multiplied by the time unit.
 *
 * For a proton that is `2845.75 rad/s * 0.021275 s = 60.546`. The two wrong versions, and why each survived:
 *
 *   - the first multiplied by `c / R_E` instead of `R_E / c`, which is the same factor of 47 in the wrong
 *     direction -- the error is 47 squared, 2209;
 *   - the second was **spelled** as a division by `R_E / c` while still computing `B_eq / (R_E / c)`, which is
 *     the same 2209 error with a comment claiming it had been fixed. The assertion that was supposed to catch it
 *     read `q_prime / kNormalizedPerSecond == q B_eq / m`, which is the reciprocal of the conversion, so the test
 *     agreed with the defect instead of finding it. A test written from the same understanding as the code
 *     agrees with the code whatever the code does; the check that found this one came from outside both --
 *     `magnetosphere.boris.a_uniform_field_gives_the_relativistic_gyrofrequency` integrates a proton in a
 *     uniform field of one unit and measures how far it turns per unit time.
 *
 * @param charge_mass_si The species' charge-to-mass ratio, in coulombs per kilogram.
 *
 * @ownership   pure
 * @thread      any
 * @pre         `charge_mass_si` is finite
 * @post        The species' normalized charge-to-mass ratio, in radians per normalized time
 * @invariant   The ratio of two species' values equals the ratio of their SI charge-to-mass ratios
 * @invariant   `value * kNormalizedPerSecond == charge_mass_si * kEquatorialSurfaceFieldT`
 * @errors      noexcept: a product of finite constants, with nothing to report
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.the_gyrofrequency_matches_the_textbook
 */
[[nodiscard]] inline double normalized_charge_mass(double charge_mass_si) noexcept {
    return charge_mass_si * kEquatorialSurfaceFieldT * kLightCrossingTimeS;
}

/// @brief A proton's normalized charge-to-mass ratio. About 60.55 per normalized time.
inline const double kProtonNormalizedChargeMass = normalized_charge_mass(kProtonChargeMassSI);

/// @brief An electron's normalized charge-to-mass ratio. Negative, 1836 times larger in magnitude.
inline const double kElectronNormalizedChargeMass = normalized_charge_mass(kElectronChargeMassSI);

/// @brief An alpha particle's normalized charge-to-mass ratio. Half a proton's, because `q/m` is half.
inline const double kAlphaNormalizedChargeMass = normalized_charge_mass(kAlphaChargeMassSI);

/**
 * @brief How much of the surface field a dipole does not describe, as a fraction.
 *
 * About 11%, from `geomagnetic.hpp` where its provenance is written down. A report prints beside a dipole field so the reader knows what
 * the model leaves out, and the justification for the reopening condition in `dipole.hpp`: a feature at the 10%
 * level is the largest thing a dipole cannot show, so a claim finer than that needs a different field model.
 *
 * @ownership   pure
 * @thread      any
 * @pre         none
 * @post        Between 0 and 1
 * @invariant   Equals `geomagnetic::kNonDipoleFraction`
 * @errors      noexcept: a `constexpr` value, so the compiler is the one that checks it
 * @complexity  O(1)
 * @nondet      none
 * @frozen      no
 * @tests       magnetosphere.units.each_conversion_is_its_own_expression
 */
inline constexpr double kNondipoleFraction = kNonDipoleFraction;

}  // namespace qp::plugins::magnetosphere
