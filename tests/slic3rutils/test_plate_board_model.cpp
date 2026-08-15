// PlateBoardModel: the sidebar plate board's data layer, plus the PartPlate narrow reads that
// need the same headless harness and nothing more.
//
// WHY THIS FILE IS IN tests/slic3rutils AND NOT tests/libslic3r
// PlateBoardModel is declared in src/slic3r/GUI/PlateBoard.hpp and defined in libslic3r_gui.
// libslic3r_tests links libslic3r only, so it cannot see it. slic3rutils_tests is the one test
// target that already links libslic3r_gui, so this file costs no new link line and no new target.
// If PlateBoardModel is ever moved into libslic3r, move these cases to tests/libslic3r with it.
//
// WHAT "HEADLESS" MEANS HERE, EXACTLY
// The class is a pure function of PartPlateList plus PresetBundle: it resolves bed sizes from the
// preset collection directly rather than through PartPlateList::resolve_printer_bed, which bails
// without a wxApp. PartPlate itself supports a null Plater (that is CLI mode: PartPlate::set_shape
// skips its whole render-data block when m_plater is null), so a synthetic list can be built here.
//
// Two consequences of running with no wxApp, which shape what these cases can assert:
//   * PartPlate::is_slice_result_valid() answers from the slice flag alone, because there is no
//     preset bundle to recompose the plate's context against. So "sliced" is settable and testable,
//     and the third row state, "sliced, then the context moved out from under it", is not
//     reachable here. It needs the running application.
//   * PartPlate::get_extruders() reaches wxGetApp().plater() when the MODEL HAS NO OBJECTS
//     (PartPlate::check_objects_empty_and_gcode3mf), which is a null dereference with no wxApp.
//     Every list built here therefore gives its Model one object. Do not remove that line thinking
//     it is decoration.

#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
    //CommCtrl.h belongs with Windows.h, exactly as src/slic3r/pchheader.hpp pairs them, and
    //every libslic3r_gui TU gets that pair from the PCH. Including Windows.h without it and
    //then reaching a wx header sets the windows.h include guard first, so wx/msw/wrapcctl.h
    //finds HDITEM undefined and fails on `struct wxHDITEM : public HDITEM`. PartPlate.hpp
    //pulls that chain in, so this file needs the same pair the PCH gives.
    #include <CommCtrl.h>
#endif

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/PlateBoard.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

const std::string PROJECT_PRINTER = "Project Printer 0.4 nozzle";
const std::string SMALL_PRINTER   = "Small Bed 0.4 nozzle";
const std::string LARGE_PRINTER   = "Large Bed 0.6 nozzle";
// Stored on a plate but never installed. The board must preserve it verbatim.
const std::string GHOST_PRINTER   = "Retired Machine 0.8 nozzle";

// A printer preset with a rectangular printable area whose corner is deliberately not the origin,
// so a bed size taken from the max corner instead of the extents reads wrong.
Preset &add_printer(PresetBundle &bundle, const std::string &name, double x0, double y0, double w, double d, double nozzle)
{
    DynamicPrintConfig config(bundle.printers.default_preset().config);
    Preset &preset = bundle.printers.load_preset(std::string(), name, config, /*select=*/false);
    preset.printer_technology_ref() = ptFFF;
    preset.config.set_key_value("printable_area",
                                new ConfigOptionPoints(std::vector<Vec2d>{Vec2d(x0, y0), Vec2d(x0 + w, y0),
                                                                          Vec2d(x0 + w, y0 + d), Vec2d(x0, y0 + d)}));
    preset.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({nozzle}));
    return preset;
}

void build_bundle(PresetBundle &bundle)
{
    add_printer(bundle, PROJECT_PRINTER, 0., 0., 220., 220., 0.4);
    add_printer(bundle, SMALL_PRINTER, -100., -90., 200., 180., 0.4);
    add_printer(bundle, LARGE_PRINTER, 0., 0., 300., 300., 0.6);
    bundle.printers.select_preset_by_name(PROJECT_PRINTER, true);
    REQUIRE(bundle.printers.get_selected_preset_name() == PROJECT_PRINTER);
}

// A plate list with `count` plates and no Plater. See the header comment for why the model gets
// an object.
struct SyntheticPlates
{
    Model         model;
    PartPlateList plates;

    explicit SyntheticPlates(int count) : plates(nullptr, &model)
    {
        model.add_object();
        REQUIRE_FALSE(model.objects.empty());
        while (plates.get_plate_count() < count)
            REQUIRE(plates.create_plate(/*adjust_position=*/false) >= 0);
        REQUIRE(plates.get_plate_count() == count);
    }

    PartPlate &at(int index)
    {
        PartPlate *plate = plates.get_plate(index);
        REQUIRE(plate != nullptr);
        return *plate;
    }

    // Mark a plate sliced with a known print time. capture_config is false on purpose: capturing
    // recomposes the plate's context, which cannot resolve with no bundle wired to a wxApp, and
    // would clear the very snapshot it was asked to take.
    void mark_sliced(int index, float seconds)
    {
        PrintBase *  print = nullptr;
        GCodeResult *gcode = nullptr;
        int          print_index = -1;
        at(index).get_print(&print, &gcode, &print_index);
        REQUIRE(gcode != nullptr);
        gcode->print_statistics.modes[static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal)].time = seconds;
        at(index).update_slice_result_valid_state(true, /*capture_config=*/false);
        REQUIRE(at(index).is_slice_result_valid());
    }
};

// Every row index, in group order, for a grouping that files rows directly under machine headers.
std::vector<int> rows_in_group_order(const PlateBoardModel &model)
{
    std::vector<int> ordered;
    for (const PlateBoardGroup &group : model.groups())
        ordered.insert(ordered.end(), group.rows.begin(), group.rows.end());
    return ordered;
}

std::vector<std::string> group_keys(const PlateBoardModel &model)
{
    std::vector<std::string> keys;
    for (const PlateBoardGroup &group : model.groups())
        keys.push_back(group.key);
    return keys;
}

} // namespace

TEST_CASE("PlateBoardModel resolves bed size from the preset collection", "[PlateBoard][PlateContext]")
{
    PresetBundle bundle;
    build_bundle(bundle);

    double w = -1., d = -1.;

    SECTION("an installed preset gives the extents of its printable area, not its far corner")
    {
        REQUIRE(PlateBoardModel::printer_bed_size(bundle, SMALL_PRINTER, w, d));
        CHECK_THAT(w, Catch::Matchers::WithinAbs(200., 1e-9));
        CHECK_THAT(d, Catch::Matchers::WithinAbs(180., 1e-9));
    }

    SECTION("a preset this installation does not have is reported, not guessed at")
    {
        CHECK_FALSE(PlateBoardModel::printer_bed_size(bundle, GHOST_PRINTER, w, d));
        // and the caller's values are left alone rather than half-written
        CHECK_THAT(w, Catch::Matchers::WithinAbs(-1., 1e-9));
        CHECK_THAT(d, Catch::Matchers::WithinAbs(-1., 1e-9));
    }

    SECTION("an empty name is not a lookup")
    {
        CHECK_FALSE(PlateBoardModel::printer_bed_size(bundle, std::string(), w, d));
    }

    SECTION("a printable area with fewer than three points has no usable bed")
    {
        Preset &degenerate = add_printer(bundle, "Degenerate 0.4 nozzle", 0., 0., 100., 100., 0.4);
        degenerate.config.set_key_value("printable_area",
                                        new ConfigOptionPoints(std::vector<Vec2d>{Vec2d(0., 0.), Vec2d(10., 10.)}));
        CHECK_FALSE(PlateBoardModel::printer_bed_size(bundle, "Degenerate 0.4 nozzle", w, d));
    }

    SECTION("nozzle diameter comes from the preset, and an absent key is absent rather than zero")
    {
        double diameter = -1.;
        REQUIRE(PlateBoardModel::printer_nozzle_diameter(bundle, LARGE_PRINTER, diameter));
        CHECK_THAT(diameter, Catch::Matchers::WithinAbs(0.6, 1e-9));

        CHECK_FALSE(PlateBoardModel::printer_nozzle_diameter(bundle, GHOST_PRINTER, diameter));

        // The option pointer is checked rather than DynamicConfig::opt_float, which ends in
        // "return 0;" from a function returning const double& - an absent key is a dangling
        // reference there, not a zero. This case is what pins that.
        Preset &no_nozzle = add_printer(bundle, "No Nozzle 0.4 nozzle", 0., 0., 100., 100., 0.4);
        no_nozzle.config.erase("nozzle_diameter");
        CHECK_FALSE(PlateBoardModel::printer_nozzle_diameter(bundle, "No Nozzle 0.4 nozzle", diameter));
    }
}

TEST_CASE("PlateBoardModel's default grouping switches at the compact threshold", "[PlateBoard]")
{
    // Below and at the threshold, plate order: the plate number is the project's shared vocabulary
    // and every prep-pipeline filename is keyed on it, so reordering costs more than it buys.
    for (int count = 1; count <= PLATE_BOARD_COMPACT_ABOVE; ++count) {
        DYNAMIC_SECTION("plate order at " << count << " plates")
        {
            CHECK(PlateBoardModel::default_grouping(count) == PlateBoardGrouping::PlateOrder);
        }
    }
    for (int count : {PLATE_BOARD_COMPACT_ABOVE + 1, 12, 21, 36}) {
        DYNAMIC_SECTION("by machine at " << count << " plates")
        {
            CHECK(PlateBoardModel::default_grouping(count) == PlateBoardGrouping::ByMachine);
        }
    }
}

TEST_CASE("PlateBoardModel rows report each plate's own machine", "[PlateBoard][PlateContext]")
{
    PresetBundle bundle;
    build_bundle(bundle);

    SyntheticPlates list(3);
    // plate 0 names no printer at all - since the project printer's deletion (2026-08-14)
    // there is nothing standing behind it, and the row must say so rather than borrow
    list.at(1).set_printer_preset_name(SMALL_PRINTER);
    list.at(2).set_printer_preset_name(GHOST_PRINTER);

    PlateBoardModel model;
    model.rebuild(list.plates, bundle, PlateBoardGrouping::PlateOrder);
    REQUIRE(model.rows().size() == 3);

    SECTION("a plate naming no printer reads as exactly that, never as some other machine")
    {
        const PlateBoardRow &row = model.rows()[0];
        CHECK(row.printer_name.empty());
        CHECK(row.preset_missing);
    }

    SECTION("an assigned plate shows its own machine's bed, not the project's")
    {
        const PlateBoardRow &row = model.rows()[1];
        CHECK_FALSE(row.preset_missing);
        CHECK(row.printer_name == SMALL_PRINTER);
        CHECK_THAT(row.bed_w, Catch::Matchers::WithinAbs(200., 1e-9));
        CHECK_THAT(row.bed_d, Catch::Matchers::WithinAbs(180., 1e-9));
        // 220 is the project bed; reading it here would be the substitution the fork forbids
        CHECK(std::abs(row.bed_w - 220.) > 1e-9);
    }

    SECTION("an assignment this installation cannot resolve is preserved, not repaired")
    {
        const PlateBoardRow &row = model.rows()[2];
        CHECK(row.preset_missing);
        // the stored name comes back verbatim: not cleared, not remapped to a nearest match,
        // and not rendered as though the plate were following the project printer
        CHECK(row.printer_name == GHOST_PRINTER);
        CHECK(row.printer_name != PROJECT_PRINTER);
        // no bed and no nozzle are invented for it
        CHECK_THAT(row.bed_w, Catch::Matchers::WithinAbs(0., 1e-9));
        CHECK_THAT(row.bed_d, Catch::Matchers::WithinAbs(0., 1e-9));
        CHECK_THAT(row.nozzle_diameter, Catch::Matchers::WithinAbs(0., 1e-9));

        // and rebuilding wrote nothing back to the plate
        CHECK(list.at(2).get_printer_preset_name() == GHOST_PRINTER);
        CHECK(list.at(2).has_printer_assignment());
    }

    SECTION("the glyph reference is the largest bed in the project, with a floor")
    {
        // 220 project, 200x180 small, 0 for the unresolvable one: all below the 250 floor, so a
        // project of small machines does not draw a 180 mm bed at full size.
        CHECK_THAT(model.glyph_reference_mm(), Catch::Matchers::WithinAbs(250., 1e-9));

        list.at(2).set_printer_preset_name(LARGE_PRINTER);
        model.rebuild(list.plates, bundle, PlateBoardGrouping::PlateOrder);
        CHECK_THAT(model.glyph_reference_mm(), Catch::Matchers::WithinAbs(300., 1e-9));
    }

    SECTION("plate order draws no group header at all")
    {
        CHECK(model.groups().empty());
    }
}

TEST_CASE("PlateBoardModel rollup states what it counted and what it could not", "[PlateBoard]")
{
    PresetBundle bundle;
    build_bundle(bundle);

    SyntheticPlates list(4);
    list.at(0).set_printer_preset_name(SMALL_PRINTER);
    list.at(1).set_printer_preset_name(SMALL_PRINTER);
    list.at(2).set_printer_preset_name(LARGE_PRINTER);
    // plate 3 follows the project printer

    list.mark_sliced(0, 3600.f);  // 1h
    list.mark_sliced(1, 5400.f);  // 1h30
    list.mark_sliced(2, 7200.f);  // 2h
    // plate 3 is never sliced

    PlateBoardModel model;
    model.rebuild(list.plates, bundle, PlateBoardGrouping::ByMachine);

    CHECK(model.rollup().plates == 4);
    // two assigned machines plus the project printer the fourth plate follows
    CHECK(model.rollup().machines == 3);
    CHECK_THAT(model.rollup().total_seconds, Catch::Matchers::WithinAbs(16200.f, 1e-3f));
    // the longest QUEUE is a machine's total, not the longest single plate: the small bed carries
    // two plates at 1h and 1h30, which beats the large bed's single 2h plate
    CHECK_THAT(model.rollup().longest_queue_seconds, Catch::Matchers::WithinAbs(9000.f, 1e-3f));
    // without this the totals would lie by omission
    CHECK(model.rollup().not_estimated == 1);

    CHECK(model.rows()[0].sliced);
    CHECK(model.rows()[0].has_time);
    CHECK_FALSE(model.rows()[3].sliced);
    CHECK_FALSE(model.rows()[3].has_time);
    CHECK_THAT(model.rows()[3].print_time_seconds, Catch::Matchers::WithinAbs(0.f, 1e-6f));
}

TEST_CASE("PlateBoardModel grouping is stable and total above the compact threshold", "[PlateBoard]")
{
    PresetBundle bundle;
    build_bundle(bundle);

    const int plate_count = PLATE_BOARD_COMPACT_ABOVE + 1; // 9: past the point where the default switches
    REQUIRE(PlateBoardModel::default_grouping(plate_count) == PlateBoardGrouping::ByMachine);

    SyntheticPlates list(plate_count);
    // 4 on the small bed, 3 on the large bed, 2 following the project row
    for (int i = 0; i < 4; ++i)
        list.at(i).set_printer_preset_name(SMALL_PRINTER);
    for (int i = 4; i < 7; ++i)
        list.at(i).set_printer_preset_name(LARGE_PRINTER);

    list.mark_sliced(0, 3600.f);
    list.mark_sliced(4, 7200.f);
    list.mark_sliced(5, 7200.f);

    PlateBoardModel model;

    SECTION("by machine: every plate is filed exactly once, and the inherited group is last")
    {
        model.rebuild(list.plates, bundle, PlateBoardGrouping::ByMachine);
        REQUIRE(model.rows().size() == size_t(plate_count));
        REQUIRE(model.groups().size() == 3);

        std::vector<int> ordered = rows_in_group_order(model);
        REQUIRE(ordered.size() == size_t(plate_count));
        std::sort(ordered.begin(), ordered.end());
        for (int i = 0; i < plate_count; ++i)
            CHECK(ordered[size_t(i)] == i); // no row lost, no row filed twice

        // Groups file in machine-name order, and the empty-name group - plates that resolve
        // no machine at all - sorts FIRST, which is where unresolved work belongs on a board.
        // (The "inherited group sorts last" this asserted before died with the project
        // printer: absence of a machine is no longer a way of naming one.)
        CHECK(model.groups().front().key == "m:");
        CHECK(model.groups().front().plate_count == 2);

        const std::vector<std::string> keys = group_keys(model);
        CHECK(std::find(keys.begin(), keys.end(), "m:" + SMALL_PRINTER) != keys.end());
        CHECK(std::find(keys.begin(), keys.end(), "m:" + LARGE_PRINTER) != keys.end());
    }

    SECTION("rebuilding the same list twice produces the same groups in the same order")
    {
        model.rebuild(list.plates, bundle, PlateBoardGrouping::ByMachine);
        const std::vector<std::string> first  = group_keys(model);
        const std::vector<int>         first_rows = rows_in_group_order(model);

        model.rebuild(list.plates, bundle, PlateBoardGrouping::ByMachine);
        CHECK(group_keys(model) == first);
        CHECK(rows_in_group_order(model) == first_rows);
    }

    SECTION("by capacity: the same groups, sorted by descending queue hours")
    {
        model.rebuild(list.plates, bundle, PlateBoardGrouping::ByCapacity);
        REQUIRE(model.groups().size() == 3);

        for (size_t i = 1; i < model.groups().size(); ++i)
            CHECK(model.groups()[i - 1].queue_seconds >= model.groups()[i].queue_seconds);

        // The large bed carries 2 x 2h and leads; the inherited group has no sliced plate at all
        // and sinks, which a grouping that pinned it last could not express.
        CHECK(model.groups().front().key == "c:" + LARGE_PRINTER);
        CHECK_THAT(model.groups().front().queue_seconds, Catch::Matchers::WithinAbs(14400.f, 1e-3f));
        CHECK(model.groups().back().key == "c:");

        std::vector<int> ordered = rows_in_group_order(model);
        REQUIRE(ordered.size() == size_t(plate_count));
    }

    SECTION("a group key carries its mode, so collapse state cannot cross a mode change")
    {
        model.rebuild(list.plates, bundle, PlateBoardGrouping::ByMachine);
        const std::vector<std::string> by_machine = group_keys(model);

        model.rebuild(list.plates, bundle, PlateBoardGrouping::ByCapacity);
        const std::vector<std::string> by_capacity = group_keys(model);

        // the same machine, a different key
        for (const std::string &key : by_machine)
            CHECK(std::find(by_capacity.begin(), by_capacity.end(), key) == by_capacity.end());
    }

    SECTION("the collapsed section title names the machine count once the project holds several")
    {
        model.rebuild(list.plates, bundle, PlateBoardGrouping::ByMachine);
        const std::string summary = model.summary_text();
        CHECK(summary.find("3") != std::string::npos);
        CHECK(summary != PROJECT_PRINTER);
    }
}

TEST_CASE("PlateBoardModel names the project printer while every plate still follows it", "[PlateBoard]")
{
    PresetBundle bundle;
    build_bundle(bundle);

    SyntheticPlates list(3); // no plate carries an assignment

    PlateBoardModel model;
    model.rebuild(list.plates, bundle, PlateBoardGrouping::ByMachine);

    // Three plates naming no printer collapse into one empty machine key: the rollup counts
    // one machine and the summary is the empty name, said plainly rather than borrowed.
    CHECK(model.summary_text().empty());
    CHECK(model.rollup().machines == 1);
    // project_row() is gone with the project printer itself (2026-08-14): the board holds
    // only real plates, each owning its context; there is no synthetic Project row to assert.

    REQUIRE(model.groups().size() == 1);
    CHECK(model.groups().front().key == "m:");
    CHECK(model.groups().front().plate_count == 3);
}

// ----------------------------------------------------------------------------
// PartPlate's narrow reads, on the same harness
// ----------------------------------------------------------------------------
//
// The GUI-side tier-1 adapter in PartPlate.cpp reports "unresolved" for every plate here, because
// there is no wxApp and so no preset bundle. That is the same answer a real plate gets when its
// printer preset is not installed, which the picker explicitly supports ("Keep <name> (not
// installed)"), so it is the ordinary case rather than an artefact of the test.
//
// get_real_print_seq is the narrow read with teeth: ArrangeJob turns it into params.is_seq_print,
// which decides whether parts are packed with extruder clearance. Answering ByDefault for a plate
// the user set to ByObject is not a smaller answer, it is a wrong one, and it is silent.
TEST_CASE("A plate's own print sequence survives a context that does not resolve", "[PlateBoard][PlateContext]")
{
    SyntheticPlates list(2);

    SECTION("a plate that set no sequence of its own answers ByDefault")
    {
        bool same_as_global = true;
        CHECK(list.at(0).get_real_print_seq(&same_as_global) == PrintSequence::ByDefault);
        // an unresolved context cannot show that the plate matches a global value, so it does
        // not claim to
        CHECK_FALSE(same_as_global);
    }

    SECTION("a plate the user set to ByObject keeps ByObject")
    {
        list.at(1).config()->set_key_value("print_sequence",
                                           new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));
        REQUIRE(list.at(1).get_print_seq() == PrintSequence::ByObject);

        bool same_as_global = true;
        CHECK(list.at(1).get_real_print_seq(&same_as_global) == PrintSequence::ByObject);
        CHECK_FALSE(same_as_global);
    }

    SECTION("an explicit ByLayer is kept too, and is not confused with having said nothing")
    {
        list.at(1).config()->set_key_value("print_sequence",
                                           new ConfigOptionEnum<PrintSequence>(PrintSequence::ByLayer));
        bool same_as_global = true;
        CHECK(list.at(1).get_real_print_seq(&same_as_global) == PrintSequence::ByLayer);
        CHECK_FALSE(same_as_global);
        // and the plate that said nothing is still ByDefault, so the two are distinguishable
        CHECK(list.at(0).get_real_print_seq() == PrintSequence::ByDefault);
    }
}
