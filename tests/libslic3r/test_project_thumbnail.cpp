#include <catch2/catch_all.hpp>

#include "libslic3r/ProjectThumbnail.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <png.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <STEPControl_Writer.hxx>
#include <algorithm>
#include <iterator>
#include <limits>
#include <set>

using namespace Slic3r;
namespace fs = boost::filesystem;
namespace {
struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() / fs::unique_path("podslicer_thumbnail_%%%%%%%%");
    TemporaryDirectory() { fs::create_directories(path); }
    ~TemporaryDirectory() { boost::system::error_code ec; fs::remove_all(path, ec); }
};

Model cube(double x = 12., double y = 8., double z = 6.)
{
    Model model;
    model.add_object("test", "", TriangleMesh(its_make_cube(x, y, z)))->add_instance();
    return model;
}

std::vector<unsigned char> bytes(const fs::path &path)
{
    boost::nowide::ifstream input(path.string(), std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(input), {});
}

ThumbnailData read_png(const fs::path &path)
{
    const auto encoded = bytes(path);
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    REQUIRE(png_image_begin_read_from_memory(&png, encoded.data(), encoded.size()) != 0);
    ScopeGuard release([&] { png_image_free(&png); });
    REQUIRE(png.width == ProjectThumbnail::image_size);
    REQUIRE(png.height == ProjectThumbnail::image_size);
    png.format = PNG_FORMAT_RGBA;
    ThumbnailData image;
    image.set(png.width, png.height);
    REQUIRE(png_image_finish_read(&png, nullptr, image.pixels.data(), 0, nullptr) != 0);
    return image;
}

size_t visible(const ThumbnailData &image)
{
    size_t count = 0;
    for (size_t i = 3; i < image.pixels.size(); i += 4) count += image.pixels[i] > 0;
    return count;
}

size_t coloured(const ThumbnailData &image, size_t channel)
{
    size_t count = 0;
    for (size_t pixel = 0; pixel < image.pixels.size(); pixel += 4) {
        if (image.pixels[pixel + 3] < 128) continue;
        const auto colour = image.pixels[pixel + channel];
        if (colour > 2 * image.pixels[pixel + (channel + 1) % 3] &&
            colour > 2 * image.pixels[pixel + (channel + 2) % 3]) ++count;
    }
    return count;
}

void add_zip_file(mz_zip_archive &archive, const char *path, const std::string &contents)
{
    REQUIRE(mz_zip_writer_add_mem(&archive, path, contents.data(), contents.size(), MZ_DEFAULT_COMPRESSION) != 0);
}
} // namespace

TEST_CASE("Library thumbnails render distinct shaded geometry", "[ProjectThumbnail]")
{
    const auto box = ProjectThumbnail::render(cube());
    const auto tower = ProjectThumbnail::render(cube(5., 5., 35.));
    REQUIRE(box.is_valid());
    CHECK(box.width == 512);
    CHECK(box.height == 512);
    CHECK(visible(box) > 20'000);
    CHECK(visible(box) < 512 * 512);
    CHECK(box.pixels != tower.pixels);
    CHECK(visible(box) > visible(tower));
    std::set<unsigned char> shades;
    for (size_t pixel = 0; pixel < box.pixels.size(); pixel += 4)
        if (box.pixels[pixel + 3] == 255) shades.insert(box.pixels[pixel]);
    CHECK(shades.size() >= 3);
    CHECK(box.pixels[3] == 0);
}

TEST_CASE("Library thumbnails apply volume and instance transforms", "[ProjectThumbnail]")
{
    Model transformed = cube();
    ModelObject &object = *transformed.objects.front();
    object.volumes.front()->set_offset(Vec3d(4., -3., 2.));
    Geometry::Transformation placement;
    Transform3d matrix = Transform3d::Identity();
    matrix.translate(Vec3d(31., 13., 9.));
    matrix.rotate(Eigen::AngleAxisd(0.5, Vec3d::UnitZ()));
    matrix.scale(Vec3d(-1.3, 0.8, 1.7));
    placement.set_matrix(matrix);
    object.instances.front()->set_transformation(placement);
    object.add_instance()->set_offset(Vec3d(-20., 0., 0.));

    Model baked;
    for (const ModelInstance *instance : object.instances) {
        TriangleMesh mesh = object.volumes.front()->mesh();
        mesh.transform(instance->get_matrix() * object.volumes.front()->get_matrix());
        baked.add_object("baked", "", std::move(mesh))->add_instance();
    }
    const auto actual = ProjectThumbnail::render(transformed);
    const auto expected = ProjectThumbnail::render(baked);
    size_t differing_pixels = 0;
    for (size_t pixel = 0; pixel < actual.pixels.size(); pixel += 4) {
        bool differs = false;
        for (size_t channel = 0; channel < 4; ++channel)
            differs |= std::abs(int(actual.pixels[pixel + channel]) - int(expected.pixels[pixel + channel])) > 1;
        if (differs) ++differing_pixels;
    }
    CHECK(differing_pixels < 100);
    CHECK(visible(actual) > 5'000);
    CHECK(actual.pixels != ProjectThumbnail::render(cube()).pixels);
}

TEST_CASE("Library thumbnail depth hides rear geometry regardless of volume order", "[ProjectThumbnail]")
{
    Model model = cube(12., 12., 12.);
    ModelObject &object = *model.objects.front();
    object.volumes.front()->config.set_key_value("extruder", new ConfigOptionInt(2));
    ModelVolume *front = object.add_volume(TriangleMesh(its_make_cube(12., 12., 12.)));
    front->set_offset(Vec3d(20., 20., 20.));
    front->config.set_key_value("extruder", new ConfigOptionInt(1));
    ProjectThumbnail::Colours colours;
    colours.filaments = {"#FF0000", "#0000FF"};
    const auto first = ProjectThumbnail::render(model, colours);
    CHECK(coloured(first, 0) > 20'000);
    CHECK(coloured(first, 2) == 0);
    std::reverse(object.volumes.begin(), object.volumes.end());
    CHECK(ProjectThumbnail::render(model, colours).pixels == first.pixels);
}

TEST_CASE("Library thumbnails preserve instance palettes and painted material slots", "[ProjectThumbnail]")
{
    Model model = cube();
    auto *object = model.objects.front();
    object->instances.front()->set_offset(Vec3d(-20., 20., 0.));
    object->add_instance()->set_offset(Vec3d(20., -20., 0.));
    ProjectThumbnail::Colours colours;
    colours.filaments = {"#FF0000", "#0000FF"};
    colours.instances[{0, 0}] = {"#00FF00", "#FF0000"};
    auto image = ProjectThumbnail::render(model, colours);
    CHECK(coloured(image, 0) > 1'000);
    CHECK(coloured(image, 1) > 1'000);
    object->volumes.front()->mmu_segmentation_facets.set_data(TriangleSelector::painting_from_facet_states(
        std::vector<EnforcerBlockerType>(12, EnforcerBlockerType::Extruder2)));
    image = ProjectThumbnail::render(model, colours);
    CHECK(coloured(image, 0) > 1'000);
    CHECK(coloured(image, 2) > 1'000);
    CHECK(coloured(image, 1) == 0);
}

TEST_CASE("Library thumbnail generation reads STL OBJ and STEP without changing input", "[ProjectThumbnail]")
{
    const std::string extension = GENERATE("stl", "obj", "step");
    CAPTURE(extension);
    TemporaryDirectory directory;
    const auto input = directory.path / ("source." + extension);
    TriangleMesh mesh(its_make_cube(12., 8., 6.));
    if (extension == "stl") REQUIRE(mesh.write_binary(input.string().c_str()));
    else if (extension == "obj") REQUIRE(store_obj(input.string().c_str(), &mesh));
    else {
        STEPControl_Writer writer;
        REQUIRE(writer.Transfer(BRepPrimAPI_MakeBox(12., 8., 6.).Shape(), STEPControl_AsIs) == IFSelect_RetDone);
        REQUIRE(writer.Write(input.string().c_str()) == IFSelect_RetDone);
    }
    const auto original = bytes(input);
    std::string error;
    const auto output = directory.path / "preview.png";
    const bool generated = ProjectThumbnail::generate(input.string(), output.string(),
        (directory.path / "scratch").string(), error);
    INFO(error);
    REQUIRE(generated);
    CHECK(error.empty());
    CHECK(visible(read_png(output)) > 20'000);
    CHECK(bytes(input) == original);
}

TEST_CASE("Library thumbnail 3MF import keeps plate colours and geometry", "[ProjectThumbnail][3mf]")
{
    TemporaryDirectory directory;
    Model model = cube();
    model.set_backup_path((directory.path / "source-scratch").string());
    ScopeGuard detach([&] { model.set_backup_path("detach"); });
    auto *object = model.objects.front();
    object->instances.front()->set_offset(Vec3d(-20., 20., 0.));
    object->add_instance()->set_offset(Vec3d(20., -20., 0.));
    object->config.set_key_value("extruder", new ConfigOptionInt(2));
    PlateData first, second;
    first.plate_index = 0;
    first.objects_and_instances = {{0, 0}};
    first.slicing_context.filament_colours = {"#FFFFFF", "#FF0000"};
    second.plate_index = 1;
    second.objects_and_instances = {{0, 1}};
    second.slicing_context.filament_colours = {"#FFFFFF", "#00FF00"};
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#FFFFFF", "#0000FF"}));
    StoreParams store;
    store.path = (directory.path / "coloured.3mf").string();
    store.model = &model;
    store.config = &config;
    store.plate_data_list = {&first, &second};
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store));
    const auto original = bytes(store.path);
    const auto output = directory.path / "preview.png";
    std::string error;
    const bool generated = ProjectThumbnail::generate(store.path, output.string(),
        (directory.path / "import-scratch").string(), error);
    INFO(error);
    REQUIRE(generated);
    const auto image = read_png(output);
    CHECK(coloured(image, 0) > 1'000);
    CHECK(coloured(image, 1) > 1'000);
    CHECK(coloured(image, 2) == 0);
    CHECK(bytes(store.path) == original);
}

TEST_CASE("Library thumbnails resolve external 3MF model components", "[ProjectThumbnail][3mf]")
{
    TemporaryDirectory directory;
    const auto input = directory.path / "components.3mf";
    mz_zip_archive archive{};
    REQUIRE(open_zip_writer(&archive, input.string()));
    ScopeGuard close([&] { close_zip_writer(&archive); });
    add_zip_file(archive, "[Content_Types].xml", R"(<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>)");
    add_zip_file(archive, "_rels/.rels", R"(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)");
    add_zip_file(archive, "3D/3dmodel.model", R"(<?xml version="1.0"?><model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" xmlns:m="http://schemas.microsoft.com/3dmanufacturing/material/2015/02"><resources><m:colorgroup id="3"><m:color color="#00FF00"/></m:colorgroup><object id="2" type="model" pid="3" pindex="0"><components><component objectid="1" p:path="/3D/Objects/part.model" transform="1 0 0 0 1 0 0 0 1 20 10 0"/></components></object></resources><build><item objectid="2" transform="1 0 0 0 1 0 0 0 1 0 0 0"/></build></model>)");
    add_zip_file(archive, "3D/_rels/3dmodel.model.rels", R"(<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/Objects/part.model" Id="rel1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>)");
    add_zip_file(archive, "3D/Objects/part.model", R"(<?xml version="1.0"?><model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources><object id="1" type="model"><mesh><vertices><vertex x="0" y="0" z="0"/><vertex x="14" y="0" z="0"/><vertex x="0" y="10" z="0"/><vertex x="0" y="0" z="20"/></vertices><triangles><triangle v1="0" v2="2" v3="1"/><triangle v1="0" v2="1" v3="3"/><triangle v1="0" v2="3" v3="2"/><triangle v1="1" v2="2" v3="3"/></triangles></mesh></object></resources></model>)");
    REQUIRE(mz_zip_writer_finalize_archive(&archive) != 0);
    REQUIRE(close_zip_writer(&archive));
    close.reset();
    std::string error;
    const auto output = directory.path / "preview.png";
    const bool generated = ProjectThumbnail::generate(input.string(), output.string(),
        (directory.path / "scratch").string(), error);
    INFO(error);
    REQUIRE(generated);
    CHECK(coloured(read_png(output), 1) > 5'000);
}

TEST_CASE("Library thumbnails reject invalid input and never overwrite a source", "[ProjectThumbnail]")
{
    TemporaryDirectory directory;
    const auto input = directory.path / "broken.obj";
    { boost::nowide::ofstream out(input.string()); out << "v 0 0 0\nf 1 2 3\n"; }
    const auto original = bytes(input);
    std::string error;
    CHECK_FALSE(ProjectThumbnail::generate(input.string(), input.string(), (directory.path / "scratch").string(), error));
    CHECK_FALSE(error.empty());
    CHECK(bytes(input) == original);
    const auto output = directory.path / "broken.png";
    CHECK_FALSE(ProjectThumbnail::generate(input.string(), output.string(), (directory.path / "scratch").string(), error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(fs::exists(output));
    CHECK_FALSE(ProjectThumbnail::generate((directory.path / "missing.stl").string(), output.string(),
        (directory.path / "scratch").string(), error));
    CHECK_FALSE(error.empty());
    CHECK_THROWS(ProjectThumbnail::render(Model{}));
    Model invalid = cube();
    invalid.objects.front()->volumes.front()->set_offset(Vec3d(std::numeric_limits<double>::quiet_NaN(), 0., 0.));
    CHECK_THROWS(ProjectThumbnail::render(invalid));
}
