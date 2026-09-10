#include <catch2/catch_all.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "slic3r/GUI/PlateProcessSettings.hpp"
#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <regex>

using namespace Slic3r;

namespace {

Preset &add_plate_preset(PresetCollection &collection, const std::string &name)
{
    DynamicPrintConfig config(collection.default_preset().config);
    return collection.load_preset({}, name, config, false);
}

struct PlateArchive {
    ScopedTemporaryFile file{".3mf"};
    boost::filesystem::path backup = file.path().string() + "-backup";
    PlateDataPtrs plates;
    std::vector<Preset *> presets;
    Model model;

    PlateArchive() { boost::filesystem::create_directories(backup); }
    ~PlateArchive() {
        release_PlateData_list(plates);
        for (Preset *preset : presets) delete preset;
        boost::system::error_code ec;
        boost::filesystem::remove_all(backup, ec);
    }
};

} // namespace

TEST_CASE("Plate material and explicit support edits survive a project round trip into fresh G-code",
          "[PlateConfiguration][Regression]")
{
    // Independent imported configurations: different process inheritance and PETG temperatures.
    const bool inherited_support = GENERATE(false, true);
    CAPTURE(inherited_support);
    PresetBundle bundle;
    const auto vendor = bundle.vendors.emplace("TEST", VendorProfile("TEST")).first;
    auto &printer = add_plate_preset(bundle.printers, "Imported printer");
    printer.vendor = &vendor->second;
    printer.printer_technology_ref() = ptFFF;
    printer.config.set_key_value("machine_start_gcode", new ConfigOptionString(""));
    printer.config.set_key_value("machine_end_gcode", new ConfigOptionString(""));
    printer.config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4});
    auto &process = add_plate_preset(bundle.prints, "Imported process");
    process.config.set_key_value("enable_support", new ConfigOptionBool(inherited_support));
    process.config.set_key_value("raft_layers", new ConfigOptionInt(0));
    process.config.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
    process.config.set_key_value("layer_height", new ConfigOptionFloat(0.3));
    process.config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.3));
    for (bool pla : {false, true}) {
        auto &material = add_plate_preset(bundle.filaments, pla ? "Selected PLA" : "Imported PETG");
        const int temperature = pla ? 220 : (inherited_support ? 250 : 245);
        material.config.set_key_value("filament_type", new ConfigOptionStrings{pla ? "PLA" : "PETG"});
        material.config.set_key_value("nozzle_temperature", new ConfigOptionInts{temperature});
        material.config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts{temperature});
        material.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{pla ? 0.93 : 0.98});
        material.config.set_key_value("fan_min_speed", new ConfigOptionFloats{pla ? 100.0 : 30.0});
        material.config.set_key_value("fan_max_speed", new ConfigOptionFloats{pla ? 100.0 : 30.0});
        material.config.set_key_value("close_fan_the_first_x_layers", new ConfigOptionInts{0});
        material.config.set_key_value("full_fan_speed_layer", new ConfigOptionInts{1});
    }
    // The editing cursor deliberately disagrees with the selected plate.
    bundle.filament_presets = {"Imported PETG"};
    bundle.prints.get_edited_preset().config.set_key_value("enable_support", new ConfigOptionBool(false));

    PlateData edited, untouched;
    edited.plate_index = 0;
    edited.slicing_context.printer_preset_name = "Imported printer";
    edited.slicing_context.printer_vendor_id = "TEST";
    edited.slicing_context.print_preset_name = "Imported process";
    edited.slicing_context.filament_preset_names = {"Imported PETG"};
    edited.slicing_context.filament_colours = {"#335577"};
    edited.config.set_key_value("enable_support", new ConfigOptionBool(true));
    if (inherited_support) {
        edited.config.set_key_value("filament_type", new ConfigOptionStrings{"PETG"});
        edited.config.set_key_value("nozzle_temperature", new ConfigOptionInts{250});
        edited.config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts{250});
        edited.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.98});
        edited.config.set_key_value("fan_min_speed", new ConfigOptionFloats{30.0});
        edited.config.set_key_value("fan_max_speed", new ConfigOptionFloats{30.0});
    }
    untouched.plate_index = 1;
    untouched.slicing_context = edited.slicing_context;
    untouched.config = edited.config;

    std::string error;
    REQUIRE(bundle.assign_plate_material(edited.slicing_context, 0, "Selected PLA", error));
    ResolvedPlateSlicingConfig assigned;
    REQUIRE(bundle.resolve_plate_slicing_config(edited.slicing_context, std::vector<int>{1},
                                                std::vector<int>{0}, assigned, error));
    bundle.rebase_assigned_material_overrides(untouched.slicing_context, edited.slicing_context,
                                              assigned.config, edited.config);
    DynamicPrintConfig inherited, effective;
    REQUIRE(GUI::resolve_plate_process_settings(bundle, edited.slicing_context, edited.config, inherited, effective, error));
    REQUIRE(effective.opt_bool("enable_support"));
    effective.set_key_value("enable_support", new ConfigOptionBool(false));
    ModelConfig editor_overrides;
    GUI::write_plate_process_options(editor_overrides, edited.config, effective, {"enable_support"});

    PlateArchive archive;
    Model source = Test::model("overhang", Test::mesh(Test::TestMesh::overhang));
    source.set_backup_path(archive.backup.string());
    source.add_default_instances();
    DynamicPrintConfig project_config = DynamicPrintConfig::full_print_config();
    StoreParams store;
    store.path = archive.file.string();
    store.model = &source;
    store.config = &project_config;
    store.plate_data_list = {&edited, &untouched};
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(store));
    archive.model.set_backup_path(archive.backup.string());
    DynamicPrintConfig loaded;
    ConfigSubstitutionContext substitutions{ForwardCompatibilitySubstitutionRule::Enable};
    bool is_bambu = false, is_orca = false;
    Semver version;
    REQUIRE(load_bbs_3mf(archive.file.string().c_str(), &loaded, &substitutions, &archive.model,
                        &archive.plates, &archive.presets, &is_bambu, &is_orca, &version, nullptr,
                        LoadStrategy::LoadModel | LoadStrategy::LoadConfig));
    REQUIRE(archive.plates.size() == 2);
    REQUIRE(archive.plates[0]->slicing_context == edited.slicing_context);
    REQUIRE(archive.plates[1]->slicing_context == untouched.slicing_context);
    REQUIRE_FALSE(archive.plates[0]->config.opt_bool("enable_support"));
    REQUIRE(archive.plates[1]->config.opt_bool("enable_support"));
    REQUIRE_FALSE(archive.model.objects.empty());

    for (size_t i = 0; i < archive.plates.size(); ++i) {
        CAPTURE(i);
        const auto &plate = *archive.plates[i];
        ResolvedPlateSlicingConfig resolved;
        REQUIRE(bundle.resolve_plate_slicing_config(plate.slicing_context, std::vector<int>{1},
                                                    std::vector<int>{0}, resolved, error));
        resolved.config.apply(plate.config);
        CHECK(resolved.config.opt_string("filament_type", 0u) == (i == 0 ? "PLA" : "PETG"));
        CHECK_THAT(resolved.config.option<ConfigOptionFloats>("filament_flow_ratio")->get_at(0),
                   Catch::Matchers::WithinAbs(i == 0 ? 0.93 : 0.98, 1e-9));
        Print print;
        Model print_model;
        Test::init_print({archive.model.objects.front()->raw_mesh()}, print, print_model, resolved.config);
        const std::string gcode = Test::gcode(print);
        const int temperature = i == 0 ? 220 : (inherited_support ? 250 : 245);
        CHECK(std::regex_search(gcode, std::regex("M10[49][^\\n]*S" + std::to_string(temperature) + "\\b")));
        if (i == 0) {
            CHECK(print.objects().front()->support_layers().empty());
            CHECK(Test::layers_with_role(gcode, "support").empty());
            CHECK(gcode.find("M106 S255") != std::string::npos);
            CHECK_FALSE(std::regex_search(gcode, std::regex("M10[49][^\\n]*S(245|250)\\b")));
        } else {
            CHECK_FALSE(print.objects().front()->support_layers().empty());
            CHECK_FALSE(Test::layers_with_role(gcode, "support").empty());
        }
    }
}
