/**
 * @file dimensions.hpp
 * @brief SI 量纲常量与 Quantity 类型别名。
 *
 * 命名约定：
 *   - `dims::<name>` 是 Dim 常量（编译期值）
 *   - `qp::<Name>` 是 Quantity 别名（可直接书写类型）
 *
 * 本文件是所有物理代码的词汇表。新增量纲必须同时提供：
 *   1) `dims::` 常量，附定义式注释
 *   2) `qp::` 类型别名
 *   3) `unit_symbol` 的黄金测试条目（见 tests/golden/test_unit_symbols.cpp）
 */
#pragma once

#include <qp/units/quantity.hpp>

namespace qp::units::dims {

// ── 七个基本量纲 ─────────────────────────────────────────────────────────────
inline constexpr Dim length{1, 0, 0, 0, 0, 0, 0};
inline constexpr Dim mass{0, 1, 0, 0, 0, 0, 0};
inline constexpr Dim time{0, 0, 1, 0, 0, 0, 0};
inline constexpr Dim current{0, 0, 0, 1, 0, 0, 0};
inline constexpr Dim temperature{0, 0, 0, 0, 1, 0, 0};
inline constexpr Dim amount{0, 0, 0, 0, 0, 1, 0};
inline constexpr Dim luminous{0, 0, 0, 0, 0, 0, 1};

// ── 几何 ─────────────────────────────────────────────────────────────────────
inline constexpr Dim area = length * length;                       // m^2
inline constexpr Dim volume = area * length;                       // m^3
inline constexpr Dim reciprocal_length = Dim{} / length;           // 1/m（波数）

// ── 运动学 ───────────────────────────────────────────────────────────────────
inline constexpr Dim velocity = length / time;                     // m/s
inline constexpr Dim acceleration = velocity / time;               // m/s^2
inline constexpr Dim jerk = acceleration / time;                   // m/s^3

// ── 力学 ─────────────────────────────────────────────────────────────────────
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

/// 注意：力矩与能量的量纲相同，但物理意义不同。量纲系统无法区分二者，
/// 这是已知局限，须在设计文档中说明而不是假装解决。
inline constexpr Dim torque = force * length;                      // N*m ≡ J

// ── 振动与转动 ───────────────────────────────────────────────────────────────
inline constexpr Dim frequency = Dim{} / time;                     // Hz = 1/s

/// 平面角在 L0 被视为无量纲（rad = 1，与 SI 一致）。
/// 角速度/角加速度因此是 rad/s、rad/s^2 —— 它们的 Dim 与 frequency 相同。
/// 若后续需要角度专用类型，须新增 dims::angle 的特殊处理，属待决项。
inline constexpr Dim angular_velocity = Dim{} / time;              // rad/s
inline constexpr Dim angular_acceleration = angular_velocity / time;  // rad/s^2

// ── 电磁 ─────────────────────────────────────────────────────────────────────
inline constexpr Dim charge = current * time;                      // A*s = C
inline constexpr Dim voltage = power / current;                    // kg*m^2/(A*s^3) = V
inline constexpr Dim resistance = voltage / current;               // kg*m^2/(A^2*s^3) = Ω
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

// ── 热与统计 ─────────────────────────────────────────────────────────────────
inline constexpr Dim entropy = energy / temperature;               // J/K
inline constexpr Dim specific_heat = energy / (mass * temperature);  // J/(kg*K)
inline constexpr Dim thermal_conductivity = power / (length * temperature);  // W/(m*K)
inline constexpr Dim molar_mass = mass / amount;                   // kg/mol

// ── 光学与辐射 ───────────────────────────────────────────────────────────────
inline constexpr Dim illuminance = luminous / area;                // lx
inline constexpr Dim luminous_flux = luminous;                     // lm

}  // namespace qp::units::dims

namespace qp::units {

// ── 无量纲 ───────────────────────────────────────────────────────────────────
using Dimensionless = Quantity<Dim{}>;

// ── 基本量 ───────────────────────────────────────────────────────────────────
using Length = Quantity<dims::length>;
using Mass = Quantity<dims::mass>;
using Time = Quantity<dims::time>;
using Current = Quantity<dims::current>;
using Temperature = Quantity<dims::temperature>;
using Amount = Quantity<dims::amount>;
using LuminousIntensity = Quantity<dims::luminous>;

// ── 几何 ─────────────────────────────────────────────────────────────────────
using Area = Quantity<dims::area>;
using Volume = Quantity<dims::volume>;
using Wavenumber = Quantity<dims::reciprocal_length>;

// ── 运动学 ───────────────────────────────────────────────────────────────────
using Velocity = Quantity<dims::velocity>;
using Acceleration = Quantity<dims::acceleration>;
using Jerk = Quantity<dims::jerk>;

// ── 力学 ─────────────────────────────────────────────────────────────────────
using Momentum = Quantity<dims::momentum>;
using Force = Quantity<dims::force>;
using Energy = Quantity<dims::energy>;
using Work = Quantity<dims::energy>;  ///< 与 Energy 同量纲，语义区分靠命名
using Power = Quantity<dims::power>;
using Pressure = Quantity<dims::pressure>;
using Density = Quantity<dims::density>;
using MomentOfInertia = Quantity<dims::moment_of_inertia>;
using DynamicViscosity = Quantity<dims::dynamic_viscosity>;
using KinematicViscosity = Quantity<dims::kinematic_viscosity>;
using SurfaceTension = Quantity<dims::surface_tension>;
using Torque = Quantity<dims::torque>;

// ── 振动与转动 ───────────────────────────────────────────────────────────────
using Frequency = Quantity<dims::frequency>;
using AngularVelocity = Quantity<dims::angular_velocity>;
using AngularAcceleration = Quantity<dims::angular_acceleration>;

// ── 电磁 ─────────────────────────────────────────────────────────────────────
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

// ── 热与统计 ─────────────────────────────────────────────────────────────────
using Entropy = Quantity<dims::entropy>;
using SpecificHeat = Quantity<dims::specific_heat>;
using ThermalConductivity = Quantity<dims::thermal_conductivity>;
using MolarMass = Quantity<dims::molar_mass>;

// ── 光学与辐射 ───────────────────────────────────────────────────────────────
using Illuminance = Quantity<dims::illuminance>;
using LuminousFlux = Quantity<dims::luminous_flux>;

}  // namespace qp::units
