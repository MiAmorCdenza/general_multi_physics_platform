/**
 * @file test_document.cpp
 * @brief Tests for document identity and per-view layout slots.
 *
 * Test case ids match the @tests fields in the document headers byte for byte.
 *
 * The central claim under test is a negative one: **the graph does not contain
 * coordinates, and this module does not know what a layout means.** So the tests
 * assert that an uninstalled view simply has no slot, that the payload survives
 * byte for byte whatever it is, and that the frame's own bookkeeping -- order,
 * replacement, removal, dirty state -- behaves the way a save/load cycle needs it
 * to.
 */
#include <catch2/catch_test_macros.hpp>

#include <qp/authoring/document.hpp>

#include <string>
#include <vector>

using namespace qp::authoring;

// ===========================================================================
// Per-view slots
// ===========================================================================

TEST_CASE("authoring.document.layouts_are_slotted_by_view", "[authoring]") {
    ViewLayouts layouts;
    REQUIRE(layouts.empty());
    REQUIRE(layouts.size() == 0);
    REQUIRE(layouts.view_ids().empty());

    layouts.set("graph", "{\"n1\":[120,80]}");
    layouts.set("timeseries", "{\"range\":[0,10]}");

    REQUIRE(layouts.size() == 2);
    REQUIRE(layouts.has("graph"));
    REQUIRE(layouts.has("timeseries"));
    REQUIRE(layouts.get("graph") == "{\"n1\":[120,80]}");
    REQUIRE(layouts.get("timeseries") == "{\"range\":[0,10]}");

    // Order is first-set order, not sorted and not hash order. Saving the same
    // document twice must produce the same bytes in the same sequence, or a
    // version-control diff of two identical documents would be non-empty.
    REQUIRE(layouts.view_ids() == std::vector<ViewId>{"graph", "timeseries"});

    // A third view lands at the end.
    layouts.set("script", "# placeholder");
    REQUIRE(layouts.view_ids() == std::vector<ViewId>{"graph", "timeseries", "script"});
}

TEST_CASE("authoring.document.absent_view_has_no_slot", "[authoring]") {
    // The consequence worth stating plainly: a user who has not installed the
    // node editor has no "graph" slot, and that is not a missing field -- there
    // is no layout for a view that does not exist.
    ViewLayouts layouts;
    layouts.set("timeseries", "{}");

    REQUIRE_FALSE(layouts.has("graph"));
    REQUIRE(layouts.get("graph").empty());
    REQUIRE(layouts.view_ids() == std::vector<ViewId>{"timeseries"});

    // Reading an absent view is safe and repeatable: the reference returned must
    // point at something that outlives the call, not at a temporary.
    const std::string& first = layouts.get("graph");
    const std::string& second = layouts.get("graph");
    REQUIRE(&first == &second);

    // An empty view id is refused rather than stored: it would be a slot no view
    // could ever claim, and it would survive every save.
    layouts.set("", "orphan");
    REQUIRE(layouts.size() == 1);
    REQUIRE_FALSE(layouts.has(""));
}

TEST_CASE("authoring.document.layout_roundtrip", "[authoring]") {
    ViewLayouts layouts;

    // The payload is opaque bytes, so it must survive whatever a view puts in it,
    // including embedded NUL and invalid UTF-8. This is why the payload is a
    // std::string and not a text type with a validator: the core stores a view's
    // layout, it does not have an opinion about it.
    const std::string binary{"\x00\x01\xff\xfe coordinates \x00", 20};
    layouts.set("binary_view", binary);
    REQUIRE(layouts.get("binary_view") == binary);
    REQUIRE(layouts.get("binary_view").size() == binary.size());

    // Setting the same view again replaces rather than appends, and keeps the
    // view's position. Appending would give one view two layouts, and whichever a
    // reader happened to pick would be an accident.
    layouts.set("first", "a");
    layouts.set("second", "b");
    layouts.set("first", "c");
    REQUIRE(layouts.size() == 3);
    REQUIRE(layouts.get("first") == "c");
    REQUIRE(layouts.view_ids() == std::vector<ViewId>{"binary_view", "first", "second"});

    // Replacing with a shorter value must not leave a tail of the old one.
    layouts.set("second", "x");
    REQUIRE(layouts.get("second") == "x");
}

TEST_CASE("authoring.document.removing_a_view_leaves_the_rest", "[authoring]") {
    ViewLayouts layouts;
    layouts.set("graph", "g");
    layouts.set("timeseries", "t");
    layouts.set("script", "s");

    REQUIRE(layouts.remove("timeseries"));
    REQUIRE(layouts.size() == 2);
    REQUIRE_FALSE(layouts.has("timeseries"));
    REQUIRE(layouts.get("timeseries").empty());
    // The survivors keep the relative order they had.
    REQUIRE(layouts.view_ids() == std::vector<ViewId>{"graph", "script"});

    // Removing something absent reports it rather than silently succeeding: an
    // uninstall path that removes a view twice has a bug worth noticing.
    REQUIRE_FALSE(layouts.remove("timeseries"));
    REQUIRE_FALSE(layouts.remove("never_installed"));

    layouts.clear();
    REQUIRE(layouts.empty());
    REQUIRE(layouts.view_ids().empty());
    REQUIRE_FALSE(layouts.has("graph"));
}

// ===========================================================================
// Document identity
// ===========================================================================

TEST_CASE("authoring.document.identity_and_dirty_state", "[authoring]") {
    Document doc;

    // A fresh document is untitled and clean: it has never been written, and
    // nothing has been edited. Reporting it as dirty would make the host ask
    // "save changes?" about a document with nothing in it, and a user who learns
    // to dismiss that prompt will dismiss the one that mattered.
    REQUIRE(doc.is_untitled());
    REQUIRE(doc.source_path().empty());
    REQUIRE(doc.title().empty());
    REQUIRE_FALSE(doc.is_dirty());
    REQUIRE(doc.layouts().empty());

    doc.set_title("Projectile");
    REQUIRE(doc.title() == "Projectile");

    doc.mark_dirty();
    REQUIRE(doc.is_dirty());
    doc.mark_saved();
    REQUIRE_FALSE(doc.is_dirty());
}

TEST_CASE("authoring.document.save_as_keeps_the_title", "[authoring]") {
    Document doc;
    doc.set_title("Projectile");
    REQUIRE(doc.is_untitled());

    doc.set_source_path("C:/experiments/projectile.qpd");
    REQUIRE_FALSE(doc.is_untitled());
    REQUIRE(doc.source_path() == "C:/experiments/projectile.qpd");

    // Saving elsewhere must not rename the document. A user who renamed a window
    // should not watch the name revert because they chose a different file.
    doc.set_source_path("D:/backup/projectile.qpd");
    REQUIRE(doc.title() == "Projectile");
    REQUIRE(doc.source_path() == "D:/backup/projectile.qpd");

    // The layouts belong to the document, so a save-as carries them.
    doc.layouts().set("graph", "{}");
    REQUIRE(doc.layouts().has("graph"));
    REQUIRE_FALSE(doc.layouts().empty());
}
