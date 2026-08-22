#include "gltf_validation.hxx"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void appendU32(std::vector<char>& bytes, std::uint32_t value) {
  const char* raw = reinterpret_cast<const char*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

fs::path writeGlb(const std::wstring& name, std::string json,
                  std::uint32_t declaredLengthOverride = 0) {
  while (json.size() % 4 != 0) json.push_back(' ');
  std::vector<char> bytes;
  appendU32(bytes, 0x46546C67);
  appendU32(bytes, 2);
  appendU32(bytes, declaredLengthOverride == 0
      ? static_cast<std::uint32_t>(20 + json.size()) : declaredLengthOverride);
  appendU32(bytes, static_cast<std::uint32_t>(json.size()));
  appendU32(bytes, 0x4E4F534A);
  bytes.insert(bytes.end(), json.begin(), json.end());
  const fs::path path = fs::temp_directory_path() / fs::path(name);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return path;
}

bool expectValid(const fs::path& path, const gltf_validation::Expectations& expectations,
                 const char* label) {
  gltf_validation::Summary summary;
  std::string error;
  const bool valid = gltf_validation::validate(path, expectations, summary, error);
  fs::remove(path);
  if (!valid) {
    std::cerr << label << " unexpectedly failed: " << error << '\n';
    return false;
  }
  return true;
}

bool expectInvalid(const fs::path& path, const gltf_validation::Expectations& expectations,
                   const std::string& expectedError, const char* label) {
  gltf_validation::Summary summary;
  std::string error;
  const bool valid = gltf_validation::validate(path, expectations, summary, error);
  fs::remove(path);
  if (valid || error.find(expectedError) == std::string::npos) {
    std::cerr << label << " expected error containing '" << expectedError
              << "' but got '" << error << "'\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  int failures = 0;
  const std::string preserved =
      R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
      R"("nodes":[{"name":"Root","children":[1,2]},{"name":"Part A","mesh":0},{"name":"Part B","mesh":1}],)"
      R"("meshes":[{"primitives":[{}]},{"primitives":[{}]}]})";

  gltf_validation::Expectations assembly;
  assembly.sourceRootCount = 1;
  assembly.sourceComponentCount = 2;
  assembly.sourceLeafCount = 2;
  assembly.requiredNodeNames = {"Root", "Part A", "Part B"};
  if (!expectValid(writeGlb(L"cad-validator-valid.glb", preserved), assembly,
                   "preserved assembly")) ++failures;

  const fs::path wrongLength = writeGlb(L"cad-validator-length.glb", preserved, 999);
  if (!expectInvalid(wrongLength, assembly, "declared length", "declared length")) ++failures;

  const std::string emptyMeshes =
      R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"name":"Root"}],"meshes":[]})";
  if (!expectInvalid(writeGlb(L"cad-validator-empty.glb", emptyMeshes), {},
                     "no meshes", "empty mesh output")) ++failures;

  const std::string invalidChild =
      R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"children":[7],"mesh":0}],"meshes":[{}]})";
  if (!expectInvalid(writeGlb(L"cad-validator-index.glb", invalidChild), {},
                     "child index", "invalid child index")) ++failures;

  const std::string malformedJson =
      R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{}],})";
  if (!expectInvalid(writeGlb(L"cad-validator-json.glb", malformedJson), {},
                     "invalid GLTF JSON", "malformed JSON")) ++failures;

  const std::string disconnectedCycle =
      R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0},{"children":[1]}],"meshes":[{}]})";
  if (!expectInvalid(writeGlb(L"cad-validator-cycle.glb", disconnectedCycle), {},
                     "cycle", "disconnected hierarchy cycle")) ++failures;

  const std::string flattened =
      R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"name":"Combined","mesh":0}],"meshes":[{}]})";
  if (!expectInvalid(writeGlb(L"cad-validator-flat.glb", flattened), assembly,
                     "component nodes", "flattened assembly")) ++failures;

  gltf_validation::Expectations fallback;
  fallback.sourceRootCount = 1;
  fallback.sourceComponentCount = 2;
  fallback.sourceLeafCount = 2;
  fallback.requiredFallbackPartNames = 2;
  if (!expectInvalid(writeGlb(L"cad-validator-names.glb", preserved), fallback,
                     "fallback part names", "missing IGES fallback names")) ++failures;

  if (failures != 0) return 1;
  std::cout << "gltf validation tests passed\n";
  return 0;
}
