#!/usr/bin/env python3
"""Build the Drastic Android post-FX programs for the Switch renderers.

The original .dfx/.dsd files remain external build inputs from the user's APK.
This tool emits a GLES source header, Vulkan SPIR-V modules, and the two SMAA
lookup textures into a temporary build directory.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class ProgramSpec:
    name: str
    manifest: str
    pass_index: int


PROGRAMS = (
    ProgramSpec("copy", "None.dfx", 0),
    ProgramSpec("quilez", "Quilez.dfx", 0),
    ProgramSpec("scanline", "Scanline.dfx", 0),
    ProgramSpec("scale2x", "Scale2X.dfx", 0),
    ProgramSpec("hq2x", "HQ2X.dfx", 0),
    ProgramSpec("fxaa", "FXAA.dfx", 0),
    ProgramSpec("fxaa_luma", "FXAA HQ.dfx", 0),
    ProgramSpec("fxaa_hq", "FXAA HQ.dfx", 1),
    ProgramSpec("smaa_edge", "SMAA.dfx", 0),
    ProgramSpec("smaa_weight", "SMAA.dfx", 1),
    ProgramSpec("smaa_blend", "SMAA.dfx", 2),
)

VULKAN_PROGRAMS = {
    "scale2x", "hq2x", "fxaa", "fxaa_luma", "fxaa_hq",
    "smaa_edge", "smaa_weight", "smaa_blend",
}


TAG_RE = re.compile(r"<(?P<tag>[A-Za-z0-9_]+)(?::[^>]*)?>(?P<body>.*?)</(?P=tag)>", re.S)
STAGE_RE = re.compile(r"<(?P<tag>vertex|fragment)>(?P<body>.*?)</(?P=tag)>", re.S)
VARYING_RE = re.compile(
    r"\bvarying\s+(?P<type>[A-Za-z0-9_]+)\s+(?P<name>[A-Za-z0-9_]+)"
    r"(?P<array>\s*\[\s*(?P<count>[0-9]+)\s*\])?\s*;"
)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="latin-1")


def sections(text: str, wanted: str) -> list[str]:
    return [match.group("body").strip() for match in TAG_RE.finditer(text)
            if match.group("tag") == wanted]


def key_values(body: str, key: str) -> list[str]:
    values: list[str] = []
    for raw_line in body.splitlines():
        line = raw_line.split("//", 1)[0].strip()
        if not line or "=" not in line:
            continue
        current, value = (part.strip() for part in line.split("=", 1))
        if current == key:
            values.append(value)
    return values


def strip_embedded_versions(source: str) -> str:
    return "\n".join(line for line in source.splitlines()
                       if not line.lstrip().startswith("#version")) + "\n"


def strip_comments(source: str) -> str:
    def preserve_lines(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")
    source = re.sub(r"/\*.*?\*/", preserve_lines, source, flags=re.S)
    return re.sub(r"//[^\r\n]*", "", source)


def compose_program(shader_root: Path, spec: ProgramSpec) -> tuple[str, str, list[str]]:
    manifest_path = shader_root / spec.manifest
    manifest = read_text(manifest_path)
    passes = sections(manifest, "pass")
    if spec.pass_index >= len(passes):
        raise ValueError(f"{manifest_path}: pass {spec.pass_index} is missing")
    pass_body = passes[spec.pass_index]
    shader_values = key_values(pass_body, "shader")
    if not shader_values:
        raise ValueError(f"{manifest_path}: pass {spec.pass_index} has no shader")
    dsd_path = manifest_path.parent / shader_values[-1]
    dsd = read_text(dsd_path)
    stage_parts = {match.group("tag"): match.group("body").strip()
                   for match in STAGE_RE.finditer(dsd)}
    if "vertex" not in stage_parts or "fragment" not in stage_parts:
        raise ValueError(f"{dsd_path}: vertex or fragment stage is missing")

    includes = []
    for include in sections(manifest, "include"):
        for filename in key_values(include, "file"):
            includes.append(read_text(manifest_path.parent / filename))
    common = sections(manifest, "header") + includes
    vertex = "\n".join(sections(manifest, "vheader") + common +
                       [stage_parts["vertex"]])
    fragment = "\n".join(sections(manifest, "fheader") + common +
                         [stage_parts["fragment"]])
    sampler_names = []
    for raw_line in pass_body.splitlines():
        match = re.match(r"\s*sampler:([A-Za-z0-9_]+)\s*=", raw_line)
        if match:
            sampler_names.append(match.group(1))
    return (strip_comments(strip_embedded_versions(vertex)),
            strip_comments(strip_embedded_versions(fragment)), sampler_names)


def c_literal(source: str) -> str:
    lines = []
    for line in source.splitlines(keepends=True):
        escaped = (line.replace("\\", "\\\\")
                       .replace('"', '\\"')
                       .replace("\t", "\\t")
                       .replace("\r", "\\r")
                       .replace("\n", "\\n"))
        lines.append(f'    "{escaped}"')
    if not lines:
        return '    ""'
    return "\n".join(lines)


def write_gl_header(output: Path, sources: dict[str, tuple[str, str]]) -> None:
    parts = [
        "/* Generated from the user-supplied Drastic Android shader assets. */",
        "#ifndef DRASTIC_DFX_GL_GENERATED_H",
        "#define DRASTIC_DFX_GL_GENERATED_H",
        "",
    ]
    for spec in PROGRAMS:
        vertex, fragment = sources[spec.name]
        parts.extend([
            f"static const char dfx_{spec.name}_vertex_source[] =",
            c_literal(vertex) + ";",
            f"static const char dfx_{spec.name}_fragment_source[] =",
            c_literal(fragment) + ";",
            "",
        ])
    parts.extend(["#endif", ""])
    (output / "drastic_dfx_gl_generated.h").write_text(
        "\n".join(parts), encoding="utf-8", newline="\n")


def remove_builtin_uniforms(source: str) -> str:
    pattern = re.compile(
        r"\buniform\s+(?:(?:lowp|mediump|highp)\s+)?"
        r"(?:vec4\s+u_texture_size|vec2\s+u_target_size|float\s+u_time)\s*;"
    )
    return pattern.sub("", source)


def varying_locations(vertex: str) -> dict[str, int]:
    locations: dict[str, int] = {}
    next_location = 0
    for match in VARYING_RE.finditer(vertex):
        name = match.group("name")
        if name in locations:
            continue
        locations[name] = next_location
        next_location += int(match.group("count") or "1")
    return locations


def vulkan_stage(source: str, stage: str, samplers: list[str],
                 locations: dict[str, int]) -> str:
    source = strip_embedded_versions(source)
    source = re.sub(r"\b(?:lowp|mediump|highp)\b", "", source)
    source = remove_builtin_uniforms(source)

    for binding, name in enumerate(samplers):
        declaration = re.compile(
            rf"\buniform\s+sampler2D\s+{re.escape(name)}\s*;"
        )
        source, count = declaration.subn(
            f"layout(set = 0, binding = {binding}) uniform sampler2D {name};",
            source,
        )
        if stage == "fragment" and count != 1:
            raise ValueError(f"sampler {name} declaration count is {count}")

    if stage == "vertex":
        source = re.sub(
            r"\battribute\s+vec2\s+a_vertex_coordinate\s*;",
            "layout(location = 0) in vec2 a_vertex_coordinate;", source)
        source = re.sub(
            r"\battribute\s+vec2\s+a_texture_coordinate\s*;",
            "layout(location = 1) in vec2 a_texture_coordinate;", source)

    def replace_varying(match: re.Match[str]) -> str:
        name = match.group("name")
        if name not in locations:
            raise ValueError(f"unmapped varying {name}")
        qualifier = "out" if stage == "vertex" else "in"
        array = match.group("array") or ""
        return (f"layout(location = {locations[name]}) {qualifier} "
                f"{match.group('type')} {name}{array};")

    source = VARYING_RE.sub(replace_varying, source)
    preamble = [
        "#version 450",
        "#define texture2D texture",
        "#define texture2DLod textureLod",
        "layout(push_constant) uniform DfxParameters {",
        "    vec4 texture_size;",
        "    vec2 target_size;",
        "    float time;",
        "    float padding;",
        "} dfx_parameters;",
        "#define u_texture_size dfx_parameters.texture_size",
        "#define u_target_size dfx_parameters.target_size",
        "#define u_time dfx_parameters.time",
    ]
    if stage == "fragment":
        preamble.extend([
            "layout(location = 0) out vec4 dfx_output_color;",
            "#define gl_FragColor dfx_output_color",
        ])
    return "\n".join(preamble) + "\n" + source


def run(command: list[str]) -> None:
    completed = subprocess.run(command, text=True, capture_output=True)
    if completed.returncode:
        if completed.stdout:
            print(completed.stdout, file=sys.stderr)
        if completed.stderr:
            print(completed.stderr, file=sys.stderr)
        raise subprocess.CalledProcessError(completed.returncode, command)


def compile_vulkan(glslang: Path, output: Path, name: str,
                   vertex: str, fragment: str, samplers: list[str]) -> None:
    locations = varying_locations(vertex)
    vk_vertex = vulkan_stage(vertex, "vertex", [], locations)
    vk_fragment = vulkan_stage(fragment, "fragment", samplers, locations)
    vertex_path = output / f"dfx_{name}.vert"
    fragment_path = output / f"dfx_{name}.frag"
    vertex_path.write_text(vk_vertex, encoding="utf-8", newline="\n")
    fragment_path.write_text(vk_fragment, encoding="utf-8", newline="\n")
    common = [str(glslang), "-V", "--target-env", "vulkan1.1", "-Os"]
    run(common + [str(vertex_path), "-o", str(output / f"dfx_{name}_vert.bin")])
    run(common + [str(fragment_path), "-o", str(output / f"dfx_{name}_frag.bin")])
    vertex_path.unlink()
    fragment_path.unlink()


def validate_opengl(glslang: Path, output: Path, name: str,
                    vertex: str, fragment: str) -> None:
    vertex_path = output / f"dfx_{name}_gl.vert"
    fragment_path = output / f"dfx_{name}_gl.frag"
    vertex_path.write_text(vertex, encoding="utf-8", newline="\n")
    fragment_path.write_text(fragment, encoding="utf-8", newline="\n")
    try:
        run([str(glslang), "-l", str(vertex_path), str(fragment_path)])
    finally:
        vertex_path.unlink(missing_ok=True)
        fragment_path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path,
                        help="APK assets/shaders directory")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--glslang", required=True, type=Path)
    args = parser.parse_args()

    if not args.source.is_dir():
        parser.error(f"shader directory does not exist: {args.source}")
    if not args.glslang.is_file():
        parser.error(f"glslangValidator does not exist: {args.glslang}")
    include_output = args.output / "include"
    data_output = args.output / "data"
    include_output.mkdir(parents=True, exist_ok=True)
    data_output.mkdir(parents=True, exist_ok=True)

    sources: dict[str, tuple[str, str]] = {}
    sampler_map: dict[str, list[str]] = {}
    for spec in PROGRAMS:
        vertex, fragment, samplers = compose_program(args.source, spec)
        sources[spec.name] = (vertex, fragment)
        sampler_map[spec.name] = samplers
    write_gl_header(include_output, sources)

    for spec in PROGRAMS:
        vertex, fragment = sources[spec.name]
        validate_opengl(args.glslang, data_output, spec.name,
                        vertex, fragment)
        if spec.name in VULKAN_PROGRAMS:
            compile_vulkan(args.glslang, data_output, spec.name,
                           vertex, fragment, sampler_map[spec.name])

    shutil.copyfile(args.source / "smaa" / "AreaTexRGB.raw",
                    data_output / "drastic_smaa_area_rgb.bin")
    shutil.copyfile(args.source / "smaa" / "SearchTexRGB.raw",
                    data_output / "drastic_smaa_search_rgb.bin")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
