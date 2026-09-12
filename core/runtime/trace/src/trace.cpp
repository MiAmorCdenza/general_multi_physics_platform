/**
 * @file trace.cpp
 * @brief Implementation of the timeline.
 */
#include <qp/runtime/trace/trace.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace qp::runtime {
namespace {

/// @brief Whether `v` is a time this trace can order.
[[nodiscard]] bool is_orderable(double v) noexcept {
    // NaN is the one value that is neither less than, equal to, nor greater than
    // the previous time, so a trace that accepted it would have a sample whose
    // position depends on which comparison is written first. Refused at the door.
    return std::isfinite(v);
}

}  // namespace

diag::Result<std::size_t> Trace::add_channel(Channel channel) noexcept {
    if (channel.name.empty()) return diag::ErrorCode::invalid_argument;
    if (!samples_.empty()) {
        // A channel declared after the first sample would leave every earlier
        // sample one value short, and there is no honest way to fill the gap:
        // padding with zeroes would invent a measurement nobody took.
        return diag::ErrorCode::duplicate_connection;
    }
    if (std::any_of(channels_.begin(), channels_.end(),
                    [&channel](const Channel& c) { return c.name == channel.name; })) {
        // A duplicate name would make an export produce two columns with the same
        // header, and a reader could not tell which one a fit used.
        return diag::ErrorCode::invalid_argument;
    }
    channels_.push_back(std::move(channel));
    return channels_.size() - 1;
}

diag::Result<void> Trace::append(double t, std::vector<UncertainValue> values) noexcept {
    if (values.size() != channels_.size()) return diag::ErrorCode::invalid_argument;
    if (!is_orderable(t)) return diag::ErrorCode::invalid_argument;

    if (!samples_.empty() && t < samples_.back().t) {
        // Refused rather than sorted into place. Inserting out of order would
        // silently renumber every later sample, so a cursor, a cache key or a
        // saved column index taken before the insert would point at a different
        // measurement -- and nothing in the record would show that it happened.
        //
        // Equal times are allowed: that is what several channels read at one
        // instant look like, and what accumulated round-off produces.
        return diag::ErrorCode::out_of_range;
    }

    Sample sample;
    sample.index = samples_.size();
    sample.t = t;
    sample.values = std::move(values);
    samples_.push_back(std::move(sample));
    return {};
}

diag::Result<void> Trace::append(double t, UncertainValue value) noexcept {
    if (channels_.size() != 1) return diag::ErrorCode::invalid_argument;
    std::vector<UncertainValue> values;
    values.push_back(std::move(value));
    return append(t, std::move(values));
}

std::optional<double> Trace::t_begin() const noexcept {
    if (samples_.empty()) return std::nullopt;
    return samples_.front().t;
}

std::optional<double> Trace::t_end() const noexcept {
    if (samples_.empty()) return std::nullopt;
    return samples_.back().t;
}

std::optional<double> Trace::duration() const noexcept {
    if (samples_.size() < 2) return std::nullopt;
    return samples_.back().t - samples_.front().t;
}

std::optional<UncertainValue> Trace::value_at(std::uint64_t index,
                                              std::size_t channel) const noexcept {
    // Both bounds are checked. A caller that guessed either has a bug, and a
    // default value would let it continue with a plausible wrong number.
    if (index >= samples_.size()) return std::nullopt;
    const Sample& sample = samples_[static_cast<std::size_t>(index)];
    if (channel >= sample.values.size()) return std::nullopt;
    return sample.values[channel];
}

bool Trace::is_consistent() const noexcept {
    for (std::size_t i = 0; i < samples_.size(); ++i) {
        const Sample& sample = samples_[i];
        if (sample.index != i) return false;
        if (sample.values.size() != channels_.size()) return false;
        if (!is_orderable(sample.t)) return false;
        if (i > 0 && sample.t < samples_[i - 1].t) return false;
    }
    return true;
}

const Sample* Cursor::current() const noexcept {
    if (trace_ == nullptr) return nullptr;
    if (index_ >= trace_->size()) return nullptr;
    return &trace_->samples()[static_cast<std::size_t>(index_)];
}

bool Cursor::next() noexcept {
    if (!has_next()) return false;
    ++index_;
    return true;
}

bool Cursor::previous() noexcept {
    if (!has_previous()) return false;
    --index_;
    return true;
}

bool Cursor::seek(std::uint64_t index) noexcept {
    if (trace_ == nullptr) return false;
    if (index >= trace_->size()) return false;
    index_ = index;
    return true;
}

bool Cursor::has_next() const noexcept {
    if (trace_ == nullptr || !valid()) return false;
    return index_ + 1 < trace_->size();
}

}  // namespace qp::runtime
