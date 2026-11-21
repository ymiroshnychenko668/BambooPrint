#include <catch2/catch.hpp>

#include <memory>
#include <optional>

#include "libslic3r/GCode.hpp"
#include "libslic3r/Print.hpp"

using namespace Slic3r;

SCENARIO("Origin manipulation", "[GCode]") {
	Slic3r::GCode gcodegen;
	WHEN("set_origin to (10,0)") {
    	gcodegen.set_origin(Vec2d(10,0));
    	REQUIRE(gcodegen.origin() == Vec2d(10, 0));
    }
	WHEN("set_origin to (10,0) and translate by (5, 5)") {
		gcodegen.set_origin(Vec2d(10,0));
		gcodegen.set_origin(gcodegen.origin() + Vec2d(5, 5));
		THEN("origin returns reference to point") {
    		REQUIRE(gcodegen.origin() == Vec2d(15,5));
    	}
    }
}

TEST_CASE("A standalone print starts at the plate origin", "[GCode]") {
    Print print;
    REQUIRE(print.get_plate_origin() == Vec3d::Zero());
    print.set_plate_origin(Vec3d(200, 100, 0));
    REQUIRE(print.get_plate_origin() == Vec3d(200, 100, 0));
}

TEST_CASE("A fresh writer emits the first bed temperature including zero", "[GCode]") {
    std::optional<GCodeWriter> writer;
    for (int temperature : {0, 35}) {
        for (int job = 0; job < 2; ++job) {
            CAPTURE(temperature, job);
            auto &current = writer.emplace();
            const auto target = std::to_string(temperature);
            REQUIRE(current.set_bed_temperature(temperature, false).find("M140 S" + target + " ") == 0);
            REQUIRE(current.set_bed_temperature(temperature, true).find("M190 S" + target + " ") == 0);
            REQUIRE(current.set_bed_temperature(temperature, true).empty());
        }
    }
}
