#include <catch2/catch_all.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/AppConfig.hpp"

using namespace Slic3r;

namespace {

namespace fs = boost::filesystem;

struct TempPresetDir {
    fs::path path;

    TempPresetDir()
    {
        path = fs::temp_directory_path() / fs::unique_path("orcaslicer-preset-%%%%-%%%%-%%%%");
        fs::create_directories(path);
    }

    ~TempPresetDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_print_preset(const DynamicPrintConfig &default_config, const fs::path &file, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>("print_settings_id", true)->value = name;
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Write a preset json carrying a name and an "inherits" value, using the given collection's
// default config so it loads back into that collection. Works for any preset type.
void write_preset_with_inherits(const DynamicPrintConfig &default_config, const fs::path &file,
                                const std::string &name, const std::string &inherits)
{
    DynamicPrintConfig config(default_config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;

    fs::create_directories(file.parent_path());
    config.save_to_json(file.string(), name, "User", "1.0.0");
}

// Add an in-memory preset (no file) with the given inherits value (empty => root preset).
Preset &add_inmemory_preset(PresetCollection &coll, const std::string &name, const std::string &inherits = {})
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

// Mark an already-loaded preset as renamed from one or more former names.
void set_renamed_from(PresetCollection &coll, const std::string &preset_name, std::vector<std::string> old_names)
{
    for (auto it = coll.begin(); it != coll.end(); ++it)
        if (it->name == preset_name)
            it->renamed_from = std::move(old_names);
}

// A standalone print preset collection that exposes the protected rename-map builder, so a
// renamed_from scenario can be set up without the full system-profile load pipeline.
// (PresetCollection is non-copyable - it holds a mutex - so it is constructed directly with
// the same type/keys/defaults PresetBundle uses for its print collection.)
struct RenameTestCollection : public PresetCollection
{
    RenameTestCollection()
        : PresetCollection(Preset::TYPE_PRINT, Preset::print_options(),
                           static_cast<const PrintRegionConfig &>(FullPrintConfig::defaults()))
    {}
    using PresetCollection::update_map_system_profile_renamed;
};

} // namespace

TEST_CASE("Preset identity is canonicalized from load path", "[Preset][Identity]")
{
    TempPresetDir              temp_dir;
    PresetBundle               bundle;
    PresetsConfigSubstitutions substitutions;

    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_PRINT_NAME / "User.json", "User");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_LOCAL_DIR / "bundle-1" / PRESET_PRINT_NAME / "LocalBundle.json", "LocalBundle");
    write_print_preset(bundle.prints.default_preset().config, temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1" / PRESET_PRINT_NAME / "Subscribed.json", "Subscribed");

    bundle.prints.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_LOCAL_DIR / "bundle-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);
    bundle.prints.load_presets((temp_dir.path / PRESET_SUBSCRIBED_DIR / "remote-1").string(), PRESET_PRINT_NAME, substitutions, ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *root_user = bundle.prints.find_preset("User");
    REQUIRE(root_user != nullptr);
    CHECK(root_user->name == "User");
    CHECK_FALSE(root_user->is_from_bundle());

    const Preset *local_bundle = bundle.prints.find_preset("_local/bundle-1/LocalBundle");
    REQUIRE(local_bundle != nullptr);
    CHECK(local_bundle->name == "_local/bundle-1/LocalBundle");
    CHECK(local_bundle->is_from_bundle());

    const Preset *subscribed = bundle.prints.find_preset("_subscribed/remote-1/Subscribed");
    REQUIRE(subscribed != nullptr);
    CHECK(subscribed->name == "_subscribed/remote-1/Subscribed");
    CHECK(subscribed->is_from_bundle());
}

TEST_CASE("Legacy bundle import without bundle metadata stays in the user preset directory", "[Preset][Identity]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    PresetsConfigSubstitutions substitutions;
    std::vector<std::string>   result;
    int                        overwrite = 0;
    std::string                file      = (temp_dir.path / "legacy-bundle" / "Imported.json").string();
    const fs::path             user_root = temp_dir.path / "user";

    write_print_preset(bundle.prints.default_preset().config, file, "Imported");
    fs::create_directories(user_root);
    bundle.prints.update_user_presets_directory(user_root.string(), PRESET_PRINT_NAME);

    REQUIRE(bundle.import_json_presets(
        substitutions,
        file,
        [](std::string const &) { return 1; },
        ForwardCompatibilitySubstitutionRule::Disable,
        overwrite,
        result));

    const Preset *imported = bundle.prints.find_preset("Imported");
    REQUIRE(imported != nullptr);
    CHECK(imported->name == "Imported");
    CHECK(imported->bundle_id.empty());
    CHECK_FALSE(imported->is_from_bundle());
    // Detached user presets (no inherits) are saved in the "base" subfolder of the user preset root.
    CHECK(fs::equivalent(fs::path(imported->file).parent_path().parent_path(), user_root / PRESET_PRINT_NAME));
}

TEST_CASE("Current vendor type tolerates missing printer model", "[Preset][Bundle]")
{
    PresetBundle bundle;

    VendorProfile orca_vendor("ORCA");
    VendorProfile::PrinterModel model;
    model.name = "Orca Test";
    orca_vendor.models.emplace_back(model);
    bundle.vendors.emplace("ORCA", std::move(orca_vendor));

    bundle.printers.get_edited_preset().config.erase("printer_model");

    CHECK(bundle.get_current_vendor_type() == VendorType::Unknown);
}

// Carried against upstream, which asserted the opposite. Upstream returned 1 extruder when
// nozzle_diameter was missing or empty; this fork returns 0 and logs an error, because inventing
// an extruder count for a printer that never stated one is a fallback, and the count feeds
// filament-row sizing and per-plate nozzle mapping. Restoring the 1 would put a machine fact
// nobody supplied into the slicing context. See PresetBundle::get_printer_extruder_count.
TEST_CASE("Printer extruder count refuses to invent a missing nozzle diameter", "[Preset][Bundle]")
{
    PresetBundle bundle;
    DynamicPrintConfig& config = bundle.printers.get_edited_preset().config;

    config.erase("nozzle_diameter");
    CHECK(bundle.get_printer_extruder_count() == 0);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats());
    CHECK(bundle.get_printer_extruder_count() == 0);

    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6 }));
    CHECK(bundle.get_printer_extruder_count() == 2);
}

TEST_CASE("find_preset resolves a system preset's renamed_from", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" is the current preset; it was renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // The rename map knows the old name...
    const std::string *renamed = coll.get_preset_name_renamed("Old Process");
    REQUIRE(renamed != nullptr);
    CHECK(*renamed == "New Process");

    // ...and plain find_preset() now follows it (the core of this PR; previously this
    // resolution lived only in find_preset2 and a few call sites).
    const Preset *resolved = coll.find_preset("Old Process");
    REQUIRE(resolved != nullptr);
    CHECK(resolved->name == "New Process");

    // A genuinely unknown name still returns null (no spurious match).
    CHECK(coll.find_preset("Totally Unknown") == nullptr);

    // A child that still inherits the OLD name resolves through the runtime walker,
    // which uses plain find_preset().
    Preset       &child  = add_inmemory_preset(coll, "Child Process", "Old Process");
    const Preset *parent = coll.get_preset_parent(child);
    REQUIRE(parent != nullptr);
    CHECK(parent->name == "New Process");
}

TEST_CASE("find_preset resolves a preset renamed more than once", "[Preset][Rename]")
{
    RenameTestCollection coll;

    // "New Process" was renamed twice, so it carries both former names in renamed_from.
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Original Process", "Old Process" });
    coll.update_map_system_profile_renamed();

    // Each historical name resolves to the current preset.
    for (const char *old_name : { "Original Process", "Old Process" }) {
        INFO("resolving old name: " << old_name);
        const std::string *renamed = coll.get_preset_name_renamed(old_name);
        REQUIRE(renamed != nullptr);
        CHECK(*renamed == "New Process");

        const Preset *resolved = coll.find_preset(old_name);
        REQUIRE(resolved != nullptr);
        CHECK(resolved->name == "New Process");
    }

    // A child inheriting either former name resolves through the runtime walker.
    Preset &child = add_inmemory_preset(coll, "Child Process", "Original Process");
    REQUIRE(coll.get_preset_parent(child) != nullptr);
    CHECK(coll.get_preset_parent(child)->name == "New Process");
}

TEST_CASE("find_preset2 auto-matches removed Generic vendor profiles to the library", "[Preset][Rename]")
{
    PresetBundle bundle;

    // The OrcaFilamentLibrary replacement that removed empty "<vendor> Generic" profiles map to.
    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // Plain lookups do NOT fuzzy-match a removed vendor profile.
    CHECK(bundle.filaments.find_preset("Voron Generic PLA") == nullptr);
    CHECK(bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/false) == nullptr);

    // With auto_match, the removed "Voron Generic PLA" resolves to "Generic PLA @System".
    const Preset *matched = bundle.filaments.find_preset2("Voron Generic PLA", /*auto_match=*/true);
    REQUIRE(matched != nullptr);
    CHECK(matched->name == "Generic PLA @System");

    // No library preset exists for an unrelated material => still no match.
    CHECK(bundle.filaments.find_preset2("BrandX Generic PETG", /*auto_match=*/true) == nullptr);
}

TEST_CASE("Renamed parent is normalized into a loaded preset's inherits", "[Preset][Rename]")
{
    TempPresetDir        temp_dir;
    RenameTestCollection coll;

    // Current parent, renamed from "Old Process".
    add_inmemory_preset(coll, "New Process");
    set_renamed_from(coll, "New Process", { "Old Process" });
    coll.update_map_system_profile_renamed();

    // A user preset on disk that still inherits the OLD name.
    write_preset_with_inherits(coll.default_preset().config,
                               temp_dir.path / PRESET_PRINT_NAME / "Child.json", "Child", "Old Process");

    PresetsConfigSubstitutions substitutions;
    coll.load_presets(temp_dir.path.string(), PRESET_PRINT_NAME, substitutions,
                      ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = coll.find_preset("Child");
    REQUIRE(child != nullptr);
    // The dangling "Old Process" was rewritten to the resolved parent name at load time,
    // so the runtime walker (plain find_preset) can resolve the chain.
    CHECK(child->inherits() == "New Process");
    REQUIRE(coll.get_preset_parent(*child) != nullptr);
    CHECK(coll.get_preset_parent(*child)->name == "New Process");
}

TEST_CASE("Removed Generic parent is normalized into a loaded filament's inherits", "[Preset][Rename]")
{
    TempPresetDir temp_dir;
    PresetBundle  bundle;

    add_inmemory_preset(bundle.filaments, "Generic PLA @System");

    // A user filament that still inherits a removed "<vendor> Generic PLA" profile.
    write_preset_with_inherits(bundle.filaments.default_preset().config,
                               temp_dir.path / PRESET_FILAMENT_NAME / "MyPLA.json", "MyPLA", "Voron Generic PLA");

    PresetsConfigSubstitutions substitutions;
    bundle.filaments.load_presets(temp_dir.path.string(), PRESET_FILAMENT_NAME, substitutions,
                                  ForwardCompatibilitySubstitutionRule::Disable);

    const Preset *child = bundle.filaments.find_preset("MyPLA");
    REQUIRE(child != nullptr);
    CHECK(child->inherits() == "Generic PLA @System");
    REQUIRE(bundle.filaments.get_preset_parent(*child) != nullptr);
    CHECK(bundle.filaments.get_preset_parent(*child)->name == "Generic PLA @System");
}

namespace {

// A live reference to a preset's compatible_printers / compatible_prints list. Fetches the *stored*
// preset (real=true) so writes and reads hit the same object; creates the option if absent.
std::vector<std::string> &compatible_list(PresetCollection &coll, const std::string &preset_name, const char *field_key)
{
    Preset *preset = coll.find_preset(preset_name, /*first_visible_if_not_found=*/false, /*real=*/true);
    REQUIRE(preset != nullptr);
    return preset->config.option<ConfigOptionStrings>(field_key, true)->values;
}

} // namespace

TEST_CASE("Renamed printer/process names are normalized into compatible lists on load", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A user process still compatible with the OLD printer name.
    add_inmemory_preset(bundle.prints, "My Process");
    compatible_list(bundle.prints, "My Process", "compatible_printers") = { "Old Printer" };

    // A user filament referencing the OLD printer AND OLD process names, plus an unknown printer.
    add_inmemory_preset(bundle.filaments, "My Filament");
    compatible_list(bundle.filaments, "My Filament", "compatible_printers") = { "Old Printer", "Unknown Printer" };
    compatible_list(bundle.filaments, "My Filament", "compatible_prints")   = { "Old Process" };

    // Build the rename maps (done during system load in the real pipeline), then normalize.
    AppConfig app_config;
    bundle.load_installed_printers(app_config); // rebuilds every collection's rename map
    bundle.normalize_compatible_presets();

    // The stale printer name in a process' compatible_printers is rewritten to the current name.
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });

    // The stale process name in a filament's compatible_prints is rewritten (this field has no
    // runtime rename fallback, so load-time normalization is the only fix).
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_prints") == std::vector<std::string>{ "New Process" });

    // The renamed printer is rewritten while the unknown/deleted name is preserved as-is.
    CHECK(compatible_list(bundle.filaments, "My Filament", "compatible_printers") ==
          (std::vector<std::string>{ "New Printer", "Unknown Printer" }));

    // Normalizing rewrites config in place without flagging the preset dirty.
    CHECK_FALSE(bundle.prints.find_preset("My Process", false, true)->is_dirty);

    // A system preset that already references the current name is left untouched (idempotent no-op).
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.prints, "My Process", "compatible_printers") == std::vector<std::string>{ "New Printer" });
}

TEST_CASE("Renamed names are normalized into a SYSTEM preset's compatible lists", "[Preset][Rename]")
{
    PresetBundle bundle;

    // Current printer + process, each renamed from an older name.
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });
    add_inmemory_preset(bundle.prints, "New Process");
    set_renamed_from(bundle.prints, "New Process", { "Old Process" });

    // A *system* (vendor) filament whose own compatible lists still reference the OLD names. A vendor
    // profile can point at a sibling preset that was later renamed, so system presets must be
    // normalized too (they are skipped by neither collection walk).
    add_inmemory_preset(bundle.filaments, "System Filament").is_system = true;
    compatible_list(bundle.filaments, "System Filament", "compatible_printers") = { "Old Printer" };
    compatible_list(bundle.filaments, "System Filament", "compatible_prints")   = { "Old Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps
    bundle.normalize_compatible_presets();

    // The stale references in the system preset are rewritten to the current names.
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_prints") ==
          std::vector<std::string>{ "New Process" });

    // The rewrite does not flag the system preset dirty, and is idempotent.
    CHECK_FALSE(bundle.filaments.find_preset("System Filament", false, true)->is_dirty);
    bundle.normalize_compatible_presets();
    CHECK(compatible_list(bundle.filaments, "System Filament", "compatible_printers") ==
          std::vector<std::string>{ "New Printer" });
}

TEST_CASE("compatible_prints on SLA materials resolves against sla_prints, not prints", "[Preset][Rename]")
{
    PresetBundle bundle;

    // A renamed SLA process, and a same-named FFF process that must NOT be picked up: resolving the
    // SLA material's compatible_prints against `prints` would wrongly rewrite to "Wrong FFF Process".
    add_inmemory_preset(bundle.sla_prints, "New SLA Process");
    set_renamed_from(bundle.sla_prints, "New SLA Process", { "Old SLA Process" });
    add_inmemory_preset(bundle.prints, "Wrong FFF Process");
    set_renamed_from(bundle.prints, "Wrong FFF Process", { "Old SLA Process" });

    add_inmemory_preset(bundle.sla_materials, "My SLA Material");
    compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") = { "Old SLA Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config);
    bundle.normalize_compatible_presets();

    CHECK(compatible_list(bundle.sla_materials, "My SLA Material", "compatible_prints") ==
          std::vector<std::string>{ "New SLA Process" });
}

TEST_CASE("Profile validator flags dangling and renamed preset references", "[Preset][Validate]")
{
    PresetBundle bundle;

    // Current printers: a real one, and a renamed one (its old name resolves via renamed_from).
    add_inmemory_preset(bundle.printers, "Real Printer");
    add_inmemory_preset(bundle.printers, "New Printer");
    set_renamed_from(bundle.printers, "New Printer", { "Old Printer" });

    // A real process, referenced from a filament's compatible_prints.
    add_inmemory_preset(bundle.prints, "Real Process").is_system = true;

    // A fully valid system filament: references only current names.
    add_inmemory_preset(bundle.filaments, "Good Filament").is_system = true;
    compatible_list(bundle.filaments, "Good Filament", "compatible_printers") = { "Real Printer" };
    compatible_list(bundle.filaments, "Good Filament", "compatible_prints")   = { "Real Process" };

    AppConfig app_config;
    bundle.load_installed_printers(app_config); // build the rename maps

    // With only valid references, the validator is clean.
    CHECK_FALSE(bundle.check_preset_references());

    SECTION("deleted compatible_printers is flagged") {
        add_inmemory_preset(bundle.filaments, "Ghost Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Ghost Ref Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("renamed compatible_printers (old name) is flagged") {
        add_inmemory_preset(bundle.filaments, "Old Ref Filament").is_system = true;
        compatible_list(bundle.filaments, "Old Ref Filament", "compatible_printers") = { "Old Printer" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted compatible_prints is flagged") {
        add_inmemory_preset(bundle.filaments, "Bad Process Ref").is_system = true;
        compatible_list(bundle.filaments, "Bad Process Ref", "compatible_prints") = { "Ghost Process" };
        CHECK(bundle.check_preset_references());
    }

    SECTION("deleted inherits parent is flagged") {
        add_inmemory_preset(bundle.filaments, "Orphan Filament", "Ghost Parent").is_system = true;
        CHECK(bundle.check_preset_references());
    }

    SECTION("non-system preset with a dangling reference is ignored") {
        add_inmemory_preset(bundle.filaments, "User Filament"); // is_system stays false
        compatible_list(bundle.filaments, "User Filament", "compatible_printers") = { "Ghost Printer" };
        CHECK_FALSE(bundle.check_preset_references());
    }
}

TEST_CASE("Plate slicing context resolves exact FDM presets without Project-printer fallback", "[Preset][PlateContext]")
{
    PresetBundle bundle;

    auto [vendor_it, inserted] = bundle.vendors.emplace("TEST", VendorProfile("TEST"));
    REQUIRE(inserted);

    Preset &printer = add_inmemory_preset(bundle.printers, "Plate Printer");
    printer.vendor = &vendor_it->second;
    printer.printer_technology_ref() = ptFFF;
    printer.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.6}));
    printer.config.set_key_value("printer_model", new ConfigOptionString("Plate Model"));

    Preset &process = add_inmemory_preset(bundle.prints, "Plate Process");
    process.config.set_key_value("layer_height", new ConfigOptionFloat(0.27));

    Preset &filament = add_inmemory_preset(bundle.filaments, "Plate Filament");
    filament.filament_id = "FIL-001";

    // Make the Project row observably different. A named plate context must not
    // obtain either value from these edited presets.
    bundle.printers.get_edited_preset().config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.25}));
    bundle.printers.get_edited_preset().config.set_key_value("printer_model", new ConfigOptionString("Project Model"));
    bundle.prints.get_edited_preset().config.set_key_value("layer_height", new ConfigOptionFloat(0.11));

    PlateSlicingContext context;
    context.printer_preset_name   = "Plate Printer";
    context.printer_vendor_id     = "TEST";
    context.print_preset_name     = "Plate Process";
    context.filament_preset_names = {"Plate Filament"};

    ResolvedPlateSlicingConfig resolved;
    std::string error;

    SECTION("the complete effective config comes from the named plate presets") {
        REQUIRE(bundle.resolve_plate_slicing_config(context, std::vector<int>{1}, std::vector<int>{0}, resolved, error));
        CHECK(error.empty());
        REQUIRE(resolved.printer_preset != nullptr);
        REQUIRE(resolved.print_preset != nullptr);
        CHECK(resolved.printer_preset->name == "Plate Printer");
        CHECK(resolved.print_preset->name == "Plate Process");
        REQUIRE(resolved.filament_presets.size() == 1);
        CHECK(resolved.filament_presets.front()->name == "Plate Filament");
        CHECK(resolved.config.opt_string("printer_model") == "Plate Model");
        CHECK_THAT(resolved.config.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.27, 1e-9));
        CHECK_THAT(resolved.config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0),
                   Catch::Matchers::WithinAbs(0.6, 1e-9));
        CHECK(resolved.config.opt_string("printer_vendor_id") == "TEST");
        CHECK(resolved.printer_vendor_id == "TEST");
        CHECK_FALSE(resolved.is_bbl_printer);
    }

    SECTION("a missing named printer is an error") {
        context.printer_preset_name = "Missing Printer";
        CHECK_FALSE(bundle.resolve_plate_slicing_config(context, std::vector<int>{1}, std::vector<int>{0}, resolved, error));
        CHECK(error.find("Missing Printer") != std::string::npos);
    }

    SECTION("a persisted vendor mismatch is an error") {
        context.printer_vendor_id = "OTHER";
        CHECK_FALSE(bundle.resolve_plate_slicing_config(context, std::vector<int>{1}, std::vector<int>{0}, resolved, error));
        CHECK(error.find("recorded vendor 'OTHER'") != std::string::npos);
    }

    SECTION("a non-FDM printer is an error") {
        printer.printer_technology_ref() = ptSLA;
        CHECK_FALSE(bundle.resolve_plate_slicing_config(context, std::vector<int>{1}, std::vector<int>{0}, resolved, error));
        CHECK(error.find("not an FDM printer") != std::string::npos);
    }

    SECTION("AMS filament identity is exact and ambiguity is rejected") {
        REQUIRE(bundle.resolve_plate_slicing_config(context, std::vector<int>{1}, std::vector<int>{0}, resolved, error));

        DynamicPrintConfig tray;
        tray.set_key_value("filament_id", new ConfigOptionStrings({"FIL-001"}));
        tray.set_key_value("tray_name", new ConfigOptionStrings({"A1"}));

        const Preset *matched = nullptr;
        REQUIRE(bundle.resolve_ams_filament_preset(tray, resolved, 0, matched, error));
        REQUIRE(matched != nullptr);
        CHECK(matched->name == "Plate Filament");

        tray.option<ConfigOptionStrings>("filament_id")->values.front() = "MISSING";
        CHECK_FALSE(bundle.resolve_ams_filament_preset(tray, resolved, std::nullopt, matched, error));
        CHECK(error.find("no exact compatible preset") != std::string::npos);

        tray.option<ConfigOptionStrings>("filament_id")->values.front() = "FIL-001";
        Preset &ambiguous = add_inmemory_preset(bundle.filaments, "Another Plate Filament");
        ambiguous.filament_id = "FIL-001";
        CHECK_FALSE(bundle.resolve_ams_filament_preset(tray, resolved, std::nullopt, matched, error));
        CHECK(error.find("multiple compatible presets") != std::string::npos);
    }
}

// ----------------------------------------------------------------------------
// Tier 1: PresetBundle::resolve_plate_presets and PresetBundle::plate_process_option
// ----------------------------------------------------------------------------
//
// The plate-inheritance rule has one home. It used to have four, because the render path asks
// identity questions once per plate per frame and the full resolver answers them by copying every
// preset in the plate's context. The copies drifted, and the drift was not theoretical: two of
// them checked neither the nozzle definition nor the recorded vendor, and the two process copies
// checked no printer at all, so a plate the slicer calls unresolved still handed a process value
// back to the sidebar.
//
// These cases pin tier 1's own behaviour, then pin the three specific disagreements by asking all
// three surfaces about one context and requiring the same verdict. That agreement is the property
// the fold exists to create, and it is a property the copies did not have.

namespace {

// A minimal FDM bundle with one named printer, process and filament, and a Project row whose
// values differ in every field the plate also names. Anything reading the Project row where it
// should read the plate then shows up as a wrong value rather than as a pass.
struct PlateContextFixture
{
    PresetBundle bundle;
    // Valid only while no further preset is added to the SAME collection: PresetCollection holds
    // its presets in a std::deque and inserts in sorted order, so an insertion invalidates
    // references into it. Nothing below adds a printer or a process after construction except
    // deselect_via_project_embedded, and no case uses these pointers together with that.
    Preset *     printer = nullptr;
    Preset *     process = nullptr;

    PlateContextFixture()
    {
        auto [vendor_it, inserted] = bundle.vendors.emplace("TEST", VendorProfile("TEST"));
        REQUIRE(inserted);

        printer = &add_inmemory_preset(bundle.printers, "Plate Printer");
        printer->vendor                   = &vendor_it->second;
        printer->printer_technology_ref() = ptFFF;
        printer->config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.6}));
        printer->config.set_key_value("printer_model", new ConfigOptionString("Plate Model"));

        process = &add_inmemory_preset(bundle.prints, "Plate Process");
        process->config.set_key_value("layer_height", new ConfigOptionFloat(0.27));
        process->config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByObject));

        Preset &filament = add_inmemory_preset(bundle.filaments, "Plate Filament");
        filament.filament_id = "FIL-001";
        bundle.filament_presets = {"Plate Filament"};

        // The Project row, deliberately different everywhere it can be.
        bundle.printers.get_edited_preset().printer_technology_ref() = ptFFF;
        bundle.printers.get_edited_preset().config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.25}));
        bundle.printers.get_edited_preset().config.set_key_value("printer_model", new ConfigOptionString("Project Model"));
        bundle.prints.get_edited_preset().config.set_key_value("layer_height", new ConfigOptionFloat(0.11));
        bundle.prints.get_edited_preset().config.set_key_value("print_sequence",
                                                               new ConfigOptionEnum<PrintSequence>(PrintSequence::ByLayer));
    }

    PlateSlicingContext named_context() const
    {
        PlateSlicingContext context;
        context.printer_preset_name   = "Plate Printer";
        context.printer_vendor_id     = "TEST";
        context.print_preset_name     = "Plate Process";
        context.filament_preset_names = {"Plate Filament"};
        return context;
    }
};

// Leave a collection with no resolved selection, by the route the application actually takes:
// a project-embedded preset is selected, then the project is closed and its embedded presets are
// removed. That leaves m_idx_selected == size_t(-1) AND a stale edited preset still in memory,
// which is the pair that matters - reaching for the edited preset there is the fallback the fork
// forbids. (select_preset_by_name_strict, the other route into this state, is protected.)
void deselect_via_project_embedded(PresetCollection &collection, const std::string &name)
{
    add_inmemory_preset(collection, name).is_project_embedded = true;
    collection.select_preset_by_name(name, true);
    REQUIRE(collection.get_selected_preset_name() == name);
    REQUIRE(collection.reset_project_embedded_presets());
    REQUIRE(collection.get_selected_idx() == size_t(-1));
    REQUIRE(collection.get_edited_preset().name == name); // the stale preset is still there
}

// The three surfaces that each used to carry their own copy of the rule: tier 1, tier 2, and the
// narrow process read the sidebar and the render path use.
struct PlateContextVerdicts
{
    bool tier1 = false;
    bool tier2 = false;
    bool process_read = false;

    bool all_agree() const { return tier1 == tier2 && tier2 == process_read; }
};

PlateContextVerdicts ask_every_surface(const PresetBundle &bundle, const PlateSlicingContext &context)
{
    PlateContextVerdicts verdicts;

    ResolvedPlatePresets presets;
    std::string          tier1_error;
    verdicts.tier1 = bundle.resolve_plate_presets(context, presets, tier1_error);

    ResolvedPlateSlicingConfig resolved;
    std::string                tier2_error;
    verdicts.tier2 = bundle.resolve_plate_slicing_config(context, std::vector<int>{1}, std::vector<int>{0},
                                                         resolved, tier2_error);

    verdicts.process_read = bundle.plate_process_option(context, nullptr, "print_sequence") != nullptr;
    return verdicts;
}

} // namespace

TEST_CASE("Tier-1 plate preset resolution follows the inheritance rule exactly", "[Preset][PlateContext]")
{
    PlateContextFixture        fixture;
    PresetBundle &             bundle = fixture.bundle;
    const PlateSlicingContext  named  = fixture.named_context();

    ResolvedPlatePresets presets;
    // Seeded non-empty so a resolver that forgets to clear it on success is caught.
    std::string error = "not cleared";

    SECTION("a named context resolves by exact name and never from the Project row")
    {
        REQUIRE(bundle.resolve_plate_presets(named, presets, error));
        CHECK(error.empty());
        REQUIRE(presets.printer != nullptr);
        REQUIRE(presets.print != nullptr);
        CHECK(presets.printer->name == "Plate Printer");
        CHECK(presets.print->name == "Plate Process");
        CHECK(presets.printer_vendor_id == "TEST");
        CHECK_FALSE(presets.is_bbl_printer);
        // the plate's 0.6 nozzle, not the Project row's 0.25
        REQUIRE(presets.printer->config.option<ConfigOptionFloats>("nozzle_diameter") != nullptr);
        CHECK_THAT(presets.printer->config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0),
                   Catch::Matchers::WithinAbs(0.6, 1e-9));
    }

    SECTION("an empty context is a refusal, not an inheritance")
    {
        // The project printer was deleted as a state (2026-08-14). An all-empty context
        // used to resolve to whatever was being edited; now it names nothing and that is
        // an error - the edited preset is right there and deliberately not reached for.
        const PlateSlicingContext inherited; // every field empty
        CHECK_FALSE(bundle.resolve_plate_presets(inherited, presets, error));
        CHECK(error == "The plate names no printer");
        CHECK(presets.printer == nullptr);
        CHECK(presets.print == nullptr);
    }

    SECTION("a named printer with an empty process slot is still unresolved")
    {
        PlateSlicingContext printer_only = named;
        printer_only.print_preset_name.clear();
        CHECK_FALSE(bundle.resolve_plate_presets(printer_only, presets, error));
        CHECK(error == "The plate names no process");
        // a refusal hands back nothing, not a half-resolved pair
        CHECK(presets.printer == nullptr);
        CHECK(presets.print == nullptr);
    }

    SECTION("a named preset that is not installed is unresolved, never a nearest match")
    {
        PlateSlicingContext missing = named;
        missing.printer_preset_name = "Plate Printer Mk2";
        CHECK_FALSE(bundle.resolve_plate_presets(missing, presets, error));
        CHECK(error.find("Plate Printer Mk2") != std::string::npos);
        // and specifically NOT the similarly named preset that does exist
        CHECK(presets.printer == nullptr);
        CHECK(presets.print == nullptr);

        missing = named;
        missing.print_preset_name = "Plate Process Mk2";
        CHECK_FALSE(bundle.resolve_plate_presets(missing, presets, error));
        CHECK(error.find("Plate Process Mk2") != std::string::npos);
        CHECK(presets.print == nullptr);
    }

    SECTION("a broken Project printer selection cannot leak into plate resolution")
    {
        // Stronger than the section it replaces: nothing reads the Project selection on a
        // plate's behalf any more, so its state cannot matter. An empty context refuses for
        // its own reason, and a plate that names its machine resolves regardless.
        deselect_via_project_embedded(bundle.printers, "Embedded Project Printer");

        const PlateSlicingContext inherited;
        CHECK_FALSE(bundle.resolve_plate_presets(inherited, presets, error));
        CHECK(error == "The plate names no printer");
        CHECK(presets.printer == nullptr);

        // a plate that names its own machine is untouched by the Project row being broken
        CHECK(bundle.resolve_plate_presets(named, presets, error));
        CHECK(presets.printer->name == "Plate Printer");
    }

    SECTION("an unresolved Project process selection is an error")
    {
        deselect_via_project_embedded(bundle.prints, "Embedded Project Process");

        PlateSlicingContext printer_only = named;
        printer_only.print_preset_name.clear();
        CHECK_FALSE(bundle.resolve_plate_presets(printer_only, presets, error));
        CHECK(error == "The plate names no process");
        CHECK(presets.print == nullptr);

        // the plate's own named process is unaffected
        CHECK(bundle.resolve_plate_presets(named, presets, error));
        CHECK(presets.print->name == "Plate Process");
    }

    SECTION("tier 1 throws nothing, whatever it is handed")
    {
        // A query and an action must not share a throw. Tier 1 is reached from const queries and
        // from the render loop, where an exception unwinds a wx event handler and terminates the
        // application; every one of these must come back as a false with a reason instead.
        PlateSlicingContext hostile;
        hostile.printer_preset_name   = std::string(4096, 'x');
        hostile.printer_vendor_id     = "NOT-A-VENDOR";
        hostile.print_preset_name     = "\n\t\"'<&>";
        hostile.filament_preset_names = {"", "Ghost Filament"};
        CHECK_NOTHROW(bundle.resolve_plate_presets(hostile, presets, error));
        CHECK_FALSE(bundle.resolve_plate_presets(hostile, presets, error));

        deselect_via_project_embedded(bundle.printers, "Embedded Project Printer");
        CHECK_NOTHROW(bundle.resolve_plate_presets(PlateSlicingContext(), presets, error));

        // and the narrow read on top of it, including a key no ConfigDef in this build carries
        CHECK_NOTHROW(bundle.plate_process_option(hostile, nullptr, "print_sequence"));
        CHECK_NOTHROW(bundle.plate_process_option(named, nullptr, "petkos_not_a_process_option"));
        CHECK_NOTHROW(bundle.plate_process_option(PlateSlicingContext(), nullptr, "print_sequence"));
    }

    SECTION("tier 1 writes nothing, on the failing path or the succeeding one")
    {
        const size_t      printer_idx    = bundle.printers.get_selected_idx();
        const size_t      print_idx      = bundle.prints.get_selected_idx();
        const std::string edited_printer = bundle.printers.get_edited_preset().name;
        const double      edited_nozzle =
            bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0);

        PlateSlicingContext missing = named;
        missing.printer_preset_name = "Ghost Printer";
        CHECK_FALSE(bundle.resolve_plate_presets(missing, presets, error));
        CHECK(bundle.printers.get_selected_idx() == printer_idx);
        CHECK(bundle.prints.get_selected_idx() == print_idx);
        CHECK(bundle.printers.get_edited_preset().name == edited_printer);

        REQUIRE(bundle.resolve_plate_presets(named, presets, error));
        CHECK(bundle.printers.get_selected_idx() == printer_idx);
        CHECK(bundle.prints.get_selected_idx() == print_idx);
        CHECK(bundle.printers.get_edited_preset().name == edited_printer);
        CHECK_THAT(bundle.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0),
                   Catch::Matchers::WithinAbs(edited_nozzle, 1e-9));
    }
}

TEST_CASE("The three plate-context surfaces cannot disagree", "[Preset][PlateContext]")
{
    PlateContextFixture       fixture;
    PresetBundle &            bundle = fixture.bundle;
    const PlateSlicingContext named  = fixture.named_context();

    SECTION("a context that resolves: all three say yes")
    {
        const PlateContextVerdicts verdicts = ask_every_surface(bundle, named);
        CHECK(verdicts.tier1);
        CHECK(verdicts.tier2);
        CHECK(verdicts.process_read);
        CHECK(verdicts.all_agree());
    }

    SECTION("divergence 1: a printer preset with no nozzle definition")
    {
        // The GUI identity copy checked only the technology and the vendor, so it accepted a
        // machine the full resolver refuses. Nothing downstream can size a toolhead from it.
        fixture.printer->config.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::vector<double>{}));

        const PlateContextVerdicts verdicts = ask_every_surface(bundle, named);
        CHECK_FALSE(verdicts.tier1);
        CHECK_FALSE(verdicts.tier2);
        CHECK_FALSE(verdicts.process_read);
        CHECK(verdicts.all_agree());

        ResolvedPlatePresets presets;
        std::string          error;
        CHECK_FALSE(bundle.resolve_plate_presets(named, presets, error));
        CHECK(error.find("no nozzle definition") != std::string::npos);
    }

    SECTION("divergence 2: a printer preset that is not an FDM machine")
    {
        // Neither process copy looked at the printer at all, so both answered from the process
        // preset of a plate whose machine this FDM-only fork cannot slice for.
        fixture.printer->printer_technology_ref() = ptSLA;

        const PlateContextVerdicts verdicts = ask_every_surface(bundle, named);
        CHECK_FALSE(verdicts.tier1);
        CHECK_FALSE(verdicts.tier2);
        CHECK_FALSE(verdicts.process_read);
        CHECK(verdicts.all_agree());

        ResolvedPlatePresets presets;
        std::string          error;
        CHECK_FALSE(bundle.resolve_plate_presets(named, presets, error));
        CHECK(error.find("not an FDM printer") != std::string::npos);
    }

    SECTION("divergence 3: a recorded vendor the resolved preset no longer belongs to")
    {
        // Same shape as divergence 2: the process copies had no vendor check, so a preset name
        // that now resolves to another vendor's machine still produced a process value.
        PlateSlicingContext moved = named;
        moved.printer_vendor_id   = "OTHER";

        const PlateContextVerdicts verdicts = ask_every_surface(bundle, moved);
        CHECK_FALSE(verdicts.tier1);
        CHECK_FALSE(verdicts.tier2);
        CHECK_FALSE(verdicts.process_read);
        CHECK(verdicts.all_agree());

        ResolvedPlatePresets presets;
        std::string          error;
        CHECK_FALSE(bundle.resolve_plate_presets(moved, presets, error));
        CHECK(error.find("recorded vendor 'OTHER'") != std::string::npos);
    }

    SECTION("divergence 4: an unresolved Project row, reached through inheritance")
    {
        deselect_via_project_embedded(bundle.printers, "Embedded Project Printer");

        const PlateContextVerdicts verdicts = ask_every_surface(bundle, PlateSlicingContext());
        CHECK_FALSE(verdicts.tier1);
        CHECK_FALSE(verdicts.tier2);
        CHECK_FALSE(verdicts.process_read);
        CHECK(verdicts.all_agree());

        // and a plate that names its own machine is still resolvable, so this is an unresolved
        // Project row rather than an unresolvable project
        const PlateContextVerdicts named_verdicts = ask_every_surface(bundle, named);
        CHECK(named_verdicts.tier1);
        CHECK(named_verdicts.tier2);
        CHECK(named_verdicts.process_read);
    }
}

TEST_CASE("The narrow plate process read equals the composed value", "[Preset][PlateContext]")
{
    PlateContextFixture       fixture;
    PresetBundle &            bundle = fixture.bundle;
    const PlateSlicingContext named  = fixture.named_context();

    // The plate's own override, which the composition applies last and the narrow read applies
    // first - the same answer reached from opposite ends.
    DynamicPrintConfig plate_overrides;
    plate_overrides.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByLayer));

    SECTION("with no plate override, the value is the plate's process preset, not the Project's")
    {
        const ConfigOption *narrow = bundle.plate_process_option(named, nullptr, "print_sequence");
        REQUIRE(narrow != nullptr);
        CHECK(narrow->getInt() == int(PrintSequence::ByObject));
        // the Project process says ByLayer; reading it here would be the substitution the fork forbids
        CHECK(narrow->getInt() != int(PrintSequence::ByLayer));
        // and it is the option OF that preset, not a copy of it or a value from anywhere else
        CHECK(narrow == fixture.process->config.option("print_sequence"));

        ResolvedPlateSlicingConfig resolved;
        std::string                error;
        REQUIRE(bundle.resolve_plate_slicing_config(named, std::vector<int>{1}, std::vector<int>{0}, resolved, error));
        const ConfigOption *composed = resolved.config.option("print_sequence");
        REQUIRE(composed != nullptr);
        CHECK(narrow->serialize() == composed->serialize());
    }

    SECTION("a plate override wins, and still equals the composed value")
    {
        const ConfigOption *narrow = bundle.plate_process_option(named, &plate_overrides, "print_sequence");
        REQUIRE(narrow != nullptr);
        CHECK(narrow->getInt() == int(PrintSequence::ByLayer));

        ResolvedPlateSlicingConfig resolved;
        std::string                error;
        REQUIRE(bundle.resolve_plate_slicing_config(named, std::vector<int>{1}, std::vector<int>{0}, resolved, error));
        resolved.config.apply(plate_overrides, true); // exactly what resolve_plate_context does last
        const ConfigOption *composed = resolved.config.option("print_sequence");
        REQUIRE(composed != nullptr);
        CHECK(narrow->serialize() == composed->serialize());
    }

    SECTION("an unresolved context yields nothing, never the Project row's value")
    {
        PlateSlicingContext missing = named;
        missing.print_preset_name   = "Ghost Process";
        CHECK(bundle.plate_process_option(missing, nullptr, "print_sequence") == nullptr);

        // ...but a plate override is the plate's own data and survives an unresolved preset,
        // because it needs no preset to be read.
        const ConfigOption *narrow = bundle.plate_process_option(missing, &plate_overrides, "print_sequence");
        REQUIRE(narrow != nullptr);
        CHECK(narrow->getInt() == int(PrintSequence::ByLayer));
    }

    SECTION("an option the process preset does not carry is absent, not zero")
    {
        CHECK(bundle.plate_process_option(named, nullptr, "petkos_not_a_process_option") == nullptr);
    }
}

// ----------------------------------------------------------------------------
// The re-resolution mechanism: PresetBundle::reresolve_plate_context_for_printer
// ----------------------------------------------------------------------------
//
// One missing rule surfaced as three unrelated faults (a half-renamed compatible_printers
// list, a plate left unresolved after assignment, a hard refusal from the model combo).
// These cases pin the rule itself: what is kept, what is switched, what is reported, and
// what is never touched.

TEST_CASE("Reresolving a plate context after a printer change", "[Preset][PlateContext][Reresolve]")
{
    PlateContextFixture fixture;
    PresetBundle &      bundle = fixture.bundle;

    // The plate's process runs only on the plate's original printer. Set before anything
    // else is added to prints: fixture.process points into a deque.
    fixture.process->config.set_key_value("compatible_printers", new ConfigOptionStrings({"Plate Printer"}));

    // A second machine with its own declared default process. After these insertions the
    // fixture's raw pointers are not used again.
    Preset &other_process = add_inmemory_preset(bundle.prints, "Other Process");
    (void) other_process;
    Preset &other_printer = add_inmemory_preset(bundle.printers, "Other Printer");
    other_printer.vendor                   = &bundle.vendors.find("TEST")->second;
    other_printer.printer_technology_ref() = ptFFF;
    other_printer.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
    other_printer.config.set_key_value("printer_model", new ConfigOptionString("Other Model"));
    other_printer.config.set_key_value("default_print_profile", new ConfigOptionString("Other Process"));

    PresetBundle::PlateContextReresolution result;
    std::string                            error;

    SECTION("a process that cannot run switches to the new printer's own default")
    {
        PlateSlicingContext context = fixture.named_context();
        context.printer_preset_name = "Other Printer";
        context.printer_vendor_id.clear(); // assignment clears it; the mechanism re-records it

        REQUIRE(bundle.reresolve_plate_context_for_printer(context, result, error));
        CHECK(error.empty());
        CHECK(result.process_switched());
        CHECK(result.process_from == "Plate Process");
        CHECK(result.process_to == "Other Process");
        // process_to being a CONCRETE name is the whole assertion: inheritance is not a
        // state this architecture has any more
        CHECK(context.print_preset_name == "Other Process");
        // the vendor id is the identity's second half, and it followed the printer
        CHECK(context.printer_vendor_id == "TEST");

        // and the switched context genuinely resolves: this is the whole point
        ResolvedPlatePresets presets;
        std::string          tier1_error;
        CHECK(bundle.resolve_plate_presets(context, presets, tier1_error));
    }

    SECTION("a process that still runs on the new printer is kept exactly as it is")
    {
        // make the plate process claim the new printer too
        Preset *plate_process = bundle.prints.find_preset("Plate Process", false);
        REQUIRE(plate_process != nullptr);
        plate_process->config.option<ConfigOptionStrings>("compatible_printers")->values = {"Plate Printer", "Other Printer"};

        PlateSlicingContext context = fixture.named_context();
        context.printer_preset_name = "Other Printer";

        REQUIRE(bundle.reresolve_plate_context_for_printer(context, result, error));
        CHECK_FALSE(result.process_switched());
        CHECK(result.process_unresolved.empty());
        CHECK(context.print_preset_name == "Plate Process"); // still the thing the user chose
    }

    SECTION("a printer this build does not have preserves the context verbatim")
    {
        PlateSlicingContext context = fixture.named_context();
        context.printer_preset_name = "Missing Printer";
        const PlateSlicingContext before = context;

        CHECK_FALSE(bundle.reresolve_plate_context_for_printer(context, result, error));
        CHECK(error.find("Missing Printer") != std::string::npos);
        CHECK(context == before); // nothing rewritten, nothing cleared, nothing remapped
    }

    SECTION("no usable switch target leaves the slot alone and names the reason")
    {
        Preset *target = bundle.printers.find_preset("Other Printer", false);
        REQUIRE(target != nullptr);
        target->config.option<ConfigOptionString>("default_print_profile", true)->value = "Ghost Process";

        PlateSlicingContext context = fixture.named_context();
        context.printer_preset_name = "Other Printer";

        REQUIRE(bundle.reresolve_plate_context_for_printer(context, result, error));
        CHECK_FALSE(result.process_switched());
        CHECK(result.process_unresolved.find("Ghost Process") != std::string::npos);
        CHECK(context.print_preset_name == "Plate Process"); // unresolved is unresolved, not rewritten
    }

    SECTION("an empty printer name is a refusal, not an inheritance")
    {
        // The project printer was deleted as a state (2026-08-14): a plate names its own
        // printer or it is unresolved. This section used to assert the opposite - that
        // clearing the name meant "follow the project again" - which is the exact belief
        // the architecture removed.
        PlateSlicingContext context = fixture.named_context();
        context.printer_preset_name.clear();
        context.printer_vendor_id.clear();

        REQUIRE_FALSE(bundle.reresolve_plate_context_for_printer(context, result, error));
        CHECK(error == "The plate names no printer");
        CHECK(context.print_preset_name == "Plate Process"); // a refusal repairs nothing and discards nothing
    }

    SECTION("filaments are reported, never rewritten")
    {
        Preset *filament = bundle.filaments.find_preset("Plate Filament", false);
        REQUIRE(filament != nullptr);
        filament->config.set_key_value("compatible_printers", new ConfigOptionStrings({"Plate Printer"}));

        PlateSlicingContext context = fixture.named_context();
        context.printer_preset_name = "Other Printer";

        REQUIRE(bundle.reresolve_plate_context_for_printer(context, result, error));
        REQUIRE(result.incompatible_filaments.size() == 1);
        CHECK(result.incompatible_filaments.front() == "Plate Filament");
        // material choice is user intent: the slot still names what the user chose
        CHECK(context.filament_preset_names == std::vector<std::string>{"Plate Filament"});
    }

    SECTION("translating material keeps its original default colour and chosen finish")
    {
        Preset *source = bundle.filaments.find_preset("Plate Filament", false);
        source->config.set_key_value("compatible_printers", new ConfigOptionStrings{"Plate Printer"});
        source->config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
        source->config.set_key_value("default_filament_colour", new ConfigOptionStrings{"#FF3300"});
        Preset &target = add_inmemory_preset(bundle.filaments, "Other PLA");
        target.is_visible = true;
        target.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Other Printer"});
        target.config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
        target.config.set_key_value("default_filament_colour", new ConfigOptionStrings{"#0000FF"});
        bundle.printers.find_preset("Other Printer", false)->config.set_key_value(
            "default_filament_profile", new ConfigOptionStrings{"Other PLA"});
        PlateSlicingContext context = fixture.named_context();
        context.printer_preset_name = "Other Printer";
        context.filament_finishes = {int(FilamentFinish::ffMetallic)};
        REQUIRE(bundle.reresolve_plate_context_for_printer(context, result, error));
        CHECK(context.filament_preset_names == std::vector<std::string>{"Other PLA"});
        CHECK(context.filament_colours == std::vector<std::string>{"#FF3300"});
        CHECK(context.filament_finishes == std::vector<int>{int(FilamentFinish::ffMetallic)});
    }
}

TEST_CASE("Plate colours and finishes do not inherit unrelated spool pool rows", "[Preset][PlateContext][PlateAppearance]")
{
    PlateContextFixture fixture;
    PresetBundle &bundle = fixture.bundle;
    bundle.filaments.find_preset("Plate Filament", false)->config.set_key_value(
        "default_filament_colour", new ConfigOptionStrings{"#112233"});
    add_inmemory_preset(bundle.filaments, "Second Filament").config.set_key_value(
        "default_filament_colour", new ConfigOptionStrings{"#445566"});
    bundle.project_config.option<ConfigOptionStrings>("filament_colour", true)->values = {"#FF0000", "#00FF00", "#0000FF"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour_type", true)->values = {"2", "2", "2"};
    bundle.project_config.option<ConfigOptionStrings>("filament_multi_colour", true)->values = {"#FF0000;#000000", "#00FF00;#000000"};
    bundle.project_config.option<ConfigOptionEnumsGeneric>("filament_finish", true)->values = {int(FilamentFinish::ffMetallic)};

    PlateSlicingContext context = fixture.named_context();
    const bool two_slots = GENERATE(false, true);
    if (two_slots)
        context.filament_preset_names.push_back("Second Filament");
    const std::vector<std::string> expected = two_slots ? std::vector<std::string>{"#112233", "#445566"}
                                                       : std::vector<std::string>{"#112233"};
    ResolvedPlateSlicingConfig resolved;
    std::string error;
    REQUIRE(bundle.resolve_plate_slicing_config(context, std::nullopt, std::nullopt, resolved, error));
    CHECK(resolved.config.option<ConfigOptionStrings>("filament_colour")->values == expected);
    CHECK(bundle.plate_filament_colours(context) == expected);
    CHECK(resolved.config.option<ConfigOptionStrings>("filament_multi_colour")->values == expected);
    CHECK(resolved.config.option<ConfigOptionStrings>("filament_colour_type")->values == std::vector<std::string>(expected.size(), "1"));
    CHECK(resolved.config.option<ConfigOptionEnumsGeneric>("filament_finish")->values ==
          std::vector<int>(expected.size(), int(FilamentFinish::ffStandard)));
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values.size() == 3);
}

TEST_CASE("Plate appearance survives pool changes and participates in the composition cache", "[Preset][PlateContext][PlateAppearance]")
{
    PlateContextFixture fixture;
    PlateSlicingContext context = fixture.named_context();
    context.filament_colours = {"#ABCDEF"};
    context.filament_colour_types = {"2"};
    context.filament_multi_colours = {"#ABCDEF;#FEDCBA"};
    context.filament_finishes = {int(FilamentFinish::ffSilk)};
    ResolvedPlateSlicingConfig first, second, again;
    std::string error;
    PresetBundle::ComposeScope scope(fixture.bundle);
    REQUIRE(fixture.bundle.resolve_plate_slicing_config(context, std::nullopt, std::nullopt, first, error));
    PlateSlicingContext matte = context;
    matte.filament_finishes = {int(FilamentFinish::ffMatte)};
    REQUIRE(fixture.bundle.resolve_plate_slicing_config(matte, std::nullopt, std::nullopt, second, error));
    REQUIRE(fixture.bundle.resolve_plate_slicing_config(context, std::nullopt, std::nullopt, again, error));
    CHECK(first.config.option<ConfigOptionEnumsGeneric>("filament_finish")->values == context.filament_finishes);
    CHECK(second.config.option<ConfigOptionEnumsGeneric>("filament_finish")->values == matte.filament_finishes);
    CHECK(again.config.option<ConfigOptionEnumsGeneric>("filament_finish")->values == context.filament_finishes);
    CHECK(first.config.option<ConfigOptionStrings>("filament_colour_type")->values == context.filament_colour_types);
    CHECK(first.config.option<ConfigOptionStrings>("filament_multi_colour")->values == context.filament_multi_colours);
    // The CLI uses this same helper after all of its config overlays.
    DynamicPrintConfig cli = first.config;
    cli.option<ConfigOptionStrings>("filament_colour")->values = {"#000000", "#FFFFFF"};
    PresetBundle::apply_plate_filament_colours(context, cli);
    CHECK(cli.option<ConfigOptionStrings>("filament_colour")->values == context.filament_colours);
    CHECK(cli.option<ConfigOptionEnumsGeneric>("filament_finish")->values == context.filament_finishes);
}

TEST_CASE("Legacy plate appearance migrates only from matching declared materials", "[Preset][PlateContext][PlateAppearance]")
{
    PlateSlicingContext seed;
    seed.filament_preset_names = {"PLA", "PETG"};
    DynamicPrintConfig project;
    project.set_key_value("filament_colour", new ConfigOptionStrings{"#112233", "#445566"});
    project.set_key_value("filament_colour_type", new ConfigOptionStrings{"2", "1"});
    project.set_key_value("filament_multi_colour", new ConfigOptionStrings{"#112233;#FF0000", "#445566"});
    project.option<ConfigOptionEnumsGeneric>("filament_finish", true)->values = {int(FilamentFinish::ffMetallic), int(FilamentFinish::ffMatte)};
    PresetBundle::capture_plate_filament_colours(seed, project);

    PlateSlicingContext context;
    context.filament_preset_names = {"PLA", "ABS"};
    SECTION("absent legacy colours and appearance adopt only the matching slot") {
        PresetBundle::migrate_legacy_plate_filament_colours(context, seed);
        CHECK(context.filament_colours == std::vector<std::string>{"#112233", ""});
        CHECK(context.filament_colour_types == std::vector<std::string>{"2", "1"});
        CHECK(context.filament_multi_colours == std::vector<std::string>{"#112233;#FF0000", ""});
        CHECK(context.filament_finishes == std::vector<int>{int(FilamentFinish::ffMetallic), int(FilamentFinish::ffStandard)});
        const PlateSlicingContext migrated = context;
        seed.filament_colours[0] = "#000000";
        PresetBundle::migrate_legacy_plate_filament_colours(context, seed);
        CHECK(context == migrated);
    }
    SECTION("an existing colour never acquires somebody else's multi-colour stripe") {
        context.filament_colours = {"#FFFFFF", ""};
        PresetBundle::migrate_legacy_plate_filament_colours(context, seed);
        CHECK(context.filament_colours == std::vector<std::string>{"#FFFFFF", ""});
        CHECK(context.filament_colour_types[0] == "1");
        CHECK(context.filament_multi_colours[0].empty());
    }
    SECTION("a recorded blank colour remains a request for the material default") {
        context.filament_colours = {"", ""};
        PresetBundle::migrate_legacy_plate_filament_colours(context, seed);
        CHECK(context.filament_colours == std::vector<std::string>{"", ""});
    }
}

TEST_CASE("Completing legacy material names preserves the plate's AMS colours", "[Preset][PlateContext][PlateAppearance]")
{
    PlateContextFixture fixture;
    PlateSlicingContext seed = fixture.named_context();
    seed.filament_colours = {"#000000"};
    seed.filament_colour_types = {"2"};
    seed.filament_multi_colours = {"#000000;#FF0000"};
    PlateSlicingContext legacy;
    DynamicPrintConfig old_overrides;
    old_overrides.set_key_value("filament_colour", new ConfigOptionStrings{"#FFFFFF"});
    PresetBundle::capture_plate_filament_colours(legacy, old_overrides, true);
    REQUIRE(fixture.bundle.complete_plate_context(legacy, seed));
    CHECK(legacy.filament_preset_names == seed.filament_preset_names);
    CHECK(legacy.filament_colours == std::vector<std::string>{"#FFFFFF"});
    CHECK(legacy.filament_colour_types == std::vector<std::string>{"1"});
    CHECK(legacy.filament_multi_colours == std::vector<std::string>{""});
}

TEST_CASE("Moving between plates matches the complete spool identity and appends only used materials", "[Preset][PlateContext][PlateTransfer]")
{
    PlateContextFixture fixture;
    PlateSlicingContext source = fixture.named_context();
    source.filament_preset_names = {"Plate Filament", "Unused Filament"};
    source.filament_colours = {"#FF0000", "#00FF00"};
    source.filament_colour_types = {"2", "1"};
    source.filament_multi_colours = {"#FF0000 #0000FF", "#00FF00"};
    source.filament_finishes = {int(FilamentFinish::ffSilk), int(FilamentFinish::ffStandard)};
    PlateSlicingContext target = fixture.named_context();
    target.filament_colours = {"#FFFFFF"};
    std::vector<int> mapping;
    std::string error;

    REQUIRE(fixture.bundle.map_transferred_filaments(source, target, {1}, mapping, error));
    CHECK(mapping[1] == 2);
    CHECK(mapping[2] == 0);
    CHECK(target.filament_preset_names == std::vector<std::string>{"Plate Filament", "Plate Filament"});
    CHECK(target.filament_colours == std::vector<std::string>{"#FFFFFF", "#FF0000"});
    CHECK(target.filament_colour_types[1] == source.filament_colour_types[0]);
    CHECK(target.filament_multi_colours[1] == source.filament_multi_colours[0]);
    CHECK(target.filament_finishes[1] == source.filament_finishes[0]);
    const auto once = target;
    REQUIRE(fixture.bundle.map_transferred_filaments(source, target, {1}, mapping, error));
    CHECK(target == once);
    CHECK(mapping[1] == 2);
}

TEST_CASE("Replacing a plate material keeps its other slots and appearance", "[Preset][PlateContext][PlateMaterial]")
{
    PlateContextFixture fixture;
    auto &replacement = add_inmemory_preset(fixture.bundle.filaments, "Replacement PLA");
    replacement.config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    auto context = fixture.named_context();
    context.filament_preset_names.push_back("Plate Filament");
    context.filament_colours = {"#112233", "#445566"};
    context.filament_colour_types = {"2", "1"};
    context.filament_multi_colours = {"#112233;#FF0000", ""};
    context.filament_finishes = {int(FilamentFinish::ffSilk), int(FilamentFinish::ffStandard)};
    const auto before = context;
    std::string error;
    REQUIRE(fixture.bundle.assign_plate_material(context, 0, "Replacement PLA", error));
    auto expected = before;
    expected.filament_preset_names[0] = "Replacement PLA";
    CHECK(context == expected);
    CHECK(error.empty());
    CHECK_FALSE(fixture.bundle.assign_plate_material(context, 4, "Replacement PLA", error));
    CHECK(context == expected);
    CHECK_FALSE(fixture.bundle.assign_plate_material(context, 0, "Absent material", error));
    CHECK(context == expected);
}

TEST_CASE("Material replacement translates for the plate printer without substituting another material", "[Preset][PlateContext][PlateMaterial]")
{
    PlateContextFixture fixture;
    auto &source = add_inmemory_preset(fixture.bundle.filaments, "Imported PLA");
    source.config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    source.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Foreign Printer"});
    auto &target = add_inmemory_preset(fixture.bundle.filaments, "Local PLA");
    target.is_visible = true;
    target.config.set_key_value("filament_type", new ConfigOptionStrings{"PLA"});
    target.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Plate Printer"});
    // Exclude the fixture's other stock filament from the candidate set.
    fixture.bundle.filaments.find_preset("Plate Filament", false, true)->config.set_key_value(
        "filament_type", new ConfigOptionStrings{"PETG"});
    auto context = fixture.named_context();
    std::string error;
    REQUIRE(fixture.bundle.assign_plate_material(context, 0, "Imported PLA", error));
    CHECK(context.filament_preset_names[0] == "Local PLA");
    auto &unsupported = add_inmemory_preset(fixture.bundle.filaments, "Imported PEEK");
    unsupported.config.set_key_value("filament_type", new ConfigOptionStrings{"PEEK"});
    unsupported.config.set_key_value("compatible_printers", new ConfigOptionStrings{"Foreign Printer"});
    const auto before = context;
    CHECK_FALSE(fixture.bundle.assign_plate_material(context, 0, "Imported PEEK", error));
    CHECK(context == before);
    CHECK_FALSE(error.empty());
}

TEST_CASE("A failed material transfer leaves the destination unchanged", "[Preset][PlateContext][PlateTransfer]")
{
    PlateContextFixture fixture;
    PlateSlicingContext source = fixture.named_context();
    source.filament_colours = {"#FF0000"};
    PlateSlicingContext target = fixture.named_context();
    std::vector<int> mapping;
    std::string error;
    SECTION("a missing source slot cannot be guessed from the target") {
        const auto before = target;
        CHECK_FALSE(fixture.bundle.map_transferred_filaments(source, target, {1, 2}, mapping, error));
        CHECK(target == before);
        CHECK_FALSE(error.empty());
    }
    SECTION("a full destination cannot overwrite an existing spool") {
        target.filament_preset_names.assign(MAXIMUM_EXTRUDER_NUMBER, "Plate Filament");
        target.filament_colours.assign(MAXIMUM_EXTRUDER_NUMBER, "#FFFFFF");
        const auto before = target;
        CHECK_FALSE(fixture.bundle.map_transferred_filaments(source, target, {1}, mapping, error));
        CHECK(target == before);
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("Complete embedded presets load without reconstructing an unavailable parent", "[Preset][EmbeddedPreset][PlateContext]")
{
    const auto type = GENERATE(Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT);
    const std::string parent_case = GENERATE("no parent", "missing parent", "installed parent");
    const bool complete = GENERATE(true, false);
    CAPTURE(type, parent_case, complete);
    PresetBundle bundle;
    PresetCollection &collection = type == Preset::TYPE_PRINTER ? static_cast<PresetCollection &>(bundle.printers) :
                                   type == Preset::TYPE_PRINT ? bundle.prints : bundle.filaments;
    const std::string name = "Embedded exact profile";
    const std::string parent_name = parent_case == "no parent" ? "" : "Embedded parent";
    Preset source(type, name);
    source.is_project_embedded = true;
    source.is_external = true;
    source.config = collection.default_preset().config;
    source.config.option<ConfigOptionString>("inherits", true)->value = parent_name;
    std::string omitted_key;
    if (type == Preset::TYPE_PRINTER) {
        source.config.option<ConfigOptionString>("printer_settings_id", true)->value = name;
        source.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.6}));
        source.config.set_key_value("printable_height", new ConfigOptionFloat(310.));
        source.config.set_key_value("machine_start_gcode", new ConfigOptionString("G28\nM104 S221"));
        omitted_key = "printable_height";
    } else if (type == Preset::TYPE_PRINT) {
        source.config.option<ConfigOptionString>("print_settings_id", true)->value = name;
        source.config.set_key_value("layer_height", new ConfigOptionFloat(0.28));
        source.config.set_key_value("wall_loops", new ConfigOptionInt(4));
        omitted_key = "wall_loops";
    } else {
        source.config.option<ConfigOptionStrings>("filament_settings_id", true)->values = {name};
        source.config.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
        source.config.set_key_value("nozzle_temperature", new ConfigOptionInts({214}));
        source.config.set_key_value("filament_flow_ratio", new ConfigOptionFloats({0.97}));
        omitted_key = "filament_flow_ratio";
    }
    DynamicPrintConfig expected = source.config;
    if (!complete) {
        source.config.erase(omitted_key);
        expected.set_key_value(omitted_key, collection.default_preset().config.option(omitted_key)->clone());
    }
    if (parent_case == "installed parent")
        add_inmemory_preset(collection, parent_name);
    std::vector<Preset *> imported{&source};
    REQUIRE_NOTHROW(bundle.load_project_embedded_presets(imported, ForwardCompatibilitySubstitutionRule::Disable));
    const Preset *loaded = collection.find_preset(name, false);
    if (!complete && parent_case != "installed parent") {
        CHECK(loaded == nullptr);
        return;
    }
    REQUIRE(loaded != nullptr);
    CHECK(loaded->is_project_embedded);
    CHECK(loaded->is_external);
    for (const std::string &key : expected.keys()) {
        INFO("setting " << key);
        const ConfigOption *actual = loaded->config.option(key);
        REQUIRE(actual != nullptr);
        CHECK(actual->serialize() == expected.option(key)->serialize());
    }
}

TEST_CASE("Embedded printer vendor identity survives loading without an installed vendor", "[Preset][EmbeddedPreset][PlateContext][EmbeddedPrinterVendor]")
{
    const std::string source_case = GENERATE("explicit vendor", "old full config", "installed parent", "direct vendor", "installed name collision");
    CAPTURE(source_case);
    PresetBundle bundle;
    const std::string printer_name = "Exact printer(project.3mf)";
    const std::string process_name = "Exact process(project.3mf)";
    const std::string archive_vendor = "ARCHIVE_VENDOR";
    const std::string installed_vendor = "INSTALLED_VENDOR";
    const bool has_installed_vendor = source_case == "installed parent" || source_case == "direct vendor" || source_case == "installed name collision";
    bundle.vendors.emplace(installed_vendor, VendorProfile(installed_vendor));
    bundle.vendors.emplace("SELECTED_VENDOR", VendorProfile("SELECTED_VENDOR"));
    add_inmemory_preset(bundle.printers, "Unrelated selected printer").vendor = &bundle.vendors.at("SELECTED_VENDOR");
    bundle.printers.select_preset_by_name("Unrelated selected printer", true);
    add_inmemory_preset(bundle.prints, process_name);

    Preset source(Preset::TYPE_PRINTER, printer_name);
    source.is_project_embedded = true;
    source.is_external = true;
    source.config = bundle.printers.default_preset().config;
    source.config.set_key_value("printer_settings_id", new ConfigOptionString(printer_name));
    source.config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.6}));
    source.config.set_key_value("machine_start_gcode", new ConfigOptionString("G28\nM104 S221"));
    source.config.option<ConfigOptionString>("inherits", true)->value.clear();
    if (source_case == "old full config")
        source.config.erase("printer_vendor_id");
    else
        source.config.set_key_value("printer_vendor_id", new ConfigOptionString(archive_vendor));
    if (source_case == "installed parent") {
        add_inmemory_preset(bundle.printers, "Installed parent").vendor = &bundle.vendors.at(installed_vendor);
        source.inherits() = "Installed parent";
    } else if (source_case == "direct vendor") {
        source.vendor = &bundle.vendors.at(installed_vendor);
    } else if (source_case == "installed name collision") {
        add_inmemory_preset(bundle.printers, printer_name).vendor = &bundle.vendors.at(installed_vendor);
    }
    const DynamicPrintConfig source_config = source.config;
    std::vector<Preset *> imported{&source};
    REQUIRE_NOTHROW(bundle.load_project_embedded_presets(imported, ForwardCompatibilitySubstitutionRule::Disable));
    const Preset *loaded = bundle.printers.find_preset(printer_name, false);
    REQUIRE(loaded != nullptr);
    if (source_case != "installed name collision") {
        CHECK(loaded->is_project_embedded);
        for (const std::string &key : source_config.keys()) {
            INFO("preserved machine setting " << key);
            REQUIRE(loaded->config.option(key) != nullptr);
            CHECK(loaded->config.option(key)->serialize() == source_config.option(key)->serialize());
        }
    }

    PlateSlicingContext context;
    context.printer_preset_name = printer_name;
    context.printer_vendor_id = archive_vendor;
    context.print_preset_name = process_name;
    const PlateSlicingContext before = context;
    ResolvedPlatePresets resolved;
    std::string error;
    if (source_case == "explicit vendor") {
        REQUIRE(bundle.resolve_plate_presets(context, resolved, error));
        CHECK(resolved.printer_vendor_id == archive_vendor);
    } else {
        CHECK_FALSE(bundle.resolve_plate_presets(context, resolved, error));
        CHECK_FALSE(error.empty());
    }
    CHECK(context == before);
    if (has_installed_vendor) {
        context.printer_vendor_id = installed_vendor;
        REQUIRE(bundle.resolve_plate_presets(context, resolved, error));
        CHECK(resolved.printer_vendor_id == installed_vendor);
    }
    if (source_case == "explicit vendor") {
        context.printer_vendor_id.clear();
        PresetBundle::PlateContextReresolution result;
        REQUIRE(bundle.reresolve_plate_context_for_printer(context, result, error));
        CHECK(context.printer_vendor_id == archive_vendor);
    }
}

TEST_CASE("Loading embedded presets preserves the current edited selection", "[Preset][EmbeddedPreset][PlateContext][EmbeddedPresetSelection]")
{
    const auto type = GENERATE(Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT);
    const std::string selection = GENERATE("custom", "default", "unresolved");
    CAPTURE(type, selection);
    PresetBundle bundle;
    PresetCollection &collection = type == Preset::TYPE_PRINTER ? static_cast<PresetCollection &>(bundle.printers) :
                                   type == Preset::TYPE_PRINT ? bundle.prints : bundle.filaments;
    const std::string selected_name = selection == "default" ? collection.default_preset().name :
                                     type == Preset::TYPE_FILAMENT ? "AAA selected profile" : "ZZZ selected profile";
    const std::string imported_name = type == Preset::TYPE_FILAMENT ? "Generic Z embedded @System" : "AAA embedded profile";
    if (selection == "unresolved") {
        deselect_via_project_embedded(collection, selected_name);
    } else {
        if (selection == "custom")
            add_inmemory_preset(collection, selected_name);
        collection.select_preset_by_name(selected_name, true);
        REQUIRE(collection.get_selected_preset_name() == selected_name);
    }
    const std::string edited_key = type == Preset::TYPE_PRINTER ? "nozzle_diameter" :
                                   type == Preset::TYPE_PRINT ? "layer_height" : "nozzle_temperature";
    if (type == Preset::TYPE_PRINTER)
        collection.get_edited_preset().config.set_key_value(edited_key, new ConfigOptionFloats({0.8}));
    else if (type == Preset::TYPE_PRINT)
        collection.get_edited_preset().config.set_key_value(edited_key, new ConfigOptionFloat(0.31));
    else
        collection.get_edited_preset().config.set_key_value(edited_key, new ConfigOptionInts({207}));
    collection.get_edited_preset().set_dirty(true);
    const Preset edited_before = collection.get_edited_preset();

    Preset source(type, imported_name);
    source.config = collection.default_preset().config;
    source.config.option<ConfigOptionString>("inherits", true)->value.clear();
    source.is_project_embedded = true;
    source.is_external = true;
    std::vector<Preset *> imported{&source};
    REQUIRE_NOTHROW(bundle.load_project_embedded_presets(imported, ForwardCompatibilitySubstitutionRule::Disable));
    const Preset *stored = collection.find_preset(imported_name, false, true);
    REQUIRE(stored != nullptr);
    REQUIRE(stored->is_project_embedded);
    const Preset *effective = collection.find_preset(imported_name, false);
    REQUIRE(effective == stored);
    CHECK(effective->name == imported_name);
    CHECK(collection.get_selected_preset_name() == (selection == "unresolved" ? std::string() : selected_name));
    CHECK(collection.get_edited_preset().name == edited_before.name);
    CHECK(collection.get_edited_preset().is_dirty);
    CHECK(collection.get_edited_preset().config.option(edited_key)->serialize() == edited_before.config.option(edited_key)->serialize());
    if (selection != "unresolved") {
        REQUIRE(collection.find_preset(selected_name, false) == &collection.get_edited_preset());
        CHECK(collection.get_selected_preset().name == selected_name);
        if (selection == "default")
            CHECK(collection.get_selected_idx() == 0);
    } else {
        CHECK(collection.get_selected_idx() == size_t(-1));
    }
}
