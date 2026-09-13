# CAD to GLB Convertor validation

## Runtime output gate

Every conversion is validated before the Download button is enabled.

The validator checks:

- GLB magic, version, declared length, and JSON chunk bounds
- Parseable GLTF JSON
- Non-empty scenes, nodes, and meshes
- Valid scene-root, child-node, and mesh indexes
- Reachable renderable geometry and an acyclic node graph
- STEP/IGES root, component, leaf-part, hierarchy-edge, and depth preservation
- Preservation of meaningful STEP node names
- Preservation of generated IGES `Part 001`-style names

A failed check deletes the temporary result through the normal conversion-failure path and reports `output validation failed: ...` in the UI. Download remains unavailable.

## Real-format regression verification

The repeatable regression runner uses the production converter path and these local fixtures:

- `GearBox.stp`
- `abc-00000050.step`
- `GearBox.igs`
- `Menger_sponge_sample.stl`

By default it reads them from the sibling `cad-viewer-web/cad-samples` directory. CAD fixtures remain local and are not committed to this repository.

Run:

```bash
python scripts/run_regression.py
```

Optional overrides:

```bash
python scripts/run_regression.py \
  --exe C:/path/to/cad-converter2.exe \
  --fixtures C:/path/to/cad-samples \
  --output C:/path/to/results \
  --keep
```

The runner verifies:

- STEP: root, 45 component meshes, hierarchy, GearBox name, and basic colors
- Simple STEP: valid root and renderable mesh without false assembly rejection
- IGES: root, 45 component meshes, hierarchy, 45 fallback part names, and basic colors
- STL: valid scene root and renderable mesh

Temporary outputs are deleted after a passing or failing run unless `--keep` is supplied.
