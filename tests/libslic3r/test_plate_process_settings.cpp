#include <catch2/catch_all.hpp>

#include "slic3r/GUI/PlateProcessSettings.hpp"

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

struct PlateSettingsFixture
{
    PresetBundle bundle;
    PlateSlicingContext context;
    DynamicPrintConfig overrides;
    DynamicPrintConfig inherited;
    DynamicPrintConfig effective;
    std::string error;

    PlateSettingsFixture()
    {
        auto printer_config = bundle.printers.default_preset().config;
        auto &printer = bundle.printers.load_preset({}, "Plate printer", printer_config, false);
        printer.printer_technology_ref() = ptFFF;
        printer.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
        for (bool support : {false, true}) {
            auto config = bundle.prints.default_preset().config;
            config.set_key_value("enable_support", new ConfigOptionBool(support));
            bundle.prints.load_preset({}, support ? "Supports on" : "Supports off", config, false);
        }
        bundle.filaments.load_preset({}, "Plate material", bundle.filaments.default_preset().config, false);
        bundle.filament_presets = {"Plate material"};
        bundle.prints.select_preset_by_name("Supports off", true);

        context.printer_preset_name = "Plate printer";
        context.print_preset_name = "Supports on";
        context.filament_preset_names = {"Plate material"};
    }

    void refresh()
    {
        REQUIRE(resolve_plate_process_settings(bundle, context, overrides, inherited, effective, error));
    }

    void check_slice_agrees()
    {
        ResolvedPlateSlicingConfig slice;
        REQUIRE(bundle.resolve_plate_slicing_config(context, std::vector<int>{1}, std::vector<int>{0}, slice, error));
        slice.config.apply(overrides, true);
        CHECK(slice.config.opt_bool("enable_support") == effective.opt_bool("enable_support"));
    }
};

} // namespace

TEST_CASE("Plate support inspector follows the named process and stored overrides", "[PlateSettings][SupportMaterial][Regression]")
{
    PlateSettingsFixture f;

    SECTION("the global editing cursor is off and the plate process is on")
    {
        f.refresh();
        CHECK_FALSE(f.bundle.prints.get_edited_preset().config.opt_bool("enable_support"));
        CHECK(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
    }
    SECTION("an imported true plate override wins over an off process")
    {
        f.context.print_preset_name = "Supports off";
        f.overrides.set_key_value("enable_support", new ConfigOptionBool(true));
        f.refresh();
        CHECK_FALSE(f.inherited.opt_bool("enable_support"));
        CHECK(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
    }
    SECTION("an explicit false plate override wins over an on process")
    {
        f.overrides.set_key_value("enable_support", new ConfigOptionBool(false));
        f.refresh();
        CHECK(f.inherited.opt_bool("enable_support"));
        CHECK_FALSE(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
    }
    SECTION("project settings form the inherited layer below the plate")
    {
        DynamicPrintConfig project_edit;
        project_edit.set_key_value("enable_support", new ConfigOptionBool(false));
        f.bundle.park_as_project_overrides(project_edit, {"enable_support"});
        f.overrides.set_key_value("enable_support", new ConfigOptionBool(true));
        f.refresh();
        CHECK_FALSE(f.inherited.opt_bool("enable_support"));
        CHECK(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
    }
    SECTION("switching to another plate process clears the previous inherited value")
    {
        f.refresh();
        REQUIRE(f.effective.opt_bool("enable_support"));
        f.context.print_preset_name = "Supports off";
        f.refresh();
        CHECK_FALSE(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
        f.context.print_preset_name = "Supports on";
        f.refresh();
        CHECK(f.effective.opt_bool("enable_support"));
    }
}

TEST_CASE("Plate direct and dependent edits both update authoritative settings", "[PlateSettings][SupportMaterial][Regression]")
{
    PlateSettingsFixture f;
    f.overrides.set_key_value("enable_support", new ConfigOptionBool(true));
    f.overrides.set_key_value("wall_loops", new ConfigOptionInt(4));
    ModelConfig editor;
    editor.assign_config(f.overrides);
    f.refresh();

    SECTION("unchecking stores false even though the global cursor is already false")
    {
        f.effective.set_key_value("enable_support", new ConfigOptionBool(false));
        write_plate_process_options(editor, f.overrides, f.effective, {"enable_support"});
        REQUIRE(f.overrides.has("enable_support"));
        CHECK_FALSE(f.overrides.opt_bool("enable_support"));
        CHECK_FALSE(editor.get().opt_bool("enable_support"));
        f.refresh();
        CHECK_FALSE(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
    }
    SECTION("dependent corrections refresh a stale adapter without losing unrelated plate edits")
    {
        f.overrides.set_key_value("wall_loops", new ConfigOptionInt(7));
        f.effective.set_key_value("enable_support", new ConfigOptionBool(false));
        f.effective.set_key_value("raft_layers", new ConfigOptionInt(0));
        write_plate_process_options(editor, f.overrides, f.effective, {"enable_support", "raft_layers"});
        CHECK_FALSE(f.overrides.opt_bool("enable_support"));
        CHECK(f.overrides.opt_int("raft_layers") == 0);
        CHECK(f.overrides.opt_int("wall_loops") == 7);
        CHECK(editor.get().opt_int("wall_loops") == 7);
        CHECK(editor.get() == f.overrides);
        f.refresh();
        CHECK_FALSE(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
    }
    SECTION("reverting removes the override and restores the plate process value")
    {
        f.effective.set_key_value("enable_support", new ConfigOptionBool(false));
        write_plate_process_options(editor, f.overrides, f.effective, {"enable_support"});
        f.overrides.erase("enable_support");
        f.refresh();
        CHECK_FALSE(f.overrides.has("enable_support"));
        CHECK(f.effective.opt_bool("enable_support"));
        f.check_slice_agrees();
    }
}

TEST_CASE("Unresolved plate settings do not display another process", "[PlateSettings][Regression]")
{
    PlateSettingsFixture f;
    f.refresh();

    SECTION("a missing process clears the previously resolved result")
    {
        f.context.print_preset_name = "Missing imported process";
        CHECK_FALSE(resolve_plate_process_settings(f.bundle, f.context, f.overrides, f.inherited, f.effective, f.error));
        CHECK(f.inherited.empty());
        CHECK(f.effective.empty());
        CHECK_THAT(f.error, Catch::Matchers::ContainsSubstring("Missing imported process"));
    }
    SECTION("an unresolved material does not prevent editing a known process")
    {
        f.context.filament_preset_names = {"Missing imported material"};
        f.refresh();
        CHECK(f.effective.opt_bool("enable_support"));
        CHECK(f.context.filament_preset_names.front() == "Missing imported material");
    }
}

TEST_CASE("Material reassignment replaces stale plate values by material span", "[PlateSettings][PlateMaterial][Regression]")
{
    PlateSettingsFixture f;
    auto add_material = [&](const std::string &name, std::vector<int> temperatures, std::vector<double> flows) {
        DynamicPrintConfig config(f.bundle.filaments.default_preset().config);
        config.set_key_value("nozzle_temperature", new ConfigOptionInts(std::move(temperatures)));
        config.set_key_value("filament_flow_ratio", new ConfigOptionFloats(std::move(flows)));
        config.set_key_value("filament_type", new ConfigOptionStrings{name == "New PLA" ? "PLA" : "PETG"});
        config.set_key_value("fan_max_speed", new ConfigOptionFloats{name == "New PLA" ? 85.0 : 30.0});
        config.set_key_value("filament_notes", new ConfigOptionStrings{name});
        f.bundle.filaments.load_preset({}, name, config, false);
    };
    add_material("Old PETG", {250, 255}, {0.98, 0.99});
    add_material("New PLA", {210, 215, 220}, {0.91, 0.92, 0.93});
    add_material("Unchanged PETG", {260}, {0.97});
    auto previous = f.context;
    previous.filament_preset_names = {"Old PETG", "Unchanged PETG"};
    auto current = previous;
    current.filament_preset_names[0] = "New PLA";
    DynamicPrintConfig overrides;
    overrides.set_key_value("nozzle_temperature", new ConfigOptionInts{251, 256, 267});
    overrides.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1.01, 1.02, 1.07});
    overrides.set_key_value("filament_type", new ConfigOptionStrings{"PETG", "PETG"});
    overrides.set_key_value("fan_max_speed", new ConfigOptionFloats{22.0, 73.0});
    overrides.set_key_value("filament_notes", new ConfigOptionStrings{"Custom original notes", "Custom second notes"});
    overrides.set_key_value("filament_settings_id", new ConfigOptionStrings{"Old PETG", "Unchanged PETG"});
    overrides.set_key_value("enable_support", new ConfigOptionBool(false));
    overrides.set_key_value("wall_loops", new ConfigOptionInt(7));
    overrides.set_key_value("filament_colour", new ConfigOptionStrings{"#112233", "#445566"});
    overrides.set_key_value("default_filament_colour", new ConfigOptionStrings{"#112233", "#445566"});

    SECTION("composed variant widths shift the unchanged slot without discarding its override")
    {
        ResolvedPlateSlicingConfig resolved;
        REQUIRE(f.bundle.resolve_plate_slicing_config(current, std::vector<int>{1, 1}, std::vector<int>{0, 0}, resolved, f.error));
        f.bundle.rebase_assigned_material_overrides(previous, current, resolved.config, overrides);
        CHECK(overrides.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{210, 215, 220, 267});
        CHECK(overrides.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{0.91, 0.92, 0.93, 1.07});
    }
    SECTION("an unrelated unavailable material does not block incremental replacement")
    {
        previous.filament_preset_names[1] = "Missing imported material";
        current.filament_preset_names[1] = "Missing imported material";
        f.bundle.rebase_assigned_material_overrides(previous, current, DynamicPrintConfig{}, overrides);
        CHECK(overrides.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{210, 215, 220, 267});
        CHECK(overrides.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{0.91, 0.92, 0.93, 1.07});
        CHECK(current.filament_preset_names[1] == "Missing imported material");
    }
    SECTION("replacing a later material preserves every variant of the earlier slot")
    {
        current = previous;
        current.filament_preset_names[1] = "New PLA";
        f.bundle.rebase_assigned_material_overrides(previous, current, DynamicPrintConfig{}, overrides);
        CHECK(overrides.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{251, 256, 210, 215, 220});
        CHECK(overrides.option<ConfigOptionFloats>("filament_flow_ratio")->values == std::vector<double>{1.01, 1.02, 0.91, 0.92, 0.93});
    }
    SECTION("the missing old preset's width is recovered from the stored vector")
    {
        previous.filament_preset_names[0] = "Unavailable old PETG";
        f.bundle.rebase_assigned_material_overrides(previous, current, DynamicPrintConfig{}, overrides);
        CHECK(overrides.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{210, 215, 220, 267});
    }
    const size_t changed_slot = current.filament_preset_names[0] == "New PLA" ? 0 : 1;
    CHECK(overrides.option<ConfigOptionStrings>("filament_type")->values[changed_slot] == "PLA");
    CHECK(overrides.option<ConfigOptionStrings>("filament_settings_id")->values[changed_slot] == "New PLA");
    CHECK_THAT(overrides.option<ConfigOptionFloats>("fan_max_speed")->values[changed_slot], Catch::Matchers::WithinAbs(85.0, 1e-9));
    CHECK_THAT(overrides.option<ConfigOptionFloats>("fan_max_speed")->values[1 - changed_slot], Catch::Matchers::WithinAbs(changed_slot == 0 ? 73.0 : 22.0, 1e-9));
    CHECK(overrides.option<ConfigOptionStrings>("filament_notes")->values[changed_slot] == "New PLA");
    CHECK(overrides.option<ConfigOptionStrings>("filament_notes")->values[1 - changed_slot] == (changed_slot == 0 ? "Custom second notes" : "Custom original notes"));
    CHECK_FALSE(overrides.opt_bool("enable_support"));
    CHECK(overrides.opt_int("wall_loops") == 7);
    CHECK(overrides.option<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{"#112233", "#445566"});
    CHECK(overrides.option<ConfigOptionStrings>("default_filament_colour")->values == std::vector<std::string>{"#112233", "#445566"});
}
