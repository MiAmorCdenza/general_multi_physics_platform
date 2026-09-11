/**
 * @file dimensions.hpp
 * @brief SI dimension constants and Quantity type aliases.
 *
 * Naming convention:
 *   - `dims::<name>` is a Dim constant (a compile-time value)
 *   - `qp::<Name>` is a Quantity alias (a type you can write directly)
 *
 * This file is the vocabulary of all physics code. A new dimension must supply all three:
 *   1) a `dims::` constant with a defining-expression comment
 *   2) a `qp::` type alias
 *   3) a golden test entry for `unit_symbol` (see tests/golden/test_unit_symbols.cpp)
 */
#pragma once

#include <qp/units/quantity.hpp>

namespace qp::units::dims {

// -- Seven base dimensions ----------------------------------------------------
inline constexpr Dim length{1, 0, 0, 0, 0, 0, 0};
inline constexpr Dim mass{0, 1, 0, 0, 0, 0, 0};
inline constexpr Dim time{0, 0, 1, 0, 0, 0, 0};
inline constexpr Dim current{0, 0, 0, 1, 0, 0, 0};
inline constexpr Dim temperature{0, 0, 0, 0, 1, 0, 0};
inline constexpr Dim amount{0, 0, 0, 0, 0, 1, 0};
inline constexpr Dim luminous{0, 0, 0, 0, 0, 0, 1};

// -- Geometry -----------------------------------------------------------------
inline constexpr Dim area = length * length;                       // m^2
inline constexpr Dim volume = area * length;                       // m^3
inline constexpr Dim reciprocal_length = Dim{} / length;           // 1/m (wavenumber)

// -- Kinematics ---------------------------------------------------------------
inline constexpr Dim velocity = length / time;                     // m/s
inline constexpr Dim acceleration = velocity / time;               // m/s^2
inline constexpr Dim jerk = acceleration / time;                   // m/s^3

// -- Mechanics ----------------------------------------------------------------
inline constexpr Dim momentum = mass * velocity;                   // kg*m/s
inline constexpr Dim force = mass * acceleration;                  // kg*m/s^2 = N
inline constexpr Dim energy = force * length;                      // kg*m^2/s^2 = J
inline constexpr Dim power = energy / time;                        // kg*m^2/s^3 = W
inline constexpr Dim pressure = force / area;                      // kg/(m*s^2) = Pa
inline constexpr Dim density = mass / volume;                      // kg/m^3
inline constexpr Dim moment_of_inertia = mass * area;              // kg*m^2
inline constexpr Dim dynamic_viscosity = pressure * time;          // Pa*s
inline constexpr Dim kinematic_viscosity = area / time;            // m^2/s
inline constexpr Dim surface_tension = force / length;             // N/m

/// Note: torque and energy share one dimension but differ in physical meaning. The dimension system
/// cannot tell them apart, and that known limitation must be documented rather than papered over.
inline constexpr Dim torque = force * length;                      // N*m == J

// -- Vibration and rotation ---------------------------------------------------
inline constexpr Dim frequency = Dim{} / time;                     // Hz = 1/s

/// A plane angle is treated as dimensionless at L0 (rad = 1, consistent with SI).
/// Angular velocity/acceleration are therefore rad/s and rad/s^2 -- their Dim equals frequency.
/// A dedicated angle type, if ever needed, requires special handling of dims::angle; that is still open.
inline constexpr Dim angular_velocity = Dim{} / time;              // rad/s
inline constexpr Dim angular_acceleration = angular_velocity / time;  // rad/s^2

// -- Electromagnetism ---------------------------------------------------------
inline constexpr Dim charge = current * time;                      // A*s = C
inline constexpr Dim voltage = power / current;                    // kg*m^2/(A*s^3) = V
inline constexpr Dim resistance = voltage / current;               // kg*m^2/(A^2*s^3) = ohm
inline constexpr Dim conductance = Dim{} / resistance;             // S
inline constexpr Dim capacitance = charge / voltage;               // A^2*s^4/(kg*m^2) = F
inline constexpr Dim magnetic_flux = voltage * time;               // kg*m^2/(A*s^2) = Wb
inline constexpr Dim magnetic_flux_density = magnetic_flux / area; // kg/(A*s^2) = T
inline constexpr Dim inductance = magnetic_flux / current;         // kg*m^2/(A^2*s^2) = H
inline constexpr Dim electric_field = voltage / length;            // V/m
inline constexpr Dim permittivity = capacitance / length;          // F/m
inline constexpr Dim permeability = inductance / length;           // H/m
inline constexpr Dim electric_dipole = charge * length;            // C*m
inline constexpr Dim magnetic_dipole = current * area;             // A*m^2

// -- Thermodynamics and statistics --------------------------------------------
inline constexpr Dim entropy = energy / temperature;               // J/K
inline constexpr Dim specific_heat = energy / (mass * temperature);  // J/(kg*K)
inline constexpr Dim thermal_conductivity = power / (length * temperature);  // W/(m*K)
inline constexpr Dim molar_mass = mass / amount;                   // kg/mol

// -- Optics and radiation -----------------------------------------------------
inline constexpr Dim illuminance = luminous / area;                // lx
inline constexpr Dim luminous_flux = luminous;                     // lm

}  // namespace qp::units::dims

namespace qp::units {

// -- Dimensionless ------------------------------------------------------------
using Dimensionless = Quantity<Dim{}>;

// -- Base quantities ----------------------------------------------------------
using Length = Quantity<dims::length>;
using Mass = Quantity<dims::mass>;
using Time = Quantity<dims::time>;
using Current = Quantity<dims::current>;
using Temperature = Quantity<dims::temperature>;
using Amount = Quantity<dims::amount>;
using LuminousIntensity = Quantity<dims::luminous>;

// -- Geometry -----------------------------------------------------------------
using Area = Quantity<dims::area>;
using Volume = Quantity<dims::volume>;
using Wavenumber = Quantity<dims::reciprocal_length>;

// -- Kinematics ---------------------------------------------------------------
using Velocity = Quantity<dims::velocity>;
using Acceleration = Quantity<dims::acceleration>;
using Jerk = Quantity<dims::jerk>;

// -- Mechanics ----------------------------------------------------------------
using Momentum = Quantity<dims::momentum>;
using Force = Quantity<dims::force>;
using Energy = Quantity<dims::energy>;
using Work = Quantity<dims::energy>;  ///< Same dimension as Energy; the naming carries the distinction
using Power = Quantity<dims::power>;
using Pressure = Quantity<dims::pressure>;
using Density = Quantity<dims::density>;
using MomentOfInertia = Quantity<dims::moment_of_inertia>;
using DynamicViscosity = Quantity<dims::dynamic_viscosity>;
using KinematicViscosity = Quantity<dims::kinematic_viscosity>;
using SurfaceTension = Quantity<dims::surface_tension>;
using Torque = Quantity<dims::torque>;

// -- Vibration and rotation ---------------------------------------------------
using Frequency = Quantity<dims::frequency>;
using AngularVelocity = Quantity<dims::angular_velocity>;
using AngularAcceleration = Quantity<dims::angular_acceleration>;

// -- Electromagnetism ---------------------------------------------------------
using Charge = Quantity<dims::charge>;
using Voltage = Quantity<dims::voltage>;
using Resistance = Quantity<dims::resistance>;
using Conductance = Quantity<dims::conductance>;
using Capacitance = Quantity<dims::capacitance>;
using MagneticFlux = Quantity<dims::magnetic_flux>;
using MagneticFluxDensity = Quantity<dims::magnetic_flux_density>;
using Inductance = Quantity<dims::inductance>;
using ElectricField = Quantity<dims::electric_field>;
using Permittivity = Quantity<dims::permittivity>;
using Permeability = Quantity<dims::permeability>;
using ElectricDipoleMoment = Quantity<dims::electric_dipole>;
using MagneticDipoleMoment = Quantity<dims::magnetic_dipole>;

// -- Thermodynamics and statistics --------------------------------------------
using Entropy = Quantity<dims::entropy>;
using SpecificHeat = Quantity<dims::specific_heat>;
using ThermalConductivity = Quantity<dims::thermal_conductivity>;
using MolarMass = Quantity<dims::molar_mass>;

// -- Optics and radiation -----------------------------------------------------
using Illuminance = Quantity<dims::illuminance>;
using LuminousFlux = Quantity<dims::luminous_flux>;

}  // namespace qp::units
