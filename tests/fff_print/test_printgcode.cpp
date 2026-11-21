#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Preset.hpp"

#include "test_data.hpp"

#include <algorithm>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

void check_extrusion_widths(const std::string &gcode, bool with_support, const std::string &initial_width)
{
    for (const char *setting : {"outer_wall_line_width", "inner_wall_line_width", "sparse_infill_line_width",
                                "internal_solid_infill_line_width", "top_surface_line_width"}) {
        CAPTURE(setting);
        CHECK(gcode.find(std::string("; ") + setting + " = ") != std::string::npos);
    }
    CHECK(gcode.find("; initial_layer_line_width = " + initial_width + "\n") != std::string::npos);
    CHECK(gcode.find("; LINE_WIDTH: ") != std::string::npos);
    CHECK((!extrusion_heights(gcode, "Support").empty() || !extrusion_heights(gcode, "Support interface").empty()) == with_support);
}

void configure_two_filaments(DynamicPrintConfig &config)
{
    config.set_num_filaments(2);
    for (const auto &key : Preset::filament_options()) {
        auto *option = dynamic_cast<ConfigOptionVectorBase *>(config.option(key));
        if (option && option->size() == 1)
            option->resize(2);
    }
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(true));
    config.set_key_value("filament_diameter", new ConfigOptionFloats{1.75, 1.75});
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FF0000", "#00FF00"});
    config.set_key_value("filament_self_index", new ConfigOptionInts{1, 2});
    config.set_key_value("filament_map", new ConfigOptionInts{1, 1});
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats{0., 280., 280., 0.});
}

struct SpiralRaftGCodeResult
{
    double first_model_extrusion_z { -1. };
    double highest_z_before_model  { 0. };
    double nominal_model_layer_z   { 0. };
    size_t layer_z_restores        { 0 };
    bool   model_z_is_continuous   { true };
};

SpiralRaftGCodeResult spiral_raft_gcode_result(const std::string &change_filament_gcode)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    configure_two_filaments(config);
    config.set_deserialize_strict({
        { "spiral_mode", true },
        { "wall_loops", 1 },
        { "top_shell_layers", 0 },
        { "bottom_shell_layers", 1 },
        { "sparse_infill_density", "0%" },
        { "raft_layers", 2 },
        { "support_filament", 2 },
        { "support_interface_filament", 2 },
        { "wall_filament", 1 },
        { "sparse_infill_filament", 1 },
        { "solid_infill_filament", 1 },
        { "retraction_length", 0 },
        { "skirt_loops", 0 },
        { "gcode_comments", true },
        { "machine_start_gcode", "T[initial_extruder]\n" },
        { "change_filament_gcode", change_filament_gcode }
    });

    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({ TestMesh::cube_20x20x20 }, print, model, config);
    std::string gcode = Slic3r::Test::gcode(print);

    SpiralRaftGCodeResult result;
    result.nominal_model_layer_z = print.objects().front()->layers().front()->print_z;
    int                   current_tool = -1;
    bool                  raft_tool_seen = false;
    double                last_model_extrusion_z = -1.;
    GCodeReader           reader;
    reader.apply_config(config);
    reader.parse_buffer(gcode, [&result, &current_tool, &raft_tool_seen, &last_model_extrusion_z](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        if (line.cmd().size() > 1 && line.cmd().front() == 'T') {
            current_tool = atoi(line.cmd().data() + 1);
            raft_tool_seen |= current_tool == 1;
        }

        if (result.first_model_extrusion_z < 0.) {
            result.highest_z_before_model = std::max(result.highest_z_before_model, static_cast<double>(self.z()));
            if (raft_tool_seen && current_tool == 0 && line.travel() && line.has_z() &&
                self.z() > result.nominal_model_layer_z + EPSILON &&
                std::abs(line.z() - result.nominal_model_layer_z) < EPSILON)
                ++result.layer_z_restores;
            if (raft_tool_seen && current_tool == 0 && line.extruding(self) && line.dist_XY(self) > 0.)
                result.first_model_extrusion_z = self.z();
        }

        if (raft_tool_seen && current_tool == 0 && line.extruding(self) && line.dist_XY(self) > 0.) {
            if (last_model_extrusion_z >= 0.) {
                double z_step = self.z() - last_model_extrusion_z;
                result.model_z_is_continuous &= z_step >= -EPSILON && z_step <= 0.21;
            }
            last_model_extrusion_z = self.z();
        }
    });
    return result;
}

} // namespace

SCENARIO( "PrintGCode basic functionality", "[PrintGCode]") {
    GIVEN("A default configuration and a print test object") {
        WHEN("the output is executed with no support material") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({TestMesh::cube_20x20x20}, print, model, {
                { "layer_height", 0.2 },
                { "initial_layer_print_height", 0.2 },
                { "initial_layer_line_width", 0 },
                { "gcode_comments", true },
                { "skirt_loops", 1 },
                { "machine_start_gcode", "" }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            {
                INFO("Some text output is generated.");
                CHECK(gcode.size() > 0);
            }
            {
                INFO("Exported text contains slic3r version");
                CHECK(gcode.find(SLIC3R_VERSION) != std::string::npos);
            }
            {
                INFO("Exported widths match the configured first layer and generated features");
                check_extrusion_widths(gcode, false, "0");
            }
            {
                INFO("Exported text does not contain cooling markers (they were consumed)");
                CHECK(gcode.find(";_EXTRUDE_SET_SPEED") == std::string::npos);
            }

            {
                INFO("GCode preamble is emitted.");
                CHECK(gcode.find("\nG21\n") != std::string::npos);
            }

            {
                INFO("Config options emitted for print config, default region config, default object config");
                CHECK(gcode.find("; nozzle_temperature_initial_layer = ") != std::string::npos);
                CHECK(gcode.find("; layer_height") != std::string::npos);
                CHECK(gcode.find("; sparse_infill_density = ") != std::string::npos);
            }
            {
                INFO("Infill is emitted.");
                CHECK(!extrusion_heights(gcode, "Sparse infill").empty());
            }
            {
                INFO("Perimeters are emitted.");
                CHECK(!extrusion_heights(gcode, "Outer wall").empty());
            }
            {
                INFO("Skirt is emitted.");
                CHECK(!extrusion_heights(gcode, "Skirt").empty());
            }
            {
                INFO("final Z height is 20mm");
                double final_z = 0.0;
                GCodeReader reader;
                reader.apply_config(print.config());
                reader.parse_buffer(gcode, [&final_z] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
                    final_z = std::max<double>(final_z, static_cast<double>(self.z())); // record the highest Z point we reach
                });
                CHECK(final_z == Approx(20.));
            }
        }
        WHEN("output is executed with complete objects and two differently-sized meshes") {
            Slic3r::Print print;
            Slic3r::Model model;
            Slic3r::Test::init_print({Slic3r::make_cube(20, 20, 10), Slic3r::make_cube(20, 20, 20)}, print, model, {
                { "initial_layer_line_width", 0 },
                { "initial_layer_print_height", 0.3 },
                { "layer_height", 0.2 },
                { "enable_support", false },
                { "raft_layers", 0 },
                { "print_sequence", "by object" },
                { "gcode_comments", true },
                { "skirt_loops", 1 },
                { "printing_by_object_gcode", "; between-object-gcode" }
                });
            std::string gcode = Slic3r::Test::gcode(print);
            {
                INFO("Some text output is generated.");
                CHECK(gcode.size() > 0);
            }
            {
                INFO("Infill is emitted.");
                CHECK(!extrusion_heights(gcode, "Sparse infill").empty());
            }
            {
                INFO("Perimeters are emitted.");
                CHECK(!extrusion_heights(gcode, "Outer wall").empty());
            }
            {
                INFO("Skirt is emitted.");
                CHECK(!extrusion_heights(gcode, "Skirt").empty());
            }
            const std::string between_objects = "\n; between-object-gcode\n";
            const size_t boundary = gcode.find(between_objects);
            REQUIRE(boundary != std::string::npos);
            const auto first_walls = extrusion_heights(gcode.substr(0, boundary), "Outer wall");
            const auto second_walls = extrusion_heights(gcode.substr(boundary + between_objects.size()), "Outer wall");
            REQUIRE_FALSE(first_walls.empty());
            REQUIRE_FALSE(second_walls.empty());
            CHECK(*first_walls.begin() == Approx(0.3));
            CHECK(*first_walls.rbegin() == Approx(10.1));
            CHECK(*second_walls.begin() == Approx(0.3));
            CHECK(*second_walls.rbegin() == Approx(20.1));
        }
        WHEN("the output is executed with support material") {
            std::string gcode = ::Test::slice({TestMesh::cube_20x20x20}, {
                { "initial_layer_line_width", 0 },
                { "enable_support", true },
                { "raft_layers", 3 },
                { "gcode_comments", true }
                });
            {
                INFO("Some text output is generated.");
                CHECK(gcode.size() > 0);
            }
            {
                INFO("Exported widths match the configured first layer and generated features");
                check_extrusion_widths(gcode, true, "0");
            }
            {
                INFO("Raft is emitted.");
                CHECK(!extrusion_heights(gcode, "Support interface").empty());
            }
        }
        WHEN("the output is executed with a separate first layer extrusion width") {
            std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "initial_layer_line_width", "0.5" }
                });
            {
                INFO("Some text output is generated.");
                CHECK(gcode.size() > 0);
            }
            {
                INFO("Exported widths match the configured first layer and generated features");
                check_extrusion_widths(gcode, false, "0.5");
            }
        }
        WHEN("Cooling is enabled and the fan is disabled.") {
            std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "cooling", true },
                { "close_fan_the_first_x_layers", 5 }
                });
            {
                INFO("GCode to disable fan is emitted.");
                CHECK(gcode.find("\nM106 S0") != std::string::npos);
            }
        }
        WHEN("end_gcode exists with layer_num and layer_z") {
            std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                { "machine_end_gcode", "; Layer_num [layer_num]\n; Layer_z [layer_z]" },
                { "layer_height", 0.1 },
                { "initial_layer_print_height", 0.1 }
                });
            {
                INFO("layer_num and layer_z are processed in the end gcode");
                CHECK(gcode.find("; Layer_num 199") != std::string::npos);
                CHECK(gcode.find("; Layer_z 20") != std::string::npos);
            }
        }
        WHEN("current_extruder exists in start_gcode") {
            {
                std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20 }, {
                    { "machine_start_gcode", "; Extruder [current_extruder]" }
                });
                {
                    INFO("current_extruder is processed in the start gcode and set for first extruder");
                    CHECK(gcode.find("; Extruder 0") != std::string::npos);
                }
            }
            {
                DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
                configure_two_filaments(config);
                config.set_deserialize_strict({
                    { "machine_start_gcode", "; Extruder [current_extruder]" },
                    { "sparse_infill_filament", 2 },
                    { "solid_infill_filament", 2 },
                    { "wall_filament", 2 },
                    { "support_filament", 2 },
                    { "support_interface_filament", 2 }
                });
                std::string gcode = Slic3r::Test::slice({TestMesh::cube_20x20x20}, config);
                {
                    INFO("current_extruder is processed in the start gcode and set for second extruder");
                    CHECK(gcode.find("; Extruder 1") != std::string::npos);
                }
            }
        }

        WHEN("layer_num represents the layer's index from z=0") {
            std::string gcode = ::Test::slice({ TestMesh::cube_20x20x20, TestMesh::cube_20x20x20 }, {
                { "print_sequence", "by object" },
                { "gcode_comments", true },
                { "layer_change_gcode", ";Layer:[layer_num] ([layer_z] mm)" },
                { "layer_height", 0.1 },
                { "initial_layer_print_height", 0.1 }
                });
            // End of the 1st object.
            std::string token = ";Layer:199 ";
            size_t pos = gcode.find(token);
            {
                INFO("First and second object last layer is emitted");
                // First object
                REQUIRE(pos != std::string::npos);
                pos += token.size();
                REQUIRE(pos < gcode.size());
                double z = 0;
                REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
                REQUIRE(z == Approx(20.));
                // Second object
                pos = gcode.find(";Layer:399 ", pos);
                REQUIRE(pos != std::string::npos);
                pos += token.size();
                REQUIRE(pos < gcode.size());
                REQUIRE((sscanf(gcode.data() + pos, "(%lf mm)", &z) == 1));
                REQUIRE(z == Approx(20.));
            }
        }
    }
}

TEST_CASE("Cooling strips its speed markers and preserves user comments", "[PrintGCode][Cooling]")
{
    const std::string layer_gcode =
        "; Slow Down Start\n"
        "G1 F1711; user note;_EXTRUDE_SET_SPEED; after\n"
        "G1 F1712;_EXTRUDE_SET_SPEED_SUFFIX\n"
        "G1 F1713; text mentions ;_EXTRUDE_SET_SPEED in a phrase\n"
        "G1 F1714;_EXTERNAL_PERIMETER;_EXTRUDE_SET_SPEED; after\n"
        "G1 F1715;_EXTRUDE_SET_SPEED;_EXTERNAL_PERIMETER; after\n"
        "G1 F1716; repeated;_EXTRUDE_SET_SPEED;_EXTRUDE_SET_SPEED\n"
        "; Slow Down End\n";
    const auto gcode = Slic3r::Test::slice({Slic3r::make_cube(5, 5, 1)}, {{"layer_change_gcode", layer_gcode}});
    for (const char *expected : {"G1 F1711; user note; after", "G1 F1712;_EXTRUDE_SET_SPEED_SUFFIX",
                                 "G1 F1713; text mentions ;_EXTRUDE_SET_SPEED in a phrase",
                                 "G1 F1714; after", "G1 F1715; after", "G1 F1716; repeated"}) {
        CAPTURE(expected);
        CHECK(gcode.find(std::string("\n") + expected + "\n") != std::string::npos);
    }
}

TEST_CASE("Spiral vase restores object layer Z after a raft tool change", "[PrintGCode][SpiralVase]")
{
    SECTION("tool change ends at an elevated clearance Z") {
        SpiralRaftGCodeResult result = spiral_raft_gcode_result("G1 X5 F12000\nG1 Z{max_layer_z + 3.0} F1200\nT[next_extruder]\n");

        REQUIRE(result.first_model_extrusion_z == Approx(result.nominal_model_layer_z));
        REQUIRE(result.highest_z_before_model >= result.nominal_model_layer_z + 2.);
        REQUIRE(result.layer_z_restores == 1);
    }

    SECTION("tool change does not alter Z") {
        SpiralRaftGCodeResult result = spiral_raft_gcode_result("T[next_extruder]\n");

        REQUIRE(result.first_model_extrusion_z == Approx(result.nominal_model_layer_z));
        REQUIRE(result.layer_z_restores == 0);
        REQUIRE(result.model_z_is_continuous);
    }

    SECTION("tool change leaves the XY position unknown at clearance Z") {
        SpiralRaftGCodeResult result = spiral_raft_gcode_result("G1 Z{max_layer_z + 3.0} F1200\nT[next_extruder]\n");

        REQUIRE(result.first_model_extrusion_z == Approx(result.nominal_model_layer_z));
        REQUIRE(result.highest_z_before_model >= result.nominal_model_layer_z + 2.);
        REQUIRE(result.layer_z_restores == 1);
    }
}
