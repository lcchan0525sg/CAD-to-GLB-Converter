#pragma once

#include <cstdint>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace native_stl {
namespace fs = std::filesystem;

struct Triangle {
  float normal[3];
  float vertices[9];
};

#pragma pack(push, 1)
struct BinaryTriangle {
  float normal[3];
  float vertices[9];
  std::uint16_t attribute;
};
#pragma pack(pop)

inline std::vector<Triangle> read(const fs::path& input) {
  std::ifstream file(input, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open STL");
  char header[80]{};
  file.read(header, sizeof(header));
  std::uint32_t count = 0;
  file.read(reinterpret_cast<char*>(&count), sizeof(count));
  const auto size = fs::file_size(input);
  if (size == 84ull + static_cast<std::uint64_t>(count) * 50ull) {
    std::vector<Triangle> triangles(count);
    for (auto& triangle : triangles) {
      BinaryTriangle raw{};
      file.read(reinterpret_cast<char*>(&raw), sizeof(raw));
      if (!file) throw std::runtime_error("truncated binary STL");
      std::memcpy(triangle.normal, raw.normal, sizeof(raw.normal));
      std::memcpy(triangle.vertices, raw.vertices, sizeof(raw.vertices));
    }
    return triangles;
  }
  file.close();
  std::ifstream ascii(input);
  std::vector<Triangle> triangles;
  std::string line;
  Triangle current{};
  int vertexCount = 0;
  while (std::getline(ascii, line)) {
    std::istringstream tokens(line);
    std::string keyword;
    tokens >> keyword;
    if (keyword == "facet") {
      std::string normalKeyword;
      tokens >> normalKeyword >> current.normal[0] >> current.normal[1] >> current.normal[2];
      vertexCount = 0;
    } else if (keyword == "vertex") {
      if (vertexCount >= 3) throw std::runtime_error("invalid ASCII STL facet");
      tokens >> current.vertices[vertexCount * 3]
             >> current.vertices[vertexCount * 3 + 1]
             >> current.vertices[vertexCount * 3 + 2];
      ++vertexCount;
    } else if (keyword == "endfacet") {
      if (vertexCount != 3) throw std::runtime_error("invalid ASCII STL facet");
      triangles.push_back(current);
    }
  }
  if (triangles.empty()) throw std::runtime_error("no triangles found in STL");
  return triangles;
}

inline std::vector<Triangle> cleanup(const std::vector<Triangle>& triangles) {
  std::vector<Triangle> result;
  result.reserve(triangles.size());
  std::unordered_set<std::string> seen;
  seen.reserve(triangles.size());
  for (const Triangle& triangle : triangles) {
    const float* a = triangle.vertices;
    const float* b = triangle.vertices + 3;
    const float* c = triangle.vertices + 6;
    const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const float vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const float cx = uy * vz - uz * vy;
    const float cy = uz * vx - ux * vz;
    const float cz = ux * vy - uy * vx;
    if (cx * cx + cy * cy + cz * cz <= 1.0e-20f) continue;

    std::array<std::array<float, 3>, 3> sorted{{
        {{a[0], a[1], a[2]}}, {{b[0], b[1], b[2]}}, {{c[0], c[1], c[2]}}}};
    std::sort(sorted.begin(), sorted.end());
    const std::string key(reinterpret_cast<const char*>(sorted.data()), sizeof(sorted));
    if (!seen.insert(key).second) continue;
    result.push_back(triangle);
  }
  return result;
}

inline std::uint32_t padded(std::uint32_t value) { return (value + 3u) & ~3u; }

inline std::string padJson(std::string json) {
  while (json.size() % 4 != 0) json.push_back(' ');
  return json;
}

inline void writeGlb(const fs::path& output, const std::vector<Triangle>& triangles) {
  std::vector<float> positions;
  std::vector<float> normals;
  positions.reserve(triangles.size() * 9);
  normals.reserve(triangles.size() * 9);
  for (const auto& triangle : triangles) {
    positions.insert(positions.end(), triangle.vertices, triangle.vertices + 9);
    for (int i = 0; i < 3; ++i) normals.insert(normals.end(), triangle.normal, triangle.normal + 3);
  }
  const std::uint32_t positionBytes = static_cast<std::uint32_t>(positions.size() * sizeof(float));
  const std::uint32_t normalOffset = padded(positionBytes);
  const std::uint32_t normalBytes = static_cast<std::uint32_t>(normals.size() * sizeof(float));
  const std::uint32_t indexOffset = padded(normalOffset + normalBytes);
  const std::uint32_t indexCount = static_cast<std::uint32_t>(triangles.size() * 3);
  const std::uint32_t indexBytes = indexCount * sizeof(std::uint32_t);
  const std::uint32_t binBytes = padded(indexOffset + indexBytes);
  const std::string json = padJson(
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},\"indices\":2}]}],"
      "\"buffers\":[{\"byteLength\":" + std::to_string(binBytes) + "}],\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" + std::to_string(positionBytes) + "},"
      "{\"buffer\":0,\"byteOffset\":" + std::to_string(normalOffset) + ",\"byteLength\":" + std::to_string(normalBytes) + "},"
      "{\"buffer\":0,\"byteOffset\":" + std::to_string(indexOffset) + ",\"byteLength\":" + std::to_string(indexBytes) + ",\"target\":34963}],"
      "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":" + std::to_string(indexCount) + ",\"type\":\"VEC3\"},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(indexCount) + ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5125,\"count\":" + std::to_string(indexCount) + ",\"type\":\"SCALAR\"}]}");
  std::vector<std::uint8_t> binary(binBytes, 0);
  std::memcpy(binary.data(), positions.data(), positionBytes);
  std::memcpy(binary.data() + normalOffset, normals.data(), normalBytes);
  auto* indices = reinterpret_cast<std::uint32_t*>(binary.data() + indexOffset);
  for (std::uint32_t i = 0; i < indexCount; ++i) indices[i] = i;
  const std::uint32_t total = 12 + 8 + static_cast<std::uint32_t>(json.size()) + 8 + binBytes;
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  const std::uint32_t magic = 0x46546C67, version = 2, jsonType = 0x4E4F534A, binType = 0x004E4942;
  file.write(reinterpret_cast<const char*>(&magic), 4);
  file.write(reinterpret_cast<const char*>(&version), 4);
  file.write(reinterpret_cast<const char*>(&total), 4);
  const std::uint32_t jsonSize = static_cast<std::uint32_t>(json.size());
  file.write(reinterpret_cast<const char*>(&jsonSize), 4);
  file.write(reinterpret_cast<const char*>(&jsonType), 4);
  file.write(json.data(), json.size());
  file.write(reinterpret_cast<const char*>(&binBytes), 4);
  file.write(reinterpret_cast<const char*>(&binType), 4);
  file.write(reinterpret_cast<const char*>(binary.data()), binary.size());
  if (!file) throw std::runtime_error("native STL GLB write failed");
}
}
