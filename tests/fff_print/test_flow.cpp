#include <catch2/catch.hpp>

#include <map>

#include "test_data.hpp" // get access to init_print, etc

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r::Test;
using namespace Slic3r;

SCENARIO("Extrusion width specifics", "[Flow]") {
    GIVEN("A 20mm cube with a skirt, brim, walls and bottom infill") {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "skirt_loops", 1 }, { "brim_type", "outer_only" }, { "brim_width", 3 },
            { "wall_loops", 3 }, { "wall_generator", "classic" }, { "sparse_infill_density", 40 },
            { "initial_layer_print_height", 0.3 }, { "layer_height", 0.2 },
            { "outer_wall_line_width", 0.4 }, { "filament_diameter", 1.75 },
            { "filament_flow_ratio", 1 }, { "initial_layer_flow_ratio", 1 }, { "enable_arc_fitting", false }
        });

        WHEN("First layer line width is set to 0.8mm") {
            config.set("initial_layer_line_width", 0.8);
            const double first_z = config.opt_float("initial_layer_print_height");
            const double next_z = first_z + config.opt_float("layer_height");
            struct Extrusion {
                double filament = 0;
                double travel = 0;
            };
            std::map<std::string, Extrusion> first_layer, next_layer;
            std::string feature;
            const std::string feature_tag = "; FEATURE: ";
            GCodeReader reader;
            reader.apply_config(config);
            reader.parse_buffer(Test::slice({TestMesh::cube_20x20x20}, config),
                [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
                    if (line.raw().compare(0, feature_tag.size(), feature_tag) == 0)
                        feature = line.raw().substr(feature_tag.size());
                    if (!line.extruding(state) || line.dist_XY(state) == 0)
                        return;
                    const double z = line.new_Z(state);
                    if (z != Approx(first_z) && z != Approx(next_z))
                        return;
                    Extrusion &extrusion = (z == Approx(first_z) ? first_layer : next_layer)[feature];
                    extrusion.filament += line.dist_E(state);
                    extrusion.travel += line.dist_XY(state);
                });

            const double filament_area = PI * std::pow(1.75 / 2., 2);
            THEN("First layer width applies to each feature on the first layer") {
                REQUIRE(first_layer.count("Skirt") == 1);
                REQUIRE(first_layer.count("Brim") == 1);
                REQUIRE(first_layer.count("Outer wall") == 1);
                REQUIRE(first_layer.count("Bottom surface") == 1);
                const double expected = Flow(0.8f, first_z, 0.4f).mm3_per_mm();
                for (const auto &[role, extrusion] : first_layer) {
                    CAPTURE(role);
                    // Solid infill adjusts its spacing to fit the island.
                    CHECK(extrusion.filament * filament_area / extrusion.travel == Approx(expected).epsilon(0.05));
                }
            }
            THEN("First layer width does not apply to the outer wall on the next layer") {
                REQUIRE(next_layer.count("Outer wall") == 1);
                const Extrusion &wall = next_layer.at("Outer wall");
                const double expected = Flow(0.4f, next_z - first_z, 0.4f).mm3_per_mm();
                REQUIRE(wall.filament * filament_area / wall.travel == Approx(expected).epsilon(0.001));
            }
        }
    }
}
// needs gcode export
SCENARIO(" Bridge flow specifics.", "[Flow]") {
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
    GIVEN("A default config with no cooling and a fixed bridge speed, flow ratio, fixed extrusion width of 0.4mm and an overhang mesh.") {
        WHEN("bridge_flow_ratio is set to 1.0") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 0.5") {
            THEN("Output flow is as expected.") {
            }
        }
        WHEN("bridge_flow_ratio is set to 2.0") {
            THEN("Output flow is as expected.") {
            }
        }
    }
}

/// Test the expected behavior for auto-width, 
/// spacing, etc
SCENARIO("Flow: Flow math for non-bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
        ConfigOptionFloatOrPercent	width(1.0, false);
        float nozzle_diameter	= 0.4f;
        float layer_height		= 0.4f;

        // Spacing for non-bridges is has some overlap
        THEN("External perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(1.125 * nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }

        THEN("Internal perimeter flow has spacing fixed to 1.125 * nozzle_diameter") {
            auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(1.125 *nozzle_diameter - layer_height * (1.0 - PI / 4.0)));
        }
        THEN("Spacing for supplied width is 0.8927f") {
            auto flow = Flow::new_from_config_width(frExternalPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(width.value - layer_height * (1.0 - PI / 4.0)));
            flow = Flow::new_from_config_width(frPerimeter, width, nozzle_diameter, layer_height);
            REQUIRE(flow.spacing() == Approx(width.value - layer_height * (1.0 - PI / 4.0)));
        }
    }
    /// Check the min/max
    GIVEN("Nozzle Diameter of 0.25") {
        float nozzle_diameter	= 0.25f;
        float layer_height		= 0.5f;
        WHEN("layer height is set to 0.2") {
            layer_height = 0.15f;
            THEN("Max width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Approx(1.125 * nozzle_diameter));
            }
        }
        WHEN("Layer height is set to 0.25") {
            layer_height = 0.25f;
            THEN("Min width is set.") {
                auto flow = Flow::new_from_config_width(frPerimeter, ConfigOptionFloatOrPercent(0, false), nozzle_diameter, layer_height);
                REQUIRE(flow.width() == Approx(1.125 * nozzle_diameter));
            }
        }
    }

#if 0
    /// Check for an edge case in the maths where the spacing could be 0; original
    /// math is 0.99. Slic3r issue #4654
    GIVEN("Input spacing of 0.414159 and a total width of 2") {
        double in_spacing = 0.414159;
        double total_width = 2.0;
        auto flow = Flow::new_from_spacing(1.0, 0.4, 0.3);
        WHEN("solid_spacing() is called") {
            double result = flow.solid_spacing(total_width, in_spacing);
            THEN("Yielded spacing is greater than 0") {
                REQUIRE(result > 0);
            }
        }
    }
#endif    

}

/// Spacing, width calculation for bridge extrusions
SCENARIO("Flow: Flow math for bridges", "[Flow]") {
    GIVEN("Nozzle Diameter of 0.4, a desired width of 1mm and layer height of 0.5") {
		float nozzle_diameter	= 0.4f;
		float bridge_flow		= 1.0f;
        WHEN("Flow role is frExternalPerimeter") {
            auto flow = Flow::bridging_flow(nozzle_diameter * sqrt(bridge_flow), nozzle_diameter);
            THEN("Bridge width is same as nozzle diameter") {
                REQUIRE(flow.width() == Approx(nozzle_diameter));
            }
            THEN("Bridge spacing is same as nozzle diameter + BRIDGE_EXTRA_SPACING") {
                REQUIRE(flow.spacing() == Approx(nozzle_diameter + BRIDGE_EXTRA_SPACING));
            }
        }
    }
}
