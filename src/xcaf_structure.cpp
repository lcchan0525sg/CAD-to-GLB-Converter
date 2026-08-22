#include "xcaf_structure.hxx"

#include <NCollection_Sequence.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDataStd_Name.hxx>
#include <TopoDS_Iterator.hxx>
#include <XCAFDoc_DocumentTool.hxx>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <vector>

namespace xcaf_structure {
namespace {

std::wstring trim(std::wstring value) {
  const auto notSpace = [](wchar_t character) { return !std::iswspace(character); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
  value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
  return value;
}

std::wstring upper(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t character) { return std::towupper(character); });
  return value;
}

bool isPlaceholderUtf8(const std::string& rawName) {
  std::string name = rawName;
  name.erase(name.begin(), std::find_if(name.begin(), name.end(),
      [](unsigned char character) { return !std::isspace(character); }));
  std::transform(name.begin(), name.end(), name.begin(),
      [](unsigned char character) { return static_cast<char>(std::toupper(character)); });
  return name.empty() || name == "DEFAULT" || name.rfind("=>[", 0) == 0 ||
         name.rfind("=> [", 0) == 0;
}

std::string numberedPartName(std::size_t index) {
  std::ostringstream name;
  name << "Part " << std::setfill('0') << std::setw(3) << index;
  return name.str();
}

struct JsonEdit {
  std::size_t offset{};
  std::size_t length{};
  std::string replacement;
};

bool findArrayRange(const std::string& json, const std::string& key,
                    std::size_t& arrayStart, std::size_t& arrayEnd) {
  const std::string quotedKey = '"' + key + '"';
  std::size_t keyPosition = 0;
  while ((keyPosition = json.find(quotedKey, keyPosition)) != std::string::npos) {
    const std::size_t colon = json.find(':', keyPosition + quotedKey.size());
    if (colon == std::string::npos) return false;
    const std::size_t candidate = json.find('[', colon + 1);
    if (candidate == std::string::npos) return false;
    const std::size_t firstValue = json.find_first_not_of(" \t\r\n", candidate + 1);
    if (firstValue != std::string::npos &&
        (json[firstValue] == '{' || json[firstValue] == ']')) {
      arrayStart = candidate;
      break;
    }
    keyPosition += quotedKey.size();
  }
  if (keyPosition == std::string::npos) return false;

  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (std::size_t index = arrayStart; index < json.size(); ++index) {
    const char character = json[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (character == '\\') escaped = true;
      else if (character == '"') inString = false;
      continue;
    }
    if (character == '"') inString = true;
    else if (character == '[') ++depth;
    else if (character == ']' && --depth == 0) {
      arrayEnd = index;
      return true;
    }
  }
  return false;
}

std::vector<std::pair<std::size_t, std::size_t>> arrayObjectRanges(
    const std::string& json, std::size_t arrayStart, std::size_t arrayEnd) {
  std::vector<std::pair<std::size_t, std::size_t>> ranges;
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  std::size_t objectStart = 0;
  for (std::size_t index = arrayStart + 1; index < arrayEnd; ++index) {
    const char character = json[index];
    if (inString) {
      if (escaped) escaped = false;
      else if (character == '\\') escaped = true;
      else if (character == '"') inString = false;
      continue;
    }
    if (character == '"') inString = true;
    else if (character == '{') {
      if (depth++ == 0) objectStart = index;
    } else if (character == '}' && --depth == 0) {
      ranges.emplace_back(objectStart, index + 1);
    }
  }
  return ranges;
}

bool rewriteJsonNodeNames(std::string& json, std::string& error) {
  std::size_t nodesStart = 0;
  std::size_t nodesEnd = 0;
  if (!findArrayRange(json, "nodes", nodesStart, nodesEnd)) {
    error = "GLTF has no nodes array";
    return false;
  }

  const std::regex namePattern(R"json("name"\s*:\s*"([^"]*)")json");
  const std::regex meshPattern(R"json("mesh"\s*:\s*([0-9]+))json");
  std::vector<JsonEdit> edits;
  std::map<std::size_t, std::string> meshNames;
  std::size_t partIndex = 1;
  for (const auto& range : arrayObjectRanges(json, nodesStart, nodesEnd)) {
    const std::string object = json.substr(range.first, range.second - range.first);
    std::smatch nameMatch;
    const bool hasName = std::regex_search(object, nameMatch, namePattern);
    if (hasName && !isPlaceholderUtf8(nameMatch[1].str())) continue;

    const std::string fallback = numberedPartName(partIndex++);
    if (hasName) {
      const std::size_t valueOffset = range.first + static_cast<std::size_t>(nameMatch.position(1));
      edits.push_back({valueOffset, static_cast<std::size_t>(nameMatch.length(1)), fallback});
    } else {
      edits.push_back({range.first + 1, 0, "\"name\":\"" + fallback + "\","});
    }

    std::smatch meshMatch;
    if (std::regex_search(object, meshMatch, meshPattern)) {
      meshNames[static_cast<std::size_t>(std::stoull(meshMatch[1].str()))] = fallback;
    }
  }

  std::size_t meshesStart = 0;
  std::size_t meshesEnd = 0;
  if (findArrayRange(json, "meshes", meshesStart, meshesEnd)) {
    const auto meshRanges = arrayObjectRanges(json, meshesStart, meshesEnd);
    for (const auto& entry : meshNames) {
      if (entry.first >= meshRanges.size()) continue;
      const auto& range = meshRanges[entry.first];
      const std::string object = json.substr(range.first, range.second - range.first);
      std::smatch nameMatch;
      const bool hasName = std::regex_search(object, nameMatch, namePattern);
      if (hasName && !isPlaceholderUtf8(nameMatch[1].str())) continue;
      if (hasName) {
        edits.push_back({range.first + static_cast<std::size_t>(nameMatch.position(1)),
                         static_cast<std::size_t>(nameMatch.length(1)), entry.second});
      } else {
        edits.push_back({range.first + 1, 0, "\"name\":\"" + entry.second + "\","});
      }
    }
  }

  std::sort(edits.begin(), edits.end(),
            [](const JsonEdit& left, const JsonEdit& right) { return left.offset > right.offset; });
  for (const JsonEdit& edit : edits) {
    json.replace(edit.offset, edit.length, edit.replacement);
  }
  return true;
}

std::uint32_t readUint32(const std::vector<char>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

void appendUint32(std::vector<char>& bytes, std::uint32_t value) {
  const char* raw = reinterpret_cast<const char*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

void summarizeComponents(const TDF_Label& assembly, Summary& summary) {
  NCollection_Sequence<TDF_Label> components;
  if (XCAFDoc_ShapeTool::GetComponents(assembly, components, false)) {
    for (Standard_Integer index = 1; index <= components.Length(); ++index) {
      const TDF_Label component = components.Value(index);
      ++summary.componentCount;
      ++summary.namedComponentCount;
      TDF_Label referred;
      if (XCAFDoc_ShapeTool::GetReferredShape(component, referred) &&
          XCAFDoc_ShapeTool::IsAssembly(referred)) {
        summarizeComponents(referred, summary);
      }
    }
    return;
  }
  const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(assembly);
  for (TopoDS_Iterator children(shape); children.More(); children.Next()) {
    ++summary.componentCount;
    ++summary.namedComponentCount;
  }
}

}  // namespace

bool isPlaceholderName(const std::wstring& rawName) {
  const std::wstring name = upper(trim(rawName));
  return name.empty() || name == L"DEFAULT" || name.rfind(L"=>[", 0) == 0 ||
         name.rfind(L"=> [", 0) == 0;
}

std::wstring labelName(const TDF_Label& label) {
  Handle(TDataStd_Name) name;
  if (!label.IsNull() && label.FindAttribute(TDataStd_Name::GetID(), name) &&
      !name->Get().IsEmpty()) return name->Get().ToWideString();
  return L"";
}

void normalizeIgesAssembly(const Handle(TDocStd_Document)& document,
                           const std::wstring& rootFallbackName) {
  if (document.IsNull()) return;
  const Handle(XCAFDoc_ShapeTool) shapeTool =
      XCAFDoc_DocumentTool::ShapeTool(document->Main());
  if (shapeTool.IsNull()) return;
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  for (Standard_Integer index = 1; index <= roots.Length(); ++index) {
    const TDF_Label root = roots.Value(index);
    if (!isPlaceholderName(labelName(root))) continue;
    std::wstring fallback = rootFallbackName;
    if (roots.Length() > 1) fallback += L" Assembly " + std::to_wstring(index);
    TDataStd_Name::Set(root, TCollection_ExtendedString(fallback.c_str()));
  }
}

Summary summarize(const Handle(TDocStd_Document)& document) {
  Summary summary;
  if (document.IsNull()) return summary;
  const Handle(XCAFDoc_ShapeTool) shapeTool =
      XCAFDoc_DocumentTool::ShapeTool(document->Main());
  if (shapeTool.IsNull()) return summary;
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  summary.rootCount = static_cast<std::size_t>(roots.Length());
  for (Standard_Integer index = 1; index <= roots.Length(); ++index) {
    summarizeComponents(roots.Value(index), summary);
  }
  return summary;
}

bool renameFallbackGltfNodes(const std::filesystem::path& output, std::string& error) {
  try {
    std::ifstream input(output, std::ios::binary);
    if (!input) {
      error = "cannot read exported GLTF for fallback naming";
      return false;
    }
    const std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
    input.close();

    const bool isGlb = output.extension() == L".glb";
    std::string json;
    std::size_t trailingOffset = 0;
    if (isGlb) {
      constexpr std::uint32_t glbMagic = 0x46546C67;
      constexpr std::uint32_t jsonChunk = 0x4E4F534A;
      if (bytes.size() < 20 || readUint32(bytes, 0) != glbMagic ||
          readUint32(bytes, 4) != 2 || readUint32(bytes, 8) != bytes.size() ||
          readUint32(bytes, 16) != jsonChunk) {
        error = "invalid GLB header or JSON chunk";
        return false;
      }
      const std::uint32_t jsonLength = readUint32(bytes, 12);
      trailingOffset = 20 + jsonLength;
      if (trailingOffset > bytes.size()) {
        error = "invalid GLB JSON chunk length";
        return false;
      }
      json.assign(bytes.data() + 20, bytes.data() + trailingOffset);
      while (!json.empty() && (json.back() == ' ' || json.back() == '\0')) json.pop_back();
    } else {
      json.assign(bytes.begin(), bytes.end());
    }
    if (!rewriteJsonNodeNames(json, error)) return false;

    std::ofstream rewritten(output, std::ios::binary | std::ios::trunc);
    if (!rewritten) {
      error = "cannot rewrite exported GLTF fallback names";
      return false;
    }
    if (!isGlb) {
      rewritten.write(json.data(), static_cast<std::streamsize>(json.size()));
      return rewritten.good();
    }

    while (json.size() % 4 != 0) json.push_back(' ');
    const std::size_t totalSize = 20 + json.size() + (bytes.size() - trailingOffset);
    std::vector<char> result;
    result.reserve(totalSize);
    appendUint32(result, 0x46546C67);
    appendUint32(result, 2);
    appendUint32(result, static_cast<std::uint32_t>(totalSize));
    appendUint32(result, static_cast<std::uint32_t>(json.size()));
    appendUint32(result, 0x4E4F534A);
    result.insert(result.end(), json.begin(), json.end());
    result.insert(result.end(), bytes.begin() + static_cast<std::ptrdiff_t>(trailingOffset), bytes.end());
    rewritten.write(result.data(), static_cast<std::streamsize>(result.size()));
    return rewritten.good();
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

}  // namespace xcaf_structure
