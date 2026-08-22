#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace gltf_validation {

struct Expectations {
  std::size_t sourceRootCount{};
  std::size_t sourceComponentCount{};
  std::size_t sourceLeafCount{};
  std::size_t requiredFallbackPartNames{};
  std::vector<std::string> requiredNodeNames;
};

struct Summary {
  std::size_t sceneCount{};
  std::size_t sceneRootCount{};
  std::size_t nodeCount{};
  std::size_t meshCount{};
  std::size_t renderableNodeCount{};
  std::size_t namedNodeCount{};
  std::size_t hierarchyEdgeCount{};
  std::size_t maxDepth{};
  std::size_t fallbackPartNameCount{};
  std::vector<std::string> nodeNames;
};

bool validate(const std::filesystem::path& output,
              const Expectations& expectations,
              Summary& summary,
              std::string& error);

}  // namespace gltf_validation
