# CAD Converter 2 V0.31 — Third-Party Notices

Copyright © 2026 R Innovation  
Author: Chan Lap Chi  
Contact: lcchan@r2innov.com

CAD Converter 2 includes or uses the following third-party open-source components. Their original copyright notices and license terms remain in effect.

## Open CASCADE Technology 8.0.1

Used for STEP and IGES import, XCAF document handling, CAD triangulation, OCCT GLB/GLTF export, and STEP/IGES preview.

License: GNU Lesser General Public License version 2.1 with the Open CASCADE exception.

The applicable license text is included in:

```text
docs/licenses/occt-LGPL-2.1-LICENSE.txt
```

The OCCT runtime is distributed according to the terms of its upstream distribution and build configuration.

## meshoptimizer 0.25 and gltfpack

Used for native STL cleanup, vertex optimization, quantization, polygon simplification, and Meshopt compression.

Copyright (c) 2016-2025 Arseny Kapoulkine  
License: MIT License

The applicable license text is included in:

```text
docs/licenses/meshoptimizer-MIT-LICENSE.md
```

The source is vendored in:

```text
third_party/meshoptimizer
```

The packaged `gltfpack.exe` is built from this source.

## Draco

Draco compression is optional and is available when the converter is built and packaged with a Draco-enabled OCCT runtime. The V0.34D portable package uses Draco 1.5.7, copyright the Draco authors, under the Apache License 2.0. The license is included in `docs/licenses/draco-Apache-2.0-LICENSE.txt`. Source: https://github.com/google/draco/tree/1.5.7.

## Application copyright

The CAD Converter 2 application-specific code, documentation, screenshots, and integration work are copyright © 2026 R Innovation.

Author: Chan Lap Chi  
Email: lcchan@r2innov.com
