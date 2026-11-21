#!/usr/bin/env python3
"""Handwritten JSON/byte oracle for the standalone processor-map writer.

The C++ driver supplies literal callbacks and synthetic observations. This file
does not import the production writer, native parser, or provenance algorithm.
It checks public records against fixed observations and explicit byte intervals.
This is not proof that real native integration supplies those observations.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


HEADER = dict(record="header", schema="corvus-native-processor-map/1",
              generator="corvusprint-gcodeprocessor/1", basis="NATIVE_PROCESSOR_OBSERVATION",
              lineage="ORIGINAL_PROCESSOR_CALLBACK_BYTES", originalOffsetDomain="ORIGINAL_EXPORT_BYTES",
              outputOffsetDomain="FINAL_GCODE_BYTES", enumNamespace="corvusprint-processor-enums/1",
              coordinateBasis="NATIVE_PREVIEW_TRANSFORMED", timeBasis="NATIVE_PREFIX_ESTIMATE",
              timeModeOrder=["NORMAL", "STEALTH"], nativeGcodeIdMeaning="LEGACY_PROCESSOR_COUNTER",
              objectLabelMeaning="UNVERIFIED_NATIVE_LABEL", extruderFieldMeaning="NATIVE_FILAMENT_SLOT",
              arcSamples="NATIVE_PREVIEW_INTERPOLATION", rewritePasses="2", verification="NOT_EVALUATED")


def source(ident, record, begin, end, consumed):
    return dict(record="source", sourceId=str(ident), parserRecord=str(record), rawBegin=str(begin),
                rawEnd=str(end), consumedEnd=str(consumed), emptyRaw=begin == end)


def move(index, stable, kind, source_id=None, previous=None, **fields):
    result = dict(record="move", finalIndex=str(index), stableId=str(stable), kind=kind,
                  sourceId=None if source_id is None else str(source_id),
                  previousStableId=None if previous is None else str(previous), nativeGcodeId="0",
                  nativeMoveType="0", nativeExtrusionRole="0", nativePathType="0",
                  nativeExtruderField="0", nativeColorId="0", nativeObjectLabelId="-1",
                  nativePositionMm=[0, 0, 0], nativeArcCenterMm=None, deltaExtruderMm=0,
                  feedrateMmPerSecond=0, widthMm=0, heightMm=0, mm3PerMm=0, fanPercent=0,
                  additionalFanPercent=0, temperatureC=0, nativeLayerDurationSeconds=0,
                  nativeElapsedSeconds=[0, 0], nativeThermalIndex=[0, 0, 0], nativePrintZMm=0,
                  paLineCalibration=False, activeArcPointCount="0", ignoredInterpolationPointCount="0")
    result.update(fields)
    return result


def binding(ident, source_id, begin, end, original_begin, original_end, relation="EXACT"):
    return dict(record="binding", bindingId=str(ident), sourceId=str(source_id), finalBegin=str(begin),
                finalEnd=str(end), originalBegin=str(original_begin), originalEnd=str(original_end), relation=relation)


def summary(original, final, lines, sources, moves, bindings, active=0, ignored=0):
    return dict(record="summary", originalSha256=hashlib.sha256(original).hexdigest(), originalBytes=str(len(original)),
                gcodeSha256=hashlib.sha256(final).hexdigest(), gcodeBytes=str(len(final)), physicalLineCount=str(lines),
                sourceCount=str(sources), moveCount=str(moves), bindingCount=str(bindings),
                activeArcPointCount=str(active), ignoredInterpolationPointCount=str(ignored))


def literal_fixtures():
    dummy = move(0, 0, "DUMMY")
    basic_bytes = b"G1 X1\n"
    basic = [HEADER, source(1, 1, 0, 5, 6), dummy,
             move(1, 1, "OBSERVED", 1, nativeMoveType="8", nativePathType="1", nativeGcodeId="1"),
             binding(1, 1, 0, 5, 0, 5), summary(basic_bytes, basic_bytes, 1, 1, 2, 1)]
    original, final = b"A\n;P\nB\n", b"I\nA\nP1\nP2\nB\n"
    rich = [HEADER, source(1, 1, 0, 1, 2), source(2, 2, 2, 4, 5), source(3, 3, 5, 6, 7), dummy,
            move(1, 1, "OBSERVED", 1, nativeGcodeId="4", nativeMoveType="8", nativePathType="1",
                 nativePositionMm=[1.25, -0.0, 0.25]),
            move(2, 3, "SYNTHETIC_PREVIOUS", 3, previous=1, nativeGcodeId="4", nativeMoveType="3", nativeExtrusionRole="1"),
            move(3, 2, "SYNTHETIC_EVENT", 2, nativeGcodeId="5", nativeMoveType="6", nativeExtrusionRole="19"),
            move(4, 4, "OBSERVED", 3, nativeGcodeId="5", nativeMoveType="10", nativeExtrusionRole="2", nativePathType="2",
                 nativeExtruderField="2", nativeColorId="255", nativeObjectLabelId="7", nativePositionMm=[3, 2, 0.25],
                 nativeArcCenterMm=[1, 1, 0.25], deltaExtruderMm=0.5, feedrateMmPerSecond=42.5, widthMm=0.5, heightMm=0.25,
                 mm3PerMm=0.125, fanPercent=42, additionalFanPercent=25, temperatureC=210, nativeLayerDurationSeconds=12.5,
                 nativeElapsedSeconds=[7.5, 9], nativeThermalIndex=[1, 3, 2], nativePrintZMm=0.25,
                 paLineCalibration=True, activeArcPointCount="2"),
            binding(1, 1, 0, 2, 0, 1, "ASSOCIATION"), binding(2, 1, 2, 3, 0, 1),
            binding(3, 2, 4, 10, 2, 4, "REWRITTEN"), binding(4, 3, 10, 11, 5, 6),
            dict(record="arcPoint", stableId="4", pointIndex="0", nativePositionMm=[2.25, 1.5, 0.25]),
            dict(record="arcPoint", stableId="4", pointIndex="1", nativePositionMm=[3, 2, 0.25]),
            summary(original, final, 5, 3, 5, 4, active=2)]
    tail = b"A\n;tail\n"
    unused = [HEADER, source(1, 1, 0, 1, 2), dummy, move(1, 1, "OBSERVED", 1, nativeMoveType="8"),
              binding(1, 1, 0, 1, 0, 1), summary(tail, tail, 2, 1, 2, 1)]
    mixed_bytes = b"A\rB\r\nC\nD"
    mixed = [HEADER, source(1, 1, 0, 1, 2), source(2, 2, 2, 3, 5), source(3, 3, 5, 6, 7), source(4, 4, 7, 8, 8), dummy,
             move(1, 1, "OBSERVED", 1, nativeMoveType="8"), move(2, 2, "OBSERVED", 2, nativeMoveType="8"),
             move(3, 3, "OBSERVED", 3, nativeMoveType="8"), move(4, 4, "OBSERVED", 4, nativeMoveType="8"),
             binding(1, 1, 0, 1, 0, 1), binding(2, 2, 2, 3, 2, 3), binding(3, 3, 5, 6, 5, 6), binding(4, 4, 7, 8, 7, 8),
             summary(mixed_bytes, mixed_bytes, 4, 4, 5, 4)]
    stale = [HEADER, source(1, 1, 0, 5, 6), dummy,
             move(1, 1, "OBSERVED", 1, nativeMoveType="8", nativePathType="1", nativeGcodeId="1", ignoredInterpolationPointCount="17"),
             binding(1, 1, 0, 5, 0, 5), summary(basic_bytes, basic_bytes, 1, 1, 2, 1, ignored=17)]
    edges = [HEADER, source(1, 1, 0, 5, 6), dummy,
             move(1, 1, "OBSERVED", 1, nativeMoveType="8", nativePathType="1", nativeGcodeId="4294967295",
                  nativeObjectLabelId="-2147483648", nativeExtrusionRole="21", nativeColorId="255", nativeExtruderField="255",
                  feedrateMmPerSecond=3.40282347e38, deltaExtruderMm=1.40129846e-45),
             binding(1, 1, 0, 5, 0, 5), summary(basic_bytes, basic_bytes, 1, 1, 2, 1)]
    fixtures = {"minimal": (basic_bytes, basic), "complex": (final, rich), "locale_and_signed_zero": (final, rich),
                "unused_tail": (tail, unused), "empty": (b"", [HEADER, dummy, summary(b"", b"", 0, 0, 1, 0)]),
                "mixed_physical_lines": (mixed_bytes, mixed), "nonarc_stale_points_ignored": (basic_bytes, stale),
                "native_hint_and_integer_edges": (basic_bytes, edges)}
    fixtures["no_dummy_first_observed"] = (basic_bytes, [HEADER, source(1, 1, 0, 5, 6),
        move(0, 0, "OBSERVED", 1, nativeMoveType="8", nativePathType="1", nativeGcodeId="1"),
        binding(1, 1, 0, 5, 0, 5), summary(basic_bytes, basic_bytes, 1, 1, 1, 1)])
    no_dummy = b";P\nA\nB\n"
    fixtures["no_dummy_relocated_event_zero_and_seam_anchor"] = (no_dummy, [HEADER,
        source(1, 1, 0, 2, 3), source(2, 2, 3, 4, 5), source(3, 3, 5, 6, 7),
        move(0, 1, "OBSERVED", 2, nativeMoveType="8", nativeGcodeId="2"),
        move(1, 0, "SYNTHETIC_EVENT", 1, nativeMoveType="6", nativeGcodeId="2"),
        move(2, 2, "SYNTHETIC_PREVIOUS", 3, previous=0, nativeMoveType="3", nativeGcodeId="2"),
        binding(1, 1, 0, 2, 0, 2), binding(2, 2, 3, 4, 3, 4), binding(3, 3, 5, 6, 5, 6),
        summary(no_dummy, no_dummy, 3, 3, 3, 3)])
    fixtures["zero_actual_moves_empty"] = (b"", [HEADER, summary(b"", b"", 0, 0, 0, 0)])
    fixtures["zero_actual_moves_comments"] = (b";C\n", [HEADER, summary(b";C\n", b";C\n", 1, 0, 0, 0)])
    for name in ["output_mode_0600", "map_byte_boundary_reference", "map_byte_exact_boundary", "record_boundary_reference", "record_exact_boundary"]:
        fixtures[name] = (basic_bytes, basic)
    return fixtures


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise AssertionError("duplicate JSON key: " + key)
        result[key] = value
    return result


def reject_constant(value):
    raise AssertionError("nonfinite JSON constant: " + value)


def compare(actual, expected, location="record"):
    # Unlike ordinary Python equality, booleans must not compare equal to0/1.
    if isinstance(expected, dict):
        assert isinstance(actual, dict) and actual.keys() == expected.keys(), (location, "closed keys", actual, expected)
        for key in expected:
            compare(actual[key], expected[key], location + "." + key)
    elif isinstance(expected, list):
        assert isinstance(actual, list) and len(actual) == len(expected), (location, actual, expected)
        for index, (a, e) in enumerate(zip(actual, expected)):
            compare(a, e, location + "[" + str(index) + "]")
    elif isinstance(expected, bool) or expected is None or isinstance(expected, str):
        assert type(actual) is type(expected) and actual == expected, (location, actual, expected)
    else:
        assert type(actual) in (int, float) and math.isfinite(actual) and actual == expected, (location, actual, expected)
        if isinstance(expected, float) and expected == 0:
            assert math.copysign(1, actual) == math.copysign(1, expected), (location, "zero sign")


def verify(root):
    fixtures = literal_fixtures()
    successful_cancels = [p.parent.name for p in root.glob("cancel_*/output.gcode.processor-map.jsonl")]
    assert len(successful_cancels) == 1, "exactly one cancellation loop completes"
    fixtures[successful_cancels[0]] = fixtures["complex"]
    records_checked = 0
    for name, (gcode, expected) in fixtures.items():
        case = root / name
        assert (case / "output.gcode").read_bytes() == gcode, (name, "literal final bytes")
        path = case / "output.gcode.processor-map.jsonl"
        raw = path.read_bytes()
        assert raw.endswith(b"\n") and b"\r" not in raw and b"\0" not in raw, (name, "canonical JSONL framing")
        lines = raw.splitlines(keepends=True)
        assert all(len(line) <= 2048 for line in lines), (name, "record cap includes LF")
        actual = [json.loads(line, object_pairs_hook=unique_object, parse_constant=reject_constant) for line in lines]
        compare(actual, expected, name)
        assert path.stat().st_mode & 0o777 == 0o600, (name, "published file permissions")
        assert not (case / "output.gcode.processor-map.jsonl.pending").exists(), (name, "owned pending removed")
        records_checked += len(actual)
    edge_records = json.loads((root / "native_hint_and_integer_edges/output.gcode.processor-map.jsonl").read_text().splitlines()[3])
    assert struct.pack("!f", edge_records["feedrateMmPerSecond"]).hex() == "7f7fffff"
    assert struct.pack("!f", edge_records["deltaExtruderMm"]).hex() == "00000001"
    assert (root / "complex/output.gcode.processor-map.jsonl").read_bytes() == (root / "locale_and_signed_zero/output.gcode.processor-map.jsonl").read_bytes()
    # Deliberately foreign pending entries survive; all other pending files are leaks.
    foreign = {"existing_pending_not_deleted", "pending_symlink_not_followed_or_deleted"}
    actual_pending = {p.parent.name for p in root.glob("*/output.gcode.processor-map.jsonl.pending")}
    assert actual_pending == foreign, ("pending ownership", actual_pending)
    return dict(status="PASS", literalFixtures=len(fixtures), literalRecords=records_checked,
                completedCancelAttempt=successful_cancels[0], finalByteAndHashChecks=True,
                closedSchemaAndTypedFields=True, finiteFloat32EdgesAndSignedZero=True,
                scope="synthetic literal observations; standalone collector/writer; no real native adapter/export")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", type=Path)
    parser.add_argument("--scratch-root", type=Path)
    parser.add_argument("--verify-existing", type=Path)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    if args.verify_existing:
        assert not args.driver and not args.scratch_root
        result = verify(args.verify_existing)
        print(json.dumps(result, sort_keys=True))
        return
    assert args.driver and args.scratch_root and args.scratch_root.is_dir()
    scratch = Path(tempfile.mkdtemp(prefix="processor-map-contract-", dir=args.scratch_root.resolve()))
    assert scratch.stat().st_mode & 0o777 == 0o700
    success = False
    try:
        command = [str(args.driver.resolve()), str(scratch / "fixtures")]
        run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=45)
        (scratch / "driver.txt").write_text(run.stdout)
        print(run.stdout, end="")
        assert run.returncode == 0, "C++ driver failed; preserved " + str(scratch)
        assert len(re.findall(r"^PASS ", run.stdout, re.M)) == 74 and "RESULT 74/74 passed" in run.stdout
        result = verify(scratch / "fixtures")
        result.update(driverTests=74, command=command, privateScratch=str(scratch))
        (scratch / "oracle-result.json").write_text(json.dumps(result, indent=2) + "\n")
        print("ORACLE " + json.dumps(result, sort_keys=True))
        success = True
    finally:
        if success and not args.keep:
            shutil.rmtree(scratch)
        else:
            print("PRESERVED " + str(scratch))


if __name__ == "__main__":
    main()
