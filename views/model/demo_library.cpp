/**
 * @file demo_library.cpp
 * @brief The built-in demonstrator node library.
 */
#include <qp/views/model/demo_library.hpp>

#include <qp/ports/port_type.hpp>
#include <qp/units/dim.hpp>

#include <utility>

namespace qp::views {
namespace {

using qp::graph::NodeDesc;
using qp::graph::PortNumber;
using qp::graph::PortDesc;
using qp::ports::PortTypeId;

/// @brief A dimension with the given axes set. L, M, T, I, Th, N, J.
[[nodiscard]] qp::units::Dim dim(std::int8_t L = 0, std::int8_t M = 0, std::int8_t T = 0) {
    return qp::units::Dim{L, M, T, 0, 0, 0, 0};
}

/// @brief A connectable f64 input port.
[[nodiscard]] PortDesc input(PortNumber number, std::string name, std::string label) {
    PortDesc p;
    p.number = number;
    p.name = std::move(name);
    p.label = std::move(label);
    p.type = qp::ports::kScalarF64;
    p.connectable = true;
    return p;
}

/// @brief A connectable f64 output port.
[[nodiscard]] PortDesc output(PortNumber number, std::string name, std::string label) {
    PortDesc p = input(number, std::move(name), std::move(label));
    return p;
}

/// @brief A parameter port: a value the user edits, not something to connect.
///
/// `connectable == false` is what makes it a parameter rather than a socket. One
/// descriptor type carries both, which is the `PortDesc` design, and the editor
/// has to respect the flag or every parameter would grow a socket it must not have.
[[nodiscard]] PortDesc parameter(PortNumber number, std::string name, std::string label,
                                 std::string unit = {}) {
    PortDesc p;
    p.number = number;
    p.name = std::move(name);
    p.label = std::move(label);
    p.type = qp::ports::kScalarF64;
    p.connectable = false;
    p.required = true;
    p.unit_symbol = std::move(unit);
    return p;
}

/// @brief Marks a parameter as bounded, which is what makes the editor show a slider.
void bound(PortDesc& p, double low, double high, double step) {
    p.has_range = true;
    p.min_value = low;
    p.max_value = high;
    p.step = step;
}

/// @brief A discrete choice, carried as an integer parameter with labels.
[[nodiscard]] PortDesc choice(PortNumber number, std::string name, std::string label,
                              std::vector<std::string> labels) {
    PortDesc p;
    p.number = number;
    p.name = std::move(name);
    p.label = std::move(label);
    p.type = qp::ports::kEnum;
    p.connectable = false;
    p.required = true;
    p.choice_labels = std::move(labels);
    for (std::size_t i = 0; i < p.choice_labels.size(); ++i) {
        p.choice_names.push_back(std::to_string(i));
    }
    return p;
}

}  // namespace

qp::diag::Result<void> register_demo_library(TypeCatalog& catalog) noexcept {
    // ---- A source: no inputs, one output -----------------------------------
    {
        NodeDesc d;
        d.type_name = "demo.signal";
        d.label = "Signal source";
        d.description = "Emits a value that follows a chosen waveform.";
        d.category = "sources";
        d.version = 1;
        d.allow_in_field_domain = true;
        // A source may run every frame: it only reads its own parameters.
        d.allow_in_particle_domain = true;
        d.has_compute = false;   // declaration-only in this phase
        d.outputs.push_back(output(1, "value", "Value"));
        d.inputs.push_back(choice(1, "waveform", "Waveform", {"sine", "square", "ramp"}));
        PortDesc amplitude = parameter(2, "amplitude", "Amplitude", "V");
        bound(amplitude, 0.0, 10.0, 0.1);
        d.inputs.push_back(std::move(amplitude));
        PortDesc frequency = parameter(3, "frequency", "Frequency", "Hz");
        bound(frequency, 0.0, 100.0, 0.5);
        d.inputs.push_back(std::move(frequency));
        if (auto r = catalog.add(std::move(d)); !r) return r.error();
    }

    // ---- A model: inputs and one output ------------------------------------
    {
        NodeDesc d;
        d.type_name = "demo.spring_damper";
        d.label = "Spring-damper";
        d.description = "A mass on a spring with viscous damping.";
        d.category = "models";
        d.version = 1;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = false;   // integrating is the kernel's job
        d.outputs.push_back(output(1, "position", "Position"));
        d.inputs.push_back(input(1, "drive", "Drive"));
        // Deliberately unbounded: the editor must not invent a range for it. A
        // spin box that silently limited an unbounded parameter would reject a
        // legitimate value and the user would have no way to enter it.
        d.inputs.push_back(parameter(2, "k", "Stiffness", "N/m"));
        d.inputs.push_back(parameter(3, "c", "Damping", "N*s/m"));
        PortDesc mass = parameter(4, "m", "Mass", "kg");
        bound(mass, 0.001, 100.0, 0.001);
        d.inputs.push_back(std::move(mass));
        d.inputs.push_back(choice(5, "integrator", "Integrator", {"euler", "rk4", "verlet"}));
        if (auto r = catalog.add(std::move(d)); !r) return r.error();
    }

    // ---- An instrument: a sink with a measurement --------------------------
    {
        NodeDesc d;
        d.type_name = "demo.instrument";
        d.label = "Readout";
        d.description = "Records readings of one quantity with a stated resolution.";
        d.category = "instruments";
        d.version = 1;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = true;
        d.inputs.push_back(input(1, "measured", "Measured"));
        // The resolution is what turns a reading into an uncertainty, so it is a
        // parameter with a unit rather than a bare number: a resolution without a
        // unit is the commonest way a lab report becomes wrong.
        PortDesc resolution = parameter(2, "resolution", "Resolution", "V");
        bound(resolution, 1.0e-6, 1.0, 1.0e-6);
        d.inputs.push_back(std::move(resolution));
        PortDesc averaging = parameter(3, "samples", "Samples");
        bound(averaging, 1.0, 1000.0, 1.0);
        d.inputs.push_back(std::move(averaging));
        if (auto r = catalog.add(std::move(d)); !r) return r.error();
    }

    // ---- A sink with no outputs, to exercise the endpoint case -------------
    {
        NodeDesc d;
        d.type_name = "demo.export";
        d.label = "Export";
        d.description = "Writes a series to a file. The format is a plugin.";
        d.category = "output";
        d.version = 1;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = false;   // file I/O must never run every frame
        d.inputs.push_back(input(1, "series", "Series"));
        PortDesc path = parameter(2, "path", "Path");
        path.type = qp::ports::kString;
        d.inputs.push_back(std::move(path));
        if (auto r = catalog.add(std::move(d)); !r) return r.error();
    }

    // ---- A boolean parameter, so the editor renders a checkbox ------------
    {
        NodeDesc d;
        d.type_name = "demo.filter";
        d.label = "Low-pass filter";
        d.description = "First-order filter with an optional bypass.";
        d.category = "models";
        d.version = 1;
        d.allow_in_field_domain = true;
        d.allow_in_particle_domain = true;
        d.outputs.push_back(output(1, "filtered", "Filtered"));
        d.inputs.push_back(input(1, "signal", "Signal"));
        PortDesc cutoff = parameter(2, "cutoff", "Cutoff", "Hz");
        bound(cutoff, 0.1, 1000.0, 0.1);
        d.inputs.push_back(std::move(cutoff));
        PortDesc bypass = parameter(3, "bypass", "Bypass");
        bypass.type = qp::ports::kBool;
        d.inputs.push_back(std::move(bypass));
        if (auto r = catalog.add(std::move(d)); !r) return r.error();
    }

    return {};
}

}  // namespace qp::views
