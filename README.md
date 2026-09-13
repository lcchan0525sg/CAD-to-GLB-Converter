# CAD to GLB Convertor

A native Windows desktop application for converting STEP, IGES, and STL models to GLB, with an interactive 3D preview and assembly tree.

## Features

- Import STEP (`.step`, `.stp`), IGES (`.iges`, `.igs`), and STL (`.stl`).
- Export binary GLB (the default) or glTF with companion binary data.
- Preview original CAD part colors and inspect assembly structure.
- Preserve CAD hierarchy and validate exported files before download.
- Use optional Draco compression with a compatible OCCT build, or Meshopt compression for STL.
- Cancel conversion and save completed results through a native Windows dialog.

![Application preview from the earlier V0.31 release](docs/images/v0.31-gearbox-tree.png)

The screenshot and bundled V0.31 manual show the previous application name. The current application is named CAD to GLB Convertor.

## Run

If a portable release is available, extract its complete archive and run `run.bat` inside it. Keep the executable, runtime DLLs, `gltfpack.exe`, and `docs` folder together.

1. Browse for a CAD file or drag it onto the application.
2. Review the preview and choose the output and compression settings.
3. Click **Convert**, then **Download** to save the validated result.

The current build label is V0.34D. Output filenames default to a `-D` suffix. DWG and DXF are not supported.

## Build from source

Requirements:

- Windows x64 and Visual Studio 2022 with Desktop development with C++.
- CMake 3.20 or newer.
- An installed Open CASCADE Technology (OCCT) 8.0.1 build, including headers, libraries, runtime DLLs, and its CMake package.
- For Draco support, an OCCT build configured with Draco and the matching `draco.dll`.

meshoptimizer and gltfpack source is included in `third_party/meshoptimizer`.

Run from the repository root in PowerShell, replacing the OCCT path with your installation:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DOpenCASCADE_DIR="C:/deps/occt-8.0.1/cmake"
cmake --build build --config Release --target cad-converter2
.\build\Release\run.bat
```

For Draco, add `-DDRACO_RUNTIME_DLL="C:/deps/draco/bin/draco.dll"` to the configure command and point `OpenCASCADE_DIR` to the Draco-enabled OCCT installation. Supplying a DLL alone does not enable Draco in OCCT.

The build copies OCCT runtime files, gltfpack, and documentation beside `cad-converter2.exe`. Additional runtime dependencies of your OCCT installation must also be available. The executable retains its original filename for compatibility.

## Validation

Run the standalone output-validator tests without installing OCCT:

```powershell
cmake -S . -B build-validation -G "Visual Studio 17 2022" -A x64 -DCAD_CONVERTER_VALIDATION_ONLY=ON
cmake --build build-validation --config Release
ctest --test-dir build-validation -C Release --output-on-failure
```

GitHub Actions runs these tests on Windows. This checks output validation; it does not build or exercise the CAD importer or desktop UI.

For the real STEP/IGES/STL regression suite, install Python 3.10 or newer and provide the local fixtures described in [Validation](docs/VALIDATION.md):

```powershell
python scripts/run_regression.py --exe build/Release/cad-converter2.exe --fixtures C:/cad-samples
```

## Documentation and publishing

- [User manual (V0.31)](docs/CAD-Converter-2-V0.31-User-Manual.md)
- [Changelog](CHANGELOG.md)
- [GitHub publishing guide](docs/PUBLISHING.md)
- [Third-party notices](docs/THIRD-PARTY-NOTICES.md)

## Copyright and dependencies

Application code and documentation are available under the [MIT License](LICENSE). Existing R Innovation copyright notices are retained. Included third-party components retain their respective licenses; see the notices above.
