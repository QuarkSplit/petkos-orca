#include <catch2/catch_all.hpp>
#include "slic3r/GUI/ProjectLibrary.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <iterator>
#include <boost/nowide/cstdlib.hpp>
#include <png.h>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>

namespace fs = boost::filesystem;
using nlohmann::json;
using namespace Slic3r;

namespace {
struct LibraryFixture {
    fs::path root = fs::temp_directory_path() / fs::unique_path("podslicer-library-%%%%-%%%%");
    LibraryFixture() { fs::create_directories(root / "Props" / "03-prepped"); }
    ~LibraryFixture() { boost::system::error_code ec; fs::remove_all(root, ec); }
    fs::path write(const std::string &name, const std::string &content = "solid test\nendsolid") {
        auto file = root / "Props" / "03-prepped" / name;
        boost::nowide::ofstream out(file.string(), std::ios::binary);
        out << content;
        return file;
    }
    std::string png(size_t dimension = 2) {
        const auto file = root / "fixture.png";
        std::vector<uint8_t> pixels(dimension * dimension * 3, 90);
        pixels[0] = 240;
        REQUIRE(Slic3r::png::write_rgb_to_file(file.string(), dimension, dimension, pixels));
        boost::nowide::ifstream in(file.string(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    fs::path project(const std::string &name, bool sliced = false, const std::string &plate_settings = "",
                     const std::vector<std::pair<std::string, std::string>> &extra = {}) {
        auto file = root / "Props" / "03-prepped" / name;
        mz_zip_archive zip{};
        REQUIRE(open_zip_writer(&zip, file.string()));
        const std::string config = R"({"printer_settings_id":"Printer","filament_settings_id":["PLA","PETG"],"filament_type":["PLA","PETG"],"filament_colour":["#FFFFFF","#000000"]})";
        const std::string plates = plate_settings.empty() ? R"(<config><plate><metadata key="plater_printer_preset" value="Printer &amp; one"/><metadata key="plater_filament_presets" value="PLA"/><metadata key="plater_filament_colours" value="#FF0000"/></plate><plate><metadata key="plater_printer_preset" value="Printer two"/><metadata key="plater_filament_presets" value="PETG"/><metadata key="plater_filament_colours" value="#0000FF"/></plate></config>)" : plate_settings;
        REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/project_settings.config", config.data(), config.size(), MZ_DEFAULT_COMPRESSION));
        REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/model_settings.config", plates.data(), plates.size(), MZ_DEFAULT_COMPRESSION));
        for (const auto &[path, bytes] : extra)
            REQUIRE(mz_zip_writer_add_mem(&zip, path.c_str(), bytes.data(), bytes.size(), MZ_DEFAULT_COMPRESSION));
        if (sliced) {
            const std::string info = R"(<config><plate><metadata key="index" value="1"/><metadata key="prediction" value="3600"/><filament id="1" color="#FF0000" used_g="12.5"/></plate><plate><metadata key="index" value="2"/><filament id="1" color="#FFFFFF" used_g="100"/></plate></config>)";
            REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/slice_info.config", info.data(), info.size(), MZ_DEFAULT_COMPRESSION));
            REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/plate_1.gcode", "G28", 3, MZ_DEFAULT_COMPRESSION));
            REQUIRE(mz_zip_writer_add_mem(&zip, "Metadata/plate_2.gcode", "G28", 3, MZ_DEFAULT_COMPRESSION));
        }
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(close_zip_writer(&zip));
        return file;
    }
};
}

TEST_CASE("Library scans folders without hiding unrelated files and reports unavailable roots", "[project_library]")
{
    LibraryFixture fixture;
    const auto stl = fixture.write("Separate.stl");
    fixture.project("Configured.3mf");
    fixture.write("Broken.3mf", "not a zip");
    fixture.write("._resource.3mf", "not a project");
    const auto missing = fixture.root / "Missing";
    std::atomic<bool> cancel{false};
    const auto result = GUI::ProjectLibrary::scan({fixture.root.string(), (fixture.root / "Props").string(), missing.string()}, "", {}, cancel);
    REQUIRE(result["items"].size() == 3);
    REQUIRE(result["rootStatus"][2]["error"] != "");
    REQUIRE(fs::exists(stl));
    for (const auto &item : result["items"]) {
        CHECK(item["superseded"] == false);
        CHECK(item["group"] == "Props");
        if (item["name"] == "Broken") CHECK(item["state"] == "unreadable");
        if (item["name"] == "Configured") {
            CHECK(item["state"] == "prepped");
            CHECK(item["plateCount"] == 2);
            CHECK(item["colours"] == json::array({"#0000FF", "#FF0000"}));
            REQUIRE(item["filaments"].size() == 2);
            CHECK(item["filaments"][0]["type"] == "PLA");
            CHECK(item["filaments"][1]["type"] == "PETG");
            CHECK(item["machines"][0] == "Printer & one");
        }
    }
    cancel = true;
    CHECK(GUI::ProjectLibrary::scan({fixture.root.string()}, "", {}, cancel)["items"].empty());
}

TEST_CASE("Library refresh forgets moved files and rereads changed metadata", "[project_library]")
{
    LibraryFixture fixture;
    auto file = fixture.project("Project.3mf");
    std::atomic<bool> cancel{false};
    auto first = GUI::ProjectLibrary::scan({fixture.root.string()}, "", {}, cancel);
    REQUIRE(first["items"].size() == 1);
    fixture.write("Project.3mf", "archive replaced by a failed download");
    auto changed = GUI::ProjectLibrary::scan({fixture.root.string()}, "", first, cancel);
    CHECK(changed["items"][0]["state"] == "unreadable");
    const auto renamed = file.parent_path() / "Renamed.3mf";
    fs::rename(file, renamed);
    auto moved = GUI::ProjectLibrary::scan({fixture.root.string()}, "", changed, cancel);
    REQUIRE(moved["items"].size() == 1);
    CHECK(moved["items"][0]["path"] == renamed.string());
}

TEST_CASE("Library keeps measurements only for the corresponding plate colour", "[project_library]")
{
    LibraryFixture fixture;
    fixture.project("Sliced.3mf", true);
    std::atomic<bool> cancel{false};
    const auto result = GUI::ProjectLibrary::scan({fixture.root.string()}, "", {}, cancel);
    REQUIRE(result["items"].size() == 1);
    const auto &item = result["items"][0];
    CHECK(item["state"] == "sliced");
    CHECK(item["hours"] == 1.0);
    CHECK(item["grams"] == 12.5);
    CHECK(item["filaments"][0]["grams"] == 12.5);
    CHECK_FALSE(item["filaments"][1].contains("grams"));
}

TEST_CASE("Library material discovery survives missing colours and project decoration", "[project_library]")
{
    LibraryFixture fixture;
    fixture.project("Materials.3mf", false,
        R"(<config><plate><metadata key="plater_filament_presets" value="PLA(Materials.3mf);PETG"/><metadata key="plater_filament_colours" value="&quot;&quot;"/></plate><plate><metadata key="plater_filament_presets" value="PETG"/><metadata key="plater_filament_colours" value="#00000000"/></plate></config>)");
    std::atomic<bool> cancel{false};
    auto result = GUI::ProjectLibrary::scan({fixture.root.string()}, "", {}, cancel);
    REQUIRE(result["items"].size() == 1);
    const auto &item = result["items"][0];
    CHECK(item["materials"] == json::array({"PETG", "PLA"}));
    CHECK(item["colours"].empty());
    REQUIRE(item["filaments"].size() == 2);
    CHECK(item["filaments"][0]["type"] == "PLA");
    CHECK(item["filaments"][1]["type"] == "PETG");
    CHECK(item["filaments"][0]["colour"] == "");
    CHECK(item["filaments"][1]["colour"] == "");

    SECTION("Refresh invalidates records produced by the previous catalogue schema") {
        result.erase("schemaVersion");
        result["items"][0]["materials"] = json::array();
        const auto refreshed = GUI::ProjectLibrary::scan({fixture.root.string()}, "", result, cancel);
        CHECK(refreshed["items"][0]["materials"] == json::array({"PETG", "PLA"}));
    }
}

TEST_CASE("Library extracts a standard relationship thumbnail and repairs its missing cache", "[project_library]")
{
    LibraryFixture fixture;
    const auto image = fixture.png();
    const auto project = fixture.project("Related.3mf", false, "", {
        {"_rels/.rels", R"(<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="preview" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/thumbnail" Target="/Previews/front%20view.png"/></Relationships>)"},
        {"Previews/front view.png", image}});
    const auto cache = fixture.root / "thumbnails";
    std::atomic<bool> cancel{false};
    auto result = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), {}, cancel);
    REQUIRE(result["items"].size() == 1);
    auto &item = result["items"][0];
    CHECK(item["state"] == "prepped");
    CHECK(item["thumbState"] == "ready");
    REQUIRE(item["views"].size() == 1);
    CHECK(item["views"][0]["label"] == "Model");
    const auto revision = item["revision"];
    const fs::path cached(item["thumbCacheFile"].get<std::string>());
    REQUIRE(fs::exists(cached));
    const auto original_size = fs::file_size(project);
    REQUIRE(fs::remove(cached));
    const auto repaired = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), result, cancel);
    REQUIRE(repaired["items"].size() == 1);
    CHECK(repaired["items"][0]["thumbState"] == "ready");
    CHECK(repaired["items"][0]["revision"] == revision);
    CHECK(fs::exists(cached));
    CHECK(fs::file_size(project) == original_size);
}

TEST_CASE("Broken optional images cannot make a readable library project unreadable", "[project_library]")
{
    LibraryFixture fixture;
    auto broken = fixture.png();
    // Retain a real signature and IHDR, but truncate the compressed pixels.
    broken.resize(40);
    std::vector<std::pair<std::string, std::string>> extra = {
        {"Auxiliaries/.thumbnails/thumbnail_middle.png", broken},
        {"_rels/.rels", "not XML"}};
    bool fallback = false;
    SECTION("A normal metadata thumbnail is used after the malformed image") {
        fallback = true;
        extra.emplace_back("Metadata/thumbnail.png", fixture.png());
    }
    SECTION("A geometry preview is requested when every embedded image is invalid") {}
    fixture.project("BrokenPreview.3mf", false, "", extra);
    const auto cache = fixture.root / "thumbnails";
    std::atomic<bool> cancel{false};
    const auto result = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), {}, cancel);
    REQUIRE(result["items"].size() == 1);
    const auto &item = result["items"][0];
    CHECK(item["state"] == "prepped");
    CHECK(item["thumbState"] == (fallback ? "ready" : "pending"));
    CHECK(item["thumbError"] == "");
    CHECK(item["views"].size() == size_t(fallback));
}

TEST_CASE("Library generated previews follow actual file revisions and recover missing cache files", "[project_library]")
{
    LibraryFixture fixture;
    const auto source = fixture.write("Raw mesh with spaces.stl");
    const auto cache = fixture.root / "thumbnails";
    std::atomic<bool> cancel{false};
    auto scanned = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), {}, cancel);
    REQUIRE(scanned["items"].size() == 1);
    auto item = scanned["items"][0];
    CHECK(item["thumbState"] == "pending");
    const auto missing_renderer = (fixture.root / "renderer-does-not-exist").string();
    const auto unavailable = GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_renderer, cancel);
    CHECK(unavailable["thumbState"] == "unavailable");
    CHECK(unavailable["path"] == source.string());
    CHECK(unavailable["revision"] == item["revision"]);
    CHECK(unavailable["size"] == item["size"]);
    const fs::path cached(unavailable["thumbCacheFile"].get<std::string>());
    const auto image = fixture.png(512);
    {
        boost::nowide::ofstream out(cached.string(), std::ios::binary);
        out.write(image.data(), std::streamsize(image.size()));
    }
    const auto ready = GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_renderer, cancel);
    CHECK(ready["thumbState"] == "ready");
    CHECK(ready["thumbSource"] == "geometry");
    CHECK(ready["thumbError"] == "");
    item.update(ready);
    scanned["items"][0] = item;

    SECTION("Existing generated image is discovered during a metadata refresh") {
        const auto refreshed = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), {}, cancel);
        CHECK(refreshed["items"][0]["thumbState"] == "ready");
        CHECK(refreshed["items"][0]["thumbCacheFile"] == cached.string());
    }
    SECTION("Deleting a cache file queues regeneration even with unchanged source metadata") {
        REQUIRE(fs::remove(cached));
        const auto refreshed = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), scanned, cancel);
        CHECK(refreshed["items"][0]["thumbState"] == "pending");
        CHECK(refreshed["items"][0]["thumb"] == "");
        CHECK(GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_renderer, cancel)["thumbState"] == "unavailable");
    }
    SECTION("An explicit refresh retries a previous unavailable preview") {
        REQUIRE(fs::remove(cached));
        scanned["items"][0].update(unavailable);
        const auto refreshed = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), scanned, cancel);
        CHECK(refreshed["items"][0]["thumbState"] == "pending");
        CHECK(refreshed["items"][0]["thumbError"] == "");
    }
    SECTION("Forcing regeneration bypasses a valid old image") {
        const auto forced = GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_renderer, cancel, true);
        CHECK(forced["thumbState"] == "unavailable");
        CHECK(forced["thumb"] == "");
    }
    SECTION("A corrupt cached image is not retained after a forced regeneration fails") {
        {
            boost::nowide::ofstream out(cached.string(), std::ios::binary | std::ios::trunc);
            out << "corrupt";
        }
        const auto forced = GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_renderer, cancel, true);
        CHECK(forced["thumbState"] == "unavailable");
        CHECK_FALSE(fs::exists(cached));
    }
    SECTION("Changing the source never reuses the previous revision image") {
        fixture.write("Raw mesh with spaces.stl", "solid changed\nendsolid changed");
        const auto changed = GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_renderer, cancel);
        CHECK(changed["thumbState"] == "unavailable");
        CHECK(changed["size"] != item["size"]);
        CHECK(changed["thumbCacheFile"] != ready["thumbCacheFile"]);
    }
    SECTION("Cancellation returns without using the cache or launching a helper") {
        cancel = true;
        const auto cancelled = GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_renderer, cancel);
        CHECK(cancelled["thumbState"] == "unavailable");
        CHECK(cancelled["thumbError"] == "Preview cancelled.");
    }
}


namespace {
std::string library_integration_env(const char *name)
{
    const char *value = boost::nowide::getenv(name);
    return value == nullptr ? std::string() : value;
}

std::string library_integration_bytes(const fs::path &file)
{
    boost::nowide::ifstream input(file.string(), std::ios::binary);
    REQUIRE(input.is_open());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string library_integration_fingerprint(const std::string &bytes)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char byte : bytes) { hash ^= byte; hash *= UINT64_C(1099511628211); }
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}

bool library_integration_png(const fs::path &file)
{
    const auto bytes = library_integration_bytes(file);
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
        png_image_free(&image);
        return false;
    }
    if (image.width != 512 || image.height != 512) {
        png_image_free(&image);
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    const bool decoded = png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) != 0;
    png_image_free(&image);
    if (!decoded) return false;
    size_t visible = 0;
    for (size_t i = 3; i < pixels.size(); i += 4) visible += pixels[i] != 0;
    return visible > 0;
}
}

// Opt in with [.library_renderer] and PODSLICER_THUMBNAIL_EXECUTABLE pointing at
// a built application. Hidden tags keep this process integration out of ordinary runs.
TEST_CASE("Library launches the real thumbnail helper and republishes forced previews", "[.library_renderer]")
{
    const auto executable = library_integration_env("PODSLICER_THUMBNAIL_EXECUTABLE");
    if (executable.empty()) SKIP("Set PODSLICER_THUMBNAIL_EXECUTABLE to a built application");
    REQUIRE(fs::is_regular_file(fs::path(executable)));
    LibraryFixture fixture;
    const auto source = fixture.write("Actual mesh with spaces.stl", R"(solid tetrahedron
facet normal 0 0 -1
outer loop
vertex 0 0 0
vertex 0 8 0
vertex 12 0 0
endloop
endfacet
facet normal 0 -1 0
outer loop
vertex 0 0 0
vertex 12 0 0
vertex 0 0 6
endloop
endfacet
facet normal -1 0 0
outer loop
vertex 0 0 0
vertex 0 0 6
vertex 0 8 0
endloop
endfacet
facet normal 1 1 1
outer loop
vertex 12 0 0
vertex 0 8 0
vertex 0 0 6
endloop
endfacet
endsolid tetrahedron
)");
    const auto original = library_integration_bytes(source);
    const auto source_time = fs::last_write_time(source);
    const auto cache = fixture.root / "preview cache with spaces";
    const auto missing_executable = (fixture.root / "missing-renderer").string();
    std::atomic<bool> cancel{false};
    const auto scanned = GUI::ProjectLibrary::scan({fixture.root.string()}, cache.string(), {}, cancel);
    REQUIRE(scanned["items"].size() == 1);
    const auto item = scanned["items"][0];
    const auto first = GUI::ProjectLibrary::thumbnail(item, cache.string(), executable, cancel);
    INFO(first.dump());
    REQUIRE(first["thumbState"] == "ready");
    CHECK(first["thumbSource"] == "geometry");
    CHECK(first["revision"] == item["revision"]);
    CHECK(first["size"] == item["size"]);
    CHECK(first["thumbError"] == "");
    const fs::path cached(first["thumbCacheFile"].get<std::string>());
    REQUIRE(library_integration_png(cached));
    const auto rendered = library_integration_bytes(cached);
    const auto cached_time = fs::last_write_time(cached);
    const auto hit = GUI::ProjectLibrary::thumbnail(item, cache.string(), missing_executable, cancel);
    INFO(hit.dump());
    REQUIRE(hit["thumbState"] == "ready");
    CHECK(hit["thumbCacheFile"] == first["thumbCacheFile"]);
    CHECK(library_integration_bytes(cached) == rendered);
    CHECK(fs::last_write_time(cached) == cached_time);

    // A valid, different PNG proves force bypasses the old image, even if filesystem
    // timestamp resolution would otherwise conceal a second identical rendering.
    const auto different = fixture.png(512);
    REQUIRE(different != rendered);
    {
        boost::nowide::ofstream output(cached.string(), std::ios::binary | std::ios::trunc);
        output.write(different.data(), std::streamsize(different.size()));
        output.close();
        REQUIRE(bool(output));
    }
    const auto regenerated = GUI::ProjectLibrary::thumbnail(item, cache.string(), executable, cancel, true);
    INFO(regenerated.dump());
    REQUIRE(regenerated["thumbState"] == "ready");
    CHECK(regenerated["thumbCacheFile"] == first["thumbCacheFile"]);
    REQUIRE(library_integration_png(cached));
    CHECK(library_integration_bytes(cached) == rendered);
    CHECK(library_integration_bytes(source) == original);
    CHECK(fs::last_write_time(source) == source_time);
}

// Optional real-file prewarm. All four variables are explicit: executable, input
// directory, persistent cache directory, and report path. No caller directory is
// removed or cleared; only each helper's private scratch directory is disposable.
TEST_CASE("Library renders real projects into an explicitly selected persistent cache", "[.library_real_files]")
{
    const auto executable = library_integration_env("PODSLICER_THUMBNAIL_EXECUTABLE");
    const auto input_directory = library_integration_env("PODSLICER_THUMBNAIL_INPUT_DIR");
    const auto cache_directory = library_integration_env("PODSLICER_THUMBNAIL_CACHE_DIR");
    const auto report_path = library_integration_env("PODSLICER_THUMBNAIL_REPORT");
    if (executable.empty() || input_directory.empty() || cache_directory.empty() || report_path.empty())
        SKIP("Set PODSLICER_THUMBNAIL_EXECUTABLE, INPUT_DIR, CACHE_DIR and REPORT for this optional run");
    REQUIRE(fs::is_regular_file(fs::path(executable)));
    REQUIRE(fs::is_directory(fs::path(input_directory)));
    const fs::path report_file(report_path);
    REQUIRE(fs::is_directory(report_file.parent_path()));
    const auto relative_to_cache = fs::weakly_canonical(report_file).lexically_relative(fs::weakly_canonical(fs::path(cache_directory)));
    REQUIRE((relative_to_cache.empty() || *relative_to_cache.begin() == fs::path("..")));
    // Never overwrite an existing file, including a source accidentally named as the report.
    REQUIRE_FALSE(fs::exists(report_file));
    const auto missing_executable = (report_file.parent_path() / "podslicer-integration-missing-renderer").string();
    REQUIRE_FALSE(fs::exists(fs::path(missing_executable)));
    std::atomic<bool> cancel{false};
    const auto scanned = GUI::ProjectLibrary::scan({input_directory}, cache_directory, {}, cancel);
    REQUIRE_FALSE(scanned["items"].empty());
    REQUIRE(scanned["rootStatus"][0]["error"] == "");
    json report{{"inputDirectory", input_directory}, {"cacheDirectory", cache_directory},
                {"executable", executable}, {"fingerprintAlgorithm", "FNV-1a-64"},
                {"items", json::array()}, {"allReady", true}, {"allSourcesUnchanged", true}};
    for (const auto &item : scanned["items"]) {
        const fs::path source(item["path"].get<std::string>());
        const auto original = library_integration_bytes(source);
        const auto source_time = fs::last_write_time(source);
        const auto started = std::chrono::steady_clock::now();
        const auto preview = GUI::ProjectLibrary::thumbnail(item, cache_directory, executable, cancel);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        json row = preview;
        row["name"] = item["name"];
        row["metadataState"] = item["state"];
        row["scanThumbState"] = item["thumbState"];
        row["elapsedMs"] = elapsed;
        row["sourceFingerprintBefore"] = library_integration_fingerprint(original);
        const auto after = library_integration_bytes(source);
        row["sourceFingerprintAfter"] = library_integration_fingerprint(after);
        const bool unchanged = after == original && fs::last_write_time(source) == source_time;
        row["sourceUnchanged"] = unchanged;
        bool ready = preview.value("thumbState", "") == "ready";
        if (ready) {
            const fs::path cached(preview["thumbCacheFile"].get<std::string>());
            const auto image = library_integration_bytes(cached);
            row["pngFingerprint"] = library_integration_fingerprint(image);
            row["pngBytes"] = image.size();
            const bool valid = library_integration_png(cached);
            row["valid512pxPng"] = valid;
            const auto hit = GUI::ProjectLibrary::thumbnail(item, cache_directory, missing_executable, cancel);
            const bool reusable = hit.value("thumbState", "") == "ready" &&
                hit.value("thumbCacheFile", "") == cached.string() && library_integration_bytes(cached) == image;
            row["cacheOnlyHit"] = reusable;
            ready = valid && reusable;
        }
        report["allReady"] = report["allReady"].get<bool>() && ready;
        report["allSourcesUnchanged"] = report["allSourcesUnchanged"].get<bool>() && unchanged;
        report["items"].push_back(row);
        {
            boost::nowide::ofstream output(report_path, std::ios::binary | std::ios::trunc);
            output << report.dump(2) << '\n';
            output.close();
            REQUIRE(bool(output));
        }
        INFO(row.dump());
        CHECK(ready);
        CHECK(unchanged);
    }
}
