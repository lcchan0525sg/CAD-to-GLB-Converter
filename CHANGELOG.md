# Changelog

## Unreleased

- Renamed the application to CAD to GLB Convertor in the window, heading, Help dialogs, and launcher.
- Clarified supported STEP, IGES, and STL inputs in the interface; GLB remains the default output.

## V0.34D

- Routes STEP and IGES previews through OCCT XCAF so original per-part CAD colors are visible.
- Keeps the native blue OpenGL preview path for STL input.
- Preserves V0.33D validation, assembly checks, and compact layout behavior.

## V0.33D

- Made V0.33D the maintained main build and default CMake configuration.
- Replaced pre-conversion output-path controls with one post-conversion Download button.
- Consolidated Convert, Cancel, and Download into one action row and collapsed the hidden result area so the preview uses the available space.
- Added temporary output staging and native Windows Save-dialog export.
- Added overwrite handling and staged-file cleanup.
- Added phase-based conversion progress reporting.
- Added real cancellation through OCCT progress ranges and cancelable `gltfpack.exe` execution.
- Added a post-export integrity gate that blocks Download for malformed, empty, cyclic, flattened, or name-losing GLB/GLTF output.
- Added repeatable real-file regression verification for GearBox STEP/IGES and Menger STL, including hierarchy, names, and basic colors.
- Preserved the Draco `-D` output suffix convention.

## V0.32D

- Added the Draco build label and `-D` output filename suffix.
- Added Draco-enabled OCCT packaging support.
