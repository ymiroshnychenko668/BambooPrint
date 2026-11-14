#ifndef NATIVE_PREVIEW_FIXTURE_HPP
#define NATIVE_PREVIEW_FIXTURE_HPP

#include "GCodeProcessor.hpp"
#include "NativeProcessorProvenance.hpp"

// Input facts independently paired with native_preview_oracle.py. The producer
// must serialize this same finalized result and collector without reparsing.
namespace NativePreviewFixture {
namespace P = Slic3r::NativeProcessorProvenance;
inline void populate_result(Slic3r::GCodeProcessorResult &result)
{
    result.moves.clear();
    result.moves.resize(5);
    result.spiral_vase_layers.clear();
    auto &travel = result.moves[1];
    travel.type = Slic3r::EMoveType::Travel;
    travel.move_path_type = Slic3r::EMovePathType::Linear_move;
    travel.gcode_id = 4;
    travel.position = Slic3r::Vec3f(1.25f, -0.0f, 0.25f);
    auto &seam = result.moves[2];
    seam.type = Slic3r::EMoveType::Seam;
    seam.extrusion_role = static_cast<Slic3r::ExtrusionRole>(1);
    seam.gcode_id = 4;
    auto &pause = result.moves[3];
    pause.type = Slic3r::EMoveType::Pause_Print;
    pause.extrusion_role = static_cast<Slic3r::ExtrusionRole>(19);
    pause.gcode_id = 5;
    auto &arc = result.moves[4];
    arc.type = Slic3r::EMoveType::Extrude;
    arc.extrusion_role = static_cast<Slic3r::ExtrusionRole>(2);
    arc.move_path_type = Slic3r::EMovePathType::Arc_move_cw;
    arc.gcode_id = 5; arc.extruder_id = 2; arc.cp_color_id = 255;
    arc.object_label_id = 7; arc.is_pa_line_calibration = true;
    arc.delta_extruder = 0.5f; arc.feedrate = 42.5f; arc.width = 0.5f;
    arc.height = 0.25f; arc.mm3_per_mm = 0.125f; arc.fan_speed = 42;
    arc.additional_fan_speed = 25; arc.temperature = 210; arc.layer_duration = 12.5f;
    arc.thermal_index_min = 1; arc.thermal_index_max = 3; arc.thermal_index_mean = 2;
    arc.print_z = 0.25f; arc.time = {7.5f, 9};
    arc.position = Slic3r::Vec3f(3, 2, 0.25f);
    arc.arc_center_position = Slic3r::Vec3f(1, 1, 0.25f);
    arc.interpolation_points = {Slic3r::Vec3f(2.25f, 1.5f, 0.25f), Slic3r::Vec3f(3, 2, 0.25f)};
}
inline void populate_collector(P::Collector &p)
{
    const std::string original = "A\n;P\nB\n";
    p.append_dummy_move(); p.begin_buffer(original);
    p.begin_parser_record("A"); p.append_move(1, P::MoveKind::Observed); p.end_parser_record();
    p.begin_parser_record(";P"); p.append_move(2, P::MoveKind::SyntheticEvent); p.end_parser_record();
    p.begin_parser_record("B"); p.append_move(3, P::MoveKind::SyntheticPrevious, 1);
    p.relocate_move(2, 3); p.append_move(4, P::MoveKind::Observed);
    p.end_parser_record(); p.end_buffer(); p.finish_capture(5);
    p.begin_rewrite(); p.consume(0, original); p.emit_associated(0, 2, "I\n");
    p.emit_copied(0, "A\n"); p.emit_rewritten(2, 3, "P1\nP2\n"); p.emit_copied(5, "B\n");
    p.end_input(); p.finish_rewrite();
    const std::string final_bytes = "I\nA\nP1\nP2\nB\n";
    p.begin_rewrite(); p.consume(0, final_bytes); p.emit_copied(0, final_bytes);
    p.end_input(); p.finish_rewrite(); p.finish(5);
}
} // namespace NativePreviewFixture
#endif
