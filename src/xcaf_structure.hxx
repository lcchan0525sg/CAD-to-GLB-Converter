#pragma once

#include <TCollection_ExtendedString.hxx>
#include <TDF_Label.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace xcaf_structure {

struct Summary {
  std::size_t rootCount{};
  std::size_t componentCount{};
  std::size_t namedComponentCount{};
};

struct AssemblySignature {
  std::size_t rootCount{};
  std::size_t componentCount{};
  std::size_t leafCount{};
  std::vector<std::wstring> meaningfulNames;
};

bool isPlaceholderName(const std::wstring& name);
std::wstring labelName(const TDF_Label& label);
void normalizeIgesAssembly(const Handle(TDocStd_Document)& document,
                           const std::wstring& rootFallbackName);
Summary summarize(const Handle(TDocStd_Document)& document);
AssemblySignature assemblySignature(const Handle(TDocStd_Document)& document);
bool renameFallbackGltfNodes(const std::filesystem::path& output,
                             std::string& error);

}  // namespace xcaf_structure
