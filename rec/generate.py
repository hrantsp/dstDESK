#!/usr/bin/env python3
"""Generate protocol constants for both halves of the pipeline from protocol.json.

One source, two outputs: a C++ header for dstDESK and an ES module for dstORCH.
Hand-maintained copies on either side drift; see decision 3 in dstOMNI/DESIGN.md.

    generate.py --cpp-out <path> --js-out <path>
    generate.py --check --cpp-out <path> --js-out <path>

--check regenerates in memory and exits non-zero if the files on disk differ, so a
build can fail loudly rather than compiling against stale constants.
"""

import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).parent
SOURCE = HERE / "protocol.json"

BANNER = "Generated from protocol.json by generate.py — do not edit."

_CPP_TYPES = {"u8": "std::uint8_t", "u16": "std::uint16_t", "u32": "std::uint32_t"}
_TYPE_SIZES = {"u8": 1, "u16": 2, "u32": 4}


def load():
    spec = json.loads(SOURCE.read_text())
    validate(spec)
    return spec


def validate(spec):
    """Catch specification mistakes here rather than as puzzling runtime behaviour."""
    frame = spec["frame"]
    fields = frame["fields"]

    expected_offset = 0
    for field in fields:
        if field["offset"] != expected_offset:
            raise SystemExit(
                f"protocol.json: field '{field['name']}' is at offset {field['offset']}, "
                f"but the preceding fields end at {expected_offset}. "
                "Header fields must be contiguous."
            )
        expected_offset += _TYPE_SIZES[field["type"]]

    if expected_offset != frame["headerBytes"]:
        raise SystemExit(
            f"protocol.json: fields occupy {expected_offset} bytes but headerBytes is "
            f"{frame['headerBytes']}."
        )

    # The payload is read directly as int16_t on the C++ side, so the header must not
    # leave it on an odd address.
    if frame["headerBytes"] % 2 != 0:
        raise SystemExit(
            f"protocol.json: headerBytes ({frame['headerBytes']}) is odd, which would "
            "misalign the int16 payload."
        )

    if spec["audio"]["encoding"] != "pcm_s16le":
        raise SystemExit("protocol.json: only pcm_s16le is supported by the generator.")

    values = [s["value"] for s in spec["streams"]]
    if len(set(values)) != len(values):
        raise SystemExit("protocol.json: duplicate stream values.")


def derived(spec):
    audio = spec["audio"]
    frame_bytes = spec["frame"]["headerBytes"] + audio["frameSamples"] * 2
    duration_ms = audio["frameSamples"] * 1000 / audio["sampleRate"]
    return frame_bytes, duration_ms


def render_cpp(spec):
    audio = spec["audio"]
    frame = spec["frame"]
    frame_bytes, duration_ms = derived(spec)

    out = [
        f"// {BANNER}",
        "",
        "#ifndef DST_DESK_CORE_PROTOCOL_HPP",
        "#define DST_DESK_CORE_PROTOCOL_HPP",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace DST { namespace DESK { namespace Core {",
        "",
        f"inline constexpr std::uint8_t  kVersion      = {spec['protocolVersion']};",
        f"inline constexpr std::uint32_t kSampleRate   = {audio['sampleRate']};",
        f"inline constexpr std::uint16_t kFrameSamples = {audio['frameSamples']};",
        f"inline constexpr std::uint16_t kChannels     = {audio['channels']};",
        f"inline constexpr std::size_t   kHeaderBytes  = {frame['headerBytes']};",
        f"inline constexpr std::size_t   kFrameBytes   = {frame_bytes};",
        f"inline constexpr double        kFrameMillis  = {duration_ms};",
        f"inline constexpr std::uint16_t kDefaultPort  = {spec['transport']['defaultPort']};",
        "",
        "// The origin the server accepts unless told otherwise. Fixed by the 'key' in",
        "// dstORCH/manifest.json, from which Chrome derives the extension id.",
        f'inline constexpr auto kExtensionOrigin = "{spec["transport"]["extensionOrigin"]}";',
        "",
        "// Byte offsets within the frame header.",
    ]
    for field in frame["fields"]:
        name = "kOffset" + field["name"][0].upper() + field["name"][1:]
        out.append(f"inline constexpr std::size_t {name:<21} = {field['offset']};")

    names = [ss["name"].capitalize() for ss in spec["streams"]]
    width = max(len(nn) for nn in names)

    out += ["", "struct Stream { enum Value : std::uint8_t", "{"]
    for stream, name in zip(spec["streams"], names):
        out.append(f"  {name:<{width}} = {stream['value']},")
    out += ["};", ""]

    def switchBlock(subject, arms):
        """Render a switch in the house style: arms stacked under the opening brace,
        with every ` : ` aligned. Padding is computed rather than hand-counted, so the
        alignment survives a rename in protocol.json."""
        lhsWidth = max(len(lhs) for lhs, _ in arms)
        lines = []
        head = f"    switch ({subject}) {{ "
        for index, (lhs, rhs) in enumerate(arms):
            prefix = head if index == 0 else " " * len(head)
            lines.append(f"{prefix}{lhs:<{lhsWidth}} : {rhs}")
        lines[-1] += " }"
        return lines

    out += ["  static constexpr auto label(Stream::Value val) noexcept", "  {"]
    out += switchBlock(
        "val",
        [(f"case Stream::{nn}", f'return "{ss["label"]}";')
         for ss, nn in zip(spec["streams"], names)]
        + [("default", 'return "Unknown";')],
    )
    out += ["  }", ""]

    out += [
        "  // Whether a raw wire byte names a stream this protocol version defines.",
        "  static constexpr bool isKnown(std::uint8_t value) noexcept",
        "  {",
    ]
    out += switchBlock(
        "value",
        [(f"case {nn}", "return true;") for nn in names]
        + [("default", "return false;")],
    )
    out += [
        "  }",
        "};",
        "",
        "// Close codes paired with the error codes in PROTOCOL.md §4.3.",
    ]
    for err in spec["errorCodes"]:
        const = "kClose" + "".join(pp.capitalize() for pp in err["code"].split("-"))
        out.append(f"inline constexpr std::uint16_t {const:<33} = {err['close']};")

    out += [
        "",
        "} } } // namespace DST::DESK::Core",
        "",
        "#endif // DST_DESK_CORE_PROTOCOL_HPP",
        "",
    ]
    return "\n".join(out)


def render_js(spec):
    audio = spec["audio"]
    frame = spec["frame"]
    frame_bytes, duration_ms = derived(spec)

    out = [
        f"// {BANNER}",
        "",
        f"export const VERSION = {spec['protocolVersion']};",
        f"export const SAMPLE_RATE = {audio['sampleRate']};",
        f"export const FRAME_SAMPLES = {audio['frameSamples']};",
        f"export const CHANNELS = {audio['channels']};",
        f"export const HEADER_BYTES = {frame['headerBytes']};",
        f"export const FRAME_BYTES = {frame_bytes};",
        f"export const FRAME_MILLIS = {duration_ms};",
        f"export const DEFAULT_PORT = {spec['transport']['defaultPort']};",
        f"export const EXTENSION_ORIGIN = '{spec['transport']['extensionOrigin']}';",
        "",
        "// Byte offsets within the frame header.",
        "export const OFFSET = Object.freeze({",
    ]
    for field in frame["fields"]:
        out.append(f"  {field['name']}: {field['offset']},")
    out += ["});", "", "export const STREAM = Object.freeze({"]
    for stream in spec["streams"]:
        out.append(f"  {stream['name'].upper()}: {stream['value']},")
    out += ["});", "", "export const STREAM_LABEL = Object.freeze({"]
    for stream in spec["streams"]:
        out.append(f"  {stream['value']}: {json.dumps(stream['label'])},")
    out += [
        "});",
        "",
        "// The wire is little-endian; every DataView call must pass true.",
        "export const LITTLE_ENDIAN = true;",
        "",
    ]
    return "\n".join(out)


def write(path, text, check):
    path = pathlib.Path(path)
    if check:
        if not path.exists():
            print(f"MISSING: {path}", file=sys.stderr)
            return False
        if path.read_text() != text:
            print(f"STALE:   {path} differs from protocol.json", file=sys.stderr)
            return False
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text() == text:
        return True  # leave mtime alone so CMake does not rebuild the world
    path.write_text(text)
    print(f"wrote {path}")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cpp-out", required=True)
    ap.add_argument(
        "--js-out",
        help="ES module output for dstORCH. Omit when dstORCH is not checked out "
        "beside dstDESK; the C++ side still builds.",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="verify existing output matches protocol.json; write nothing",
    )
    args = ap.parse_args()

    spec = load()
    ok = write(args.cpp_out, render_cpp(spec), args.check)
    if args.js_out:
        ok &= write(args.js_out, render_js(spec), args.check)

    if not ok:
        print(
            "\nGenerated protocol constants are out of date. Re-run generate.py.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
