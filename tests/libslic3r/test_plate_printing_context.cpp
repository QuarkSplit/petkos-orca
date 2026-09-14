#include <catch2/catch_all.hpp>

#include "libslic3r/PlateSlicingContext.hpp"

using namespace Slic3r;

// The rule behind "an all-black plate is a single-filament print": slots holding the same
// spool are one filament. See PlateSlicingContext::single_spool_slot.
namespace {
PlateSlicingContext four_slots()
{
    PlateSlicingContext c;
    c.printer_preset_name    = "Kobra";
    c.print_preset_name      = "0.20mm";
    c.filament_preset_names  = {"PLA Black", "PLA Black", "PLA Red", "PLA Black"};
    c.filament_colours       = {"#000000", "#000000", "#FF0000", "#000000"};
    c.filament_colour_types  = {"1", "1", "1", "1"};
    c.filament_multi_colours = {"", "", "", ""};
    c.filament_finishes      = {0, 0, 0, 0};
    return c;
}
} // namespace

TEST_CASE("Two black slots are one spool", "[PlateSlicingContext]")
{
    const PlateSlicingContext c = four_slots();
    CHECK(c.single_spool_slot({1, 2}) == 1);
    CHECK(c.single_spool_slot({2, 4}) == 2);
    CHECK(c.single_spool_slot({4, 2, 1, 1}) == 1);
    CHECK(c.single_spool_slot({2}) == 2);
}

TEST_CASE("A different colour is a different spool", "[PlateSlicingContext]")
{
    const PlateSlicingContext c = four_slots();
    CHECK(c.single_spool_slot({1, 3}) == 0);
    CHECK(c.single_spool_slot({2, 3, 4}) == 0);
}

TEST_CASE("Same preset, different colour or finish, is a different spool", "[PlateSlicingContext]")
{
    PlateSlicingContext c = four_slots();
    c.filament_colours[1]  = "#111111";
    CHECK(c.single_spool_slot({1, 2}) == 0);
    c                      = four_slots();
    c.filament_finishes[1] = 1;
    CHECK(c.single_spool_slot({1, 2}) == 0);
    c                          = four_slots();
    c.filament_colour_types[1] = "2"; // gradient vs solid is a different spool
    CHECK(c.single_spool_slot({1, 2}) == 0);
    c                           = four_slots();
    c.filament_multi_colours[1] = "#000000,#FFFFFF";
    CHECK(c.single_spool_slot({1, 2}) == 0);
}

// The defect this file was opened for. PartPlate::get_extruders already reports the slots
// MMU paint names (ModelVolume::get_extruders merges mmuseg_extruders in), so painting is
// just another reference to a slot - and an object painted entirely in ONE slot names
// exactly that slot. The old signature took "is this plate painted at all" and refused the
// cut whenever it was true, which is why a monotone-painted plate sliced multi-filament.
TEST_CASE("Monotone paint is one spool; paint in a second spool is not", "[PlateSlicingContext]")
{
    const PlateSlicingContext c = four_slots();
    // every facet painted slot 2, object sits in slot 1: both black
    CHECK(c.single_spool_slot({1, 2}) == 1);
    // painted entirely in slot 4, nothing else referenced: black
    CHECK(c.single_spool_slot({4}) == 4);
    // painted in slot 3, which holds red: two spools, keep the full width
    CHECK(c.single_spool_slot({1, 3}) == 0);
}

TEST_CASE("Nothing to cut: one-slot context, no used slots, out-of-range slots", "[PlateSlicingContext]")
{
    PlateSlicingContext one;
    one.filament_preset_names = {"PLA Black"};
    CHECK(one.single_spool_slot({1}) == 0);
    const PlateSlicingContext c = four_slots();
    CHECK(c.single_spool_slot({}) == 0);
    CHECK(c.single_spool_slot({0, 9}) == 0);
    // A stale reference to a slot the plate does not have is not a spool to compare against.
    CHECK(c.single_spool_slot({9, 2}) == 2);
}

// An unset appearance cell is not a value; it is "whatever composition supplies". Comparing
// the raw cells made two identical spools read as different, which is the same defect one
// layer down. spool_in_slot normalises exactly as apply_plate_filament_colours does.
TEST_CASE("Unset appearance cells compare as composition's defaults", "[PlateSlicingContext]")
{
    PlateSlicingContext c;
    c.filament_preset_names = {"PLA Black", "PLA Black"};
    c.filament_colours      = {"#000000", "#000000"};
    // colour_type absent on one side, recorded blank on the other: both compose to solid "1"
    c.filament_colour_types = {"", "1"};
    CHECK(c.single_spool_slot({1, 2}) == 1);
    c.filament_colour_types.clear();
    CHECK(c.single_spool_slot({1, 2}) == 1);
    // multi colour blank composes to the slot's own colour, which is equal here
    c.filament_multi_colours = {"", "#000000"};
    CHECK(c.single_spool_slot({1, 2}) == 1);
    // finishes absent entirely: both Standard
    c.filament_finishes.clear();
    CHECK(c.single_spool_slot({1, 2}) == 1);
}

// The colour itself cannot be normalised without the filament preset, so the caller resolves
// it first (PresetBundle::plate_filament_colours, the same answer composition reaches). Left
// unresolved, a blank beside an explicit colour declines the cut - conservative, never wrong.
TEST_CASE("A blank colour beside an explicit one declines the cut until resolved", "[PlateSlicingContext]")
{
    PlateSlicingContext c;
    c.filament_preset_names = {"PLA Black", "PLA Black"};
    c.filament_colours      = {"#000000"}; // older project: colour vector shorter than the slots
    CHECK(c.single_spool_slot({1, 2}) == 0);
    c.filament_colours = {"#000000", "#000000"}; // what plate_filament_colours would produce
    CHECK(c.single_spool_slot({1, 2}) == 1);
}

TEST_CASE("cut_down_to_slot leaves one entry per vector and cuts the maps", "[PlateSlicingContext]")
{
    const PlateSlicingContext cut = four_slots().cut_down_to_slot(3);
    CHECK(cut.filament_preset_names == std::vector<std::string>{"PLA Red"});
    CHECK(cut.filament_colours == std::vector<std::string>{"#FF0000"});
    CHECK(cut.filament_colour_types.size() == 1);
    CHECK(cut.filament_multi_colours.size() == 1);
    CHECK(cut.filament_finishes == std::vector<int>{0});
    CHECK(cut.printer_preset_name == "Kobra");
    CHECK(PlateSlicingContext::cut_map_to_slot({1, 2, 2, 1}, 3, 1) == std::vector<int>{2});
    CHECK(PlateSlicingContext::cut_map_to_slot({1}, 3, 7) == std::vector<int>{7});
}

// The cut context is what compose_plate_slicing_config is fed, so whatever it says is what
// the G-code's filament-indexed arrays will be: one entry each, no second tool to change to.
TEST_CASE("A cut context is single-filament by construction", "[PlateSlicingContext]")
{
    const PlateSlicingContext c   = four_slots();
    const int                  slot = c.single_spool_slot({1, 2, 4});
    REQUIRE(slot == 1);
    const PlateSlicingContext cut = c.cut_down_to_slot(slot);
    CHECK(cut.filament_preset_names.size() == 1);
    CHECK(cut.filament_colours.size() == 1);
    CHECK(cut.filament_colour_types.size() == 1);
    CHECK(cut.filament_multi_colours.size() == 1);
    CHECK(cut.filament_finishes.size() == 1);
    CHECK(cut.is_complete());
    // and a context already one slot wide has nothing left to cut
    CHECK(cut.single_spool_slot({1}) == 0);
}
