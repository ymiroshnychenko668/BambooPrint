#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/NativeEmissionMap.hpp"
#include "libslic3r/GCode/NativeProcessorProvenance.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <openssl/sha.h>
#include <stdexcept>
#include <string>

namespace {
using namespace Slic3r;
using NativeProcessorProvenance::Collector;
using NativeProcessorProvenance::MoveKind;
using NativeProcessorProvenance::Relation;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Scratch {
    std::filesystem::path path;

    Scratch()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "bamboo-rewrites-XXXXXX").string();
        require(::mkdtemp(pattern.data()) != nullptr, "cannot create rewrite fixture directory");
        path = pattern;
    }

    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void write_file(const std::filesystem::path& path, const std::string& bytes)
{
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.write(bytes.data(), bytes.size());
    file.close();
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    require(file.is_open(), "cannot read rewrite fixture");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string sha256(std::string_view input)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    require(::SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest) != nullptr,
            "cannot hash rewrite fixture");
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    for (const unsigned char byte : digest) {
        text += hex[byte >> 4];
        text += hex[byte & 15];
    }
    return text;
}

void set_nozzles(GCodeProcessor& processor)
{
    using MultiNozzleUtils::LayeredNozzleGroupResult;
    auto nozzles = LayeredNozzleGroupResult::create(std::vector<int>{0, 0}, {{"0.4", nvtStandard, 0, 0}}, {0, 1});
    require(nozzles.has_value(), "nozzle fixture did not initialize");
    processor.initialize_from_context(std::make_shared<LayeredNozzleGroupResult>(std::move(*nozzles)));
    processor.result().filaments_count = 2;
}

void verify_original_callbacks()
{
    Collector capture;
    GCodeProcessor protected_export;
    protected_export.initialize_for_export("unused.gcode", capture);
    require(capture.move_count() == 1 && capture.move_at(0).kind == MoveKind::Dummy,
            "initial native dummy must have a matching provenance vertex");
    protected_export.reset();
    require(capture.move_count() == 0 && protected_export.result().moves.empty(),
            "native reset must discard the initial dummy in both vectors");

    GCodeProcessor ordinary;
    ordinary.initialize("unused.gcode");
    ordinary.reset();
    const std::string first = "G10\r\n; ignored\rG11\n";
    const std::string event = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Pause_Print) + "\r";
    const std::string rest = "\nG10\nG1 X5 Z0.2 E0.6 F1200\n";
    for (const auto& bytes : {first, event, rest}) {
        protected_export.process_buffer(bytes);
        ordinary.process_buffer(bytes);
    }

    const auto& result = protected_export.get_result().moves;
    const auto& ordinary_result = ordinary.get_result().moves;
    capture.finish_capture(result.size());
    require(result.size() == 5 && ordinary_result.size() == result.size() && capture.source_count() == 5,
            "one exact source is retained for each observed native callback");
    for (size_t i = 0; i < result.size(); ++i) {
        require(result[i].type == ordinary_result[i].type && result[i].gcode_id == ordinary_result[i].gcode_id &&
                result[i].position == ordinary_result[i].position, "opt-in must preserve native moves");
    }
    require(result[2].type == EMoveType::Retract && result[3].type == EMoveType::Pause_Print &&
            result[4].type == EMoveType::Extrude, "the original Z corrector must move the pause after the retract");
    require(capture.move_at(2).stable_id == 3 && capture.move_at(3).stable_id == 2 &&
            capture.move_at(3).kind == MoveKind::SyntheticEvent && capture.move_at(3).source_id == 3 &&
            capture.move_at(4).stable_id == 4, "the relocated marker must retain its birth and original callback");
    const auto retract = capture.source(1), unretract = capture.source(2), pause = capture.source(3), later = capture.source(4);
    require(retract.raw_begin == 0 && retract.raw_end == 3 && retract.consumed_end == 5 && retract.parser_record == 1,
            "CRLF must belong to the actual retract callback");
    require(unretract.raw_begin == first.find("G11") && unretract.consumed_end == first.size() && unretract.parser_record == 3,
            "ignored records must still count in native callback order");
    require(pause.raw_begin == first.size() && pause.raw_end == first.size() + event.size() - 1 &&
            pause.consumed_end == first.size() + event.size() && pause.parser_record == 4,
            "the marker's source must end at the original write boundary");
    require(later.raw_begin == first.size() + event.size() + 1 && later.parser_record == 6,
            "the next write's standalone LF is a distinct parser record");
}

void verify_seam_anchor()
{
    Collector capture;
    GCodeProcessor processor;
    processor.initialize_for_export("unused.gcode", capture);
    processor.reset();
    processor.process_buffer("G1 X1 Y1 Z0.2 F1200\n; FEATURE: Outer wall\n"
                             "G1 X2 Y1 E0.1\nG1 X2 Y2 E0.2\nG1 X1 Y2 E0.3\nG1 X1 Y1 E0.4\nG1 X3 Y1 F1200\n");
    const auto& result = processor.get_result().moves;
    capture.finish_capture(result.size());
    require(result.size() == 7 && result[5].type == EMoveType::Seam && result[6].type == EMoveType::Travel,
            "the native closed perimeter must create a seam on the next travel callback");
    const auto wall = capture.move_at(4), seam = capture.move_at(5), travel = capture.move_at(6);
    require(seam.kind == MoveKind::SyntheticPrevious && seam.previous_stable_move == wall.stable_id &&
            seam.source_id == travel.source_id && capture.source(*seam.source_id).parser_record == 7,
            "the seam must retain its prior anchor separately from its real callback source");
}

void reject_embedded_nul()
{
    Collector capture;
    GCodeProcessor processor;
    processor.initialize_for_export("unused.gcode", capture);
    processor.reset();
    try {
        processor.process_buffer(std::string("G10\0G11", 7));
    } catch (const std::runtime_error& failure) {
        require(std::string(failure.what()) == "NATIVE_PROCESSOR_PROVENANCE_NUL" && capture.move_count() == 0,
                "bytes that truncate the native parser must fail before a callback moves anything");
        return;
    }
    throw std::runtime_error("unconsumed input was accepted");
}

void verify_real_rewrites()
{
    Scratch tmp;
    // The original rewriter reads 96 KiB chunks. Put CR and LF in different reads.
    const std::string long_comment(96 * 1024 - 1, ';');
    using Tags = GCodeProcessor::ETags;
    const auto tag = [](Tags value) { return ";" + GCodeProcessor::reserved_tag(value); };
    const std::string input = long_comment + "\r\n" + tag(Tags::First_Line_M73_Placeholder) + "\r\n"
        + tag(Tags::MachineStartGCodeEnd) + "\nT0 H0\rG1 X10 Y1 Z0.2 E0.1 F60\r\nT1 H0\n"
          "G1 X25 Y3 E0.2 F60\r\nG1 X50 Y5 E0.3 F60\n" + tag(Tags::Last_Line_M73_Placeholder) + "\nG11";
    const auto captured_path = tmp.path / "captured.gcode", ordinary_path = tmp.path / "ordinary.gcode";
    write_file(captured_path, input);
    write_file(ordinary_path, input);
    Collector capture;
    GCodeProcessor protected_export;
    protected_export.initialize_for_export(captured_path.string(), capture);
    protected_export.reset();
    set_nozzles(protected_export);
    GCodeProcessor ordinary;
    ordinary.initialize(ordinary_path.string());
    ordinary.reset();
    set_nozzles(ordinary);
    protected_export.process_buffer(input);
    ordinary.process_buffer(input);
    NativeEmissionMap::Collector emission;
    const auto layer_begin = input.find(tag(Tags::First_Line_M73_Placeholder));
    emission.capture(std::string_view(input).substr(0, layer_begin));
    uint64_t invocation = 0;
    const auto scope = emission.open_scope(invocation, 42);
    emission.capture(std::string_view(input).substr(layer_begin), scope);
    protected_export.finalize(true, &emission);
    ordinary.finalize(true);

    const std::string output = read_file(captured_path);
    require(output == read_file(ordinary_path), "capture must preserve ordinary final G-code bytes");
    NativeEmissionMap::publish(captured_path.string(), emission);
    const auto emission_map = read_file(captured_path.string() + NativeEmissionMap::sidecar_suffix);
    require(emission_map.find("\"gcodeSha256\":\"" + sha256(output) + "\"") != std::string::npos &&
            emission_map.find("\"origin\":\"REWRITTEN\",\"scopeId\":\"1\"") != std::string::npos &&
            emission_map.find("\"origin\":\"NATIVE_POSTPROCESSOR\",\"scopeId\":null,\"associatedScopeId\":\"1\"") != std::string::npos,
            "both real rewrite passes must retain layer and insertion classifications over exact final bytes");
    require(output.substr(0, long_comment.size() + 2) == long_comment + "\n\n" && output.substr(output.size() - 4) == "G11\n",
            "both physical read-boundary CRLF and final unterminated input follow the ordinary byte rewrite");
    require(output.find("M73 P0 R") != std::string::npos && output.find("M73 P100 R0\n") != std::string::npos &&
            output.find("_PLACEHOLDER") == std::string::npos, "native M73 placeholders must be actually rewritten");
    require(capture.original_bytes() == input.size() && capture.original_sha256() == sha256(input) &&
            capture.output_bytes() == output.size() && capture.output_sha256() == sha256(output),
            "sealed capture must bind the exact before and after files");
    bool associated_progress = false, associated_filament_change = false;
    capture.visit_all_bindings([&](uint64_t source_id, const NativeProcessorProvenance::Binding& binding) {
        const auto source = capture.source(source_id);
        const auto raw = std::string_view(input).substr(source.raw_begin, source.raw_end - source.raw_begin);
        const auto from = std::string_view(input).substr(binding.original_begin, binding.original_end - binding.original_begin);
        const auto to = std::string_view(output).substr(binding.output_begin, binding.output_end - binding.output_begin);
        if (binding.relation == Relation::Exact)
            require(from == to, "copied physical lexemes must remain exact through both passes");
        if (binding.relation == Relation::Association && raw.substr(0, 3) == "G1 " && to.substr(0, 5) == "M73 P")
            associated_progress = true;
        if (binding.relation == Relation::Association && raw == "T1 H0" && to == "M73 E0\n")
            associated_filament_change = true;
    });
    require(associated_progress && associated_filament_change,
            "native first-pass progress and second-pass filament inserts must retain their source association");
}

void reject_replaced_rewrite_input()
{
    Scratch tmp;
    const auto file = tmp.path / "captured.gcode";
    write_file(file, "G11\n");
    Collector capture;
    GCodeProcessor processor;
    processor.initialize_for_export(file.string(), capture);
    processor.reset();
    set_nozzles(processor);
    processor.process_buffer("G10\n");
    try {
        processor.finalize(true);
    } catch (const std::runtime_error& failure) {
        require(std::string(failure.what()) == "NATIVE_PROCESSOR_PROVENANCE_INPUT_MISMATCH",
                "an altered original file must fail the exact first-rewrite digest");
        require(read_file(file) == "G11\n", "failed verification must leave the input file in place");
        return;
    }
    throw std::runtime_error("altered on-disk rewrite input was accepted");
}

void reject_failed_second_rewrite()
{
    Scratch tmp;
    const auto file = tmp.path / "captured.gcode";
    write_file(file, "G10\r\n");
    std::filesystem::create_directory(file.string() + ".postprocessed");
    Collector capture;
    GCodeProcessor processor;
    processor.initialize_for_export(file.string(), capture);
    processor.reset();
    set_nozzles(processor);
    processor.process_buffer("G10\r\n");
    bool denied_single_pass = false;
    try {
        processor.finalize(false);
    } catch (const std::logic_error&) {
        denied_single_pass = true;
    }
    require(denied_single_pass, "protected finalization cannot skip the rewrite path");
    bool failed_second = false;
    try {
        processor.finalize(true);
    } catch (const std::runtime_error& failure) {
        failed_second = std::string(failure.what()).find("opening the second rewrite") != std::string::npos;
    }
    require(failed_second && read_file(file) == "G10\r\n" && read_file(file.string() + ".postprocess") == "G10\n",
            "a second rewrite I/O failure must keep the original file even after the first pass succeeds");
    bool unsealed = false;
    try {
        (void)capture.output_sha256();
    } catch (const std::runtime_error&) {
        unsealed = true;
    }
    require(unsealed, "a failed rewrite must leave no publishable capture digest");
}
} // namespace

int main()
{
    try {
        verify_original_callbacks();
        verify_seam_anchor();
        reject_embedded_nul();
        verify_real_rewrites();
        reject_replaced_rewrite_input();
        reject_failed_second_rewrite();
        std::cout << "native processor export callbacks and rewrites verified\n";
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
