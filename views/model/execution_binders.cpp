/**
 * @file execution_binders.cpp
 * @brief The one list of mounted binders.
 */
#include <qp/views/model/execution_binders.hpp>
#include <qp/views/model/run_providers.hpp>

#include <algorithm>

namespace qp::views::model {

std::vector<qp::graph::execution::IOperatorBinder*>& execution_binders() noexcept {
    // A function-local static rather than a namespace-scope object: the initialisation order of
    // namespace-scope objects across translation units is unspecified, and this list is read from a
    // constructor (`EditorWindow` builds its controller). A static here is initialised on first use,
    // which is the first read, which is the only ordering that matters.
    static std::vector<qp::graph::execution::IOperatorBinder*> binders;
    return binders;
}

void mount_execution_binder(qp::graph::execution::IOperatorBinder* binder) {
    if (binder == nullptr) return;
    std::vector<qp::graph::execution::IOperatorBinder*>& binders = execution_binders();
    if (std::find(binders.begin(), binders.end(), binder) != binders.end()) return;
    binders.push_back(binder);
}


// -- The run providers ------------------------------------------------------------------------------------
//
// The second list, and it lives in this file for the reason the two headers live side by side: they are filled
// by the same caller at the same moment, and a second translation unit for a dozen lines would make one
// mechanism look like two.

namespace {

/// @brief The one list. `run_providers` hands out a const reference; mounting and clearing need the mutable
///        one, and keeping both here is what stops a caller from resizing the list by accident.
std::vector<qp::graph::execution::IGraphRunProvider*>& provider_list() noexcept {
    // A function-local static, for the reason `execution_binders` gives above: `EditorWindow` reads this from a
    // constructor, and the initialisation order of namespace-scope objects across translation units is not
    // something to depend on.
    static std::vector<qp::graph::execution::IGraphRunProvider*> providers;
    return providers;
}

}  // namespace

const std::vector<qp::graph::execution::IGraphRunProvider*>& run_providers() noexcept { return provider_list(); }

void mount_run_provider(qp::graph::execution::IGraphRunProvider* provider) noexcept {
    if (provider == nullptr) return;
    std::vector<qp::graph::execution::IGraphRunProvider*>& providers = provider_list();
    if (std::find(providers.begin(), providers.end(), provider) != providers.end()) return;
    providers.push_back(provider);
}

void clear_run_providers() noexcept { provider_list().clear(); }

}  // namespace qp::views::model
