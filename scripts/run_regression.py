#!/usr/bin/env python
"""Run CAD Converter 2's real STEP/IGES/STL regression set."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def glb_summary(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    if len(data) < 20 or data[:4] != b"glTF":
        raise ValueError("invalid GLB header")
    version, declared, json_length, chunk_type = struct.unpack_from("<4I", data, 4)
    if version != 2 or declared != len(data) or chunk_type != 0x4E4F534A:
        raise ValueError("invalid GLB container")
    document = json.loads(data[20 : 20 + json_length].rstrip(b" \0"))
    nodes = document.get("nodes", [])
    meshes = document.get("meshes", [])
    scene_index = document.get("scene", 0)
    scenes = document.get("scenes", [])
    roots = scenes[scene_index].get("nodes", []) if scenes else []
    names = [node.get("name", "") for node in nodes]
    edges = sum(len(node.get("children", [])) for node in nodes)
    renderable = sum("mesh" in node for node in nodes)
    primitives = [primitive for mesh in meshes for primitive in mesh.get("primitives", [])]

    def depth(index: int, active: set[int]) -> int:
        if index in active:
            raise ValueError("cycle in GLB hierarchy")
        return 1 + max(
            (depth(child, active | {index}) for child in nodes[index].get("children", [])),
            default=0,
        )

    max_depth = max((depth(root, set()) for root in roots), default=0)
    return {
        "bytes": len(data),
        "roots": len(roots),
        "nodes": len(nodes),
        "meshes": len(meshes),
        "renderable": renderable,
        "edges": edges,
        "depth": max_depth,
        "materials": len(document.get("materials", [])),
        "colored_primitives": sum("material" in primitive for primitive in primitives),
        "names": names,
        "fallback_names": sum(
            name.startswith("Part ") and name[5:].isdigit() for name in names
        ),
    }


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--fixtures", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    exe = args.exe or repo / "build-v034d-default" / "Release" / "cad-converter2.exe"
    fixtures = args.fixtures or repo.parent / "cad-viewer-web" / "cad-samples"
    output = args.output or Path(tempfile.gettempdir()) / "cad-converter2-regression"
    cases = [
        ("STEP", fixtures / "GearBox.stp", output / "GearBox-step.glb"),
        ("STEP-SIMPLE", fixtures / "abc-00000050.step", output / "Simple-step.glb"),
        ("IGES", fixtures / "GearBox.igs", output / "GearBox-iges.glb"),
        ("STL", fixtures / "Menger_sponge_sample.stl", output / "Menger-stl.glb"),
    ]

    require(exe.is_file(), f"converter not found: {exe}")
    for _, fixture, _ in cases:
        require(fixture.is_file(), f"fixture not found: {fixture}")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    summaries: dict[str, dict[str, object]] = {}
    try:
        for format_name, source, converted in cases:
            process = subprocess.run(
                [str(exe), "--regression", str(source), str(converted)],
                check=False,
            )
            if process.returncode != 0:
                error_path = Path(str(converted) + ".error.txt")
                detail = error_path.read_text(errors="replace") if error_path.exists() else "no detail"
                raise RuntimeError(f"{format_name} conversion failed ({process.returncode}): {detail}")
            summaries[format_name] = glb_summary(converted)

        step = summaries["STEP"]
        require(step["roots"] >= 1, "STEP lost the scene root")
        require(step["nodes"] >= 46, "STEP lost component nodes")
        require(step["meshes"] >= 45, "STEP lost renderable parts")
        require(step["edges"] >= 45 and step["depth"] >= 2, "STEP hierarchy was flattened")
        require("GearBox" in step["names"], "STEP root name was lost")
        require(step["materials"] >= 1 and step["colored_primitives"] >= 1,
                "STEP basic colors were lost")

        simple_step = summaries["STEP-SIMPLE"]
        require(simple_step["roots"] >= 1, "simple STEP lost the scene root")
        require(simple_step["meshes"] >= 1 and simple_step["renderable"] >= 1,
                "simple STEP has no renderable mesh")

        iges = summaries["IGES"]
        require(iges["roots"] >= 1, "IGES lost the scene root")
        require(iges["nodes"] >= 46, "IGES lost component nodes")
        require(iges["meshes"] >= 45, "IGES lost renderable parts")
        require(iges["edges"] >= 45 and iges["depth"] >= 2, "IGES hierarchy was flattened")
        require(iges["fallback_names"] >= 45, "IGES fallback part names were lost")
        require("GearBox" in iges["names"], "IGES root name was lost")
        require(iges["materials"] >= 1 and iges["colored_primitives"] >= 1,
                "IGES basic colors were lost")

        stl = summaries["STL"]
        require(stl["roots"] >= 1, "STL lost the scene root")
        require(stl["meshes"] >= 1 and stl["renderable"] >= 1, "STL has no renderable mesh")

        for format_name, _, converted in cases:
            summary = summaries[format_name]
            print(
                f"PASS {format_name}: roots={summary['roots']} nodes={summary['nodes']} "
                f"meshes={summary['meshes']} edges={summary['edges']} depth={summary['depth']} "
                f"materials={summary['materials']} bytes={summary['bytes']} output={converted}"
            )
        print("CAD regression verification passed")
        return 0
    except Exception as exception:
        print(f"FAIL: {exception}", file=sys.stderr)
        return 1
    finally:
        if not args.keep and output.exists():
            shutil.rmtree(output)


if __name__ == "__main__":
    raise SystemExit(main())
