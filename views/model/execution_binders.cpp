/**
 * @file execution_binders.cpp
 * @brief The one list of mounted binders.
 */
#include <qp/views/model/execution_binders.hpp>

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

}  // namespace qp::views::model
