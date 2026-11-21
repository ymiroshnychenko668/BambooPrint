#include "libslic3r/GCode/NativeObjectWitness.hpp"

#include <cassert>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

using namespace Slic3r::NativeObjectWitness;

int main() {
    Collector witness(true);
    const Instance first{1, 2, 3}, second{4, 5, 6};
    const std::string move = "G1 X1 E1\n";
    const std::string head = "; a counterfeit G1 X1 E1 comment\n";
    const std::string batch = head + move + move;
    const size_t start = head.size();
    witness.operation(first, Kind::Perimeter, start - 2).shifted(1)
        .emitted(1, 2, Writer::LinearXY, 0.25, false, move);
    witness.operation(second, Kind::Infill, start + move.size())
        .emitted(0, 4, Writer::LinearXY, 0.0, true, move);
    witness.seal_layer(batch);
    witness.operation(first, Kind::Ironing, 0)
        .emitted(0, 12, Writer::ArcCCW, 0.05, false, "G3\n");
    witness.seal_layer("G3\n");
    const auto read = [&](Collector::Document document) {
        std::string text;
        witness.write(document, {first, second}, std::string(64, 'a'), 10,
            [&](std::string_view bytes) { text += bytes; });
        return nlohmann::json::parse(text);
    };
    const auto membership = read(Collector::Document::Membership), audit = read(Collector::Document::Audit);
    const auto operation = read(Collector::Document::Operation), writer = read(Collector::Document::Writer);
    assert(membership["events"].size() == 3 && audit["events"].size() == 3 && writer["events"].size() == 3);
    assert(membership["events"][0]["begin"] == std::to_string(start) &&
        membership["events"][1]["begin"] == std::to_string(start + move.size()));
    assert(membership["events"][0]["bytesHex"] == membership["events"][1]["bytesHex"]);
    assert(membership["events"][0]["printObjectId"] == "1" && membership["events"][1]["printObjectId"] == "4");
    assert(operation["stageLengths"][0] == std::to_string(batch.size() + 3) &&
        operation["roots"][2]["begin"] == std::to_string(batch.size()));
    assert(operation["edges"].empty() && operation["spans"][0]["relation"] == "UNATTRIBUTED");
    assert(writer["events"][1]["forceNoExtrusion"] == true && writer["events"][2]["writerKind"] == "ARC_CCW");
    assert(!writer["events"][0].contains("positiveCommand"));

    Collector integrity(false);
    integrity.operation(first, Kind::Infill, 0).emitted(0, 2, Writer::LinearXY, 1.0, false, move);
    try {
        integrity.seal_layer(head + move);
        assert(false); // Matching text somewhere else in the layer proves nothing.
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()) == "NATIVE_OBJECT_WITNESS_BYTES");
    }
}
