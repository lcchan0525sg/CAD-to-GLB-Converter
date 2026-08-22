#include "gltf_validation.hxx"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace gltf_validation {
namespace {

struct JsonValue {
  enum class Type { Null, Boolean, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean{};
  double number{};
  std::string string;
  std::vector<JsonValue> array;
  std::map<std::string, JsonValue> object;
};

class JsonParser {
public:
  explicit JsonParser(const std::string& text) : text_(text) {}

  bool parse(JsonValue& value, std::string& error) {
    skipWhitespace();
    if (!parseValue(value, error)) return false;
    skipWhitespace();
    if (position_ != text_.size()) {
      error = "JSON has trailing non-whitespace data";
      return false;
    }
    return true;
  }

private:
  void skipWhitespace() {
    while (position_ < text_.size() &&
           (text_[position_] == ' ' || text_[position_] == '\t' ||
            text_[position_] == '\r' || text_[position_] == '\n' ||
            text_[position_] == '\0')) ++position_;
  }

  bool parseValue(JsonValue& value, std::string& error) {
    skipWhitespace();
    if (position_ >= text_.size()) return fail("unexpected end of JSON", error);
    const char current = text_[position_];
    if (current == '{') return parseObject(value, error);
    if (current == '[') return parseArray(value, error);
    if (current == '"') {
      value.type = JsonValue::Type::String;
      return parseString(value.string, error);
    }
    if (current == '-' || (current >= '0' && current <= '9')) return parseNumber(value, error);
    if (match("true")) {
      value.type = JsonValue::Type::Boolean;
      value.boolean = true;
      return true;
    }
    if (match("false")) {
      value.type = JsonValue::Type::Boolean;
      value.boolean = false;
      return true;
    }
    if (match("null")) {
      value.type = JsonValue::Type::Null;
      return true;
    }
    return fail("invalid JSON value", error);
  }

  bool parseObject(JsonValue& value, std::string& error) {
    value.type = JsonValue::Type::Object;
    ++position_;
    skipWhitespace();
    if (consume('}')) return true;
    for (;;) {
      std::string key;
      if (!parseString(key, error)) return false;
      skipWhitespace();
      if (!consume(':')) return fail("expected ':' after object key", error);
      JsonValue member;
      if (!parseValue(member, error)) return false;
      value.object.emplace(std::move(key), std::move(member));
      skipWhitespace();
      if (consume('}')) return true;
      if (!consume(',')) return fail("expected ',' or '}' in object", error);
      skipWhitespace();
    }
  }

  bool parseArray(JsonValue& value, std::string& error) {
    value.type = JsonValue::Type::Array;
    ++position_;
    skipWhitespace();
    if (consume(']')) return true;
    for (;;) {
      JsonValue item;
      if (!parseValue(item, error)) return false;
      value.array.push_back(std::move(item));
      skipWhitespace();
      if (consume(']')) return true;
      if (!consume(',')) return fail("expected ',' or ']' in array", error);
      skipWhitespace();
    }
  }

  static void appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7f) output.push_back(static_cast<char>(codePoint));
    else if (codePoint <= 0x7ff) {
      output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
      output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint <= 0xffff) {
      output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
      output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
      output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
      output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
      output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
  }

  bool parseHex4(std::uint32_t& value, std::string& error) {
    if (position_ + 4 > text_.size()) return fail("incomplete JSON unicode escape", error);
    value = 0;
    for (int index = 0; index < 4; ++index) {
      const char character = text_[position_++];
      value <<= 4;
      if (character >= '0' && character <= '9') value += character - '0';
      else if (character >= 'a' && character <= 'f') value += character - 'a' + 10;
      else if (character >= 'A' && character <= 'F') value += character - 'A' + 10;
      else return fail("invalid JSON unicode escape", error);
    }
    return true;
  }

  bool parseString(std::string& value, std::string& error) {
    if (!consume('"')) return fail("expected JSON string", error);
    value.clear();
    while (position_ < text_.size()) {
      const unsigned char character = static_cast<unsigned char>(text_[position_++]);
      if (character == '"') return true;
      if (character < 0x20) return fail("control character in JSON string", error);
      if (character != '\\') {
        value.push_back(static_cast<char>(character));
        continue;
      }
      if (position_ >= text_.size()) return fail("incomplete JSON escape", error);
      const char escaped = text_[position_++];
      switch (escaped) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
          std::uint32_t codePoint = 0;
          if (!parseHex4(codePoint, error)) return false;
          if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
            if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                text_[position_ + 1] != 'u') {
              return fail("missing low surrogate in JSON string", error);
            }
            position_ += 2;
            std::uint32_t low = 0;
            if (!parseHex4(low, error)) return false;
            if (low < 0xdc00 || low > 0xdfff) return fail("invalid low surrogate", error);
            codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
          }
          appendUtf8(value, codePoint);
          break;
        }
        default: return fail("invalid JSON escape", error);
      }
    }
    return fail("unterminated JSON string", error);
  }

  bool parseNumber(JsonValue& value, std::string& error) {
    const std::size_t start = position_;
    if (text_[position_] == '-') ++position_;
    if (position_ >= text_.size()) return fail("incomplete JSON number", error);
    if (text_[position_] == '0') ++position_;
    else {
      if (text_[position_] < '1' || text_[position_] > '9') return fail("invalid JSON number", error);
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
    }
    if (position_ < text_.size() && text_[position_] == '.') {
      ++position_;
      const std::size_t decimalStart = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (decimalStart == position_) return fail("invalid JSON decimal", error);
    }
    if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
      ++position_;
      if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
      const std::size_t exponentStart = position_;
      while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
      if (exponentStart == position_) return fail("invalid JSON exponent", error);
    }
    value.type = JsonValue::Type::Number;
    value.number = std::strtod(text_.substr(start, position_ - start).c_str(), nullptr);
    return std::isfinite(value.number) || fail("non-finite JSON number", error);
  }

  bool consume(char expected) {
    if (position_ >= text_.size() || text_[position_] != expected) return false;
    ++position_;
    return true;
  }

  bool match(const char* literal) {
    const std::size_t length = std::strlen(literal);
    if (text_.compare(position_, length, literal) != 0) return false;
    position_ += length;
    return true;
  }

  bool fail(const char* message, std::string& error) const {
    std::ostringstream text;
    text << message << " at byte " << position_;
    error = text.str();
    return false;
  }

  const std::string& text_;
  std::size_t position_{};
};

const JsonValue* member(const JsonValue& object, const char* key) {
  if (object.type != JsonValue::Type::Object) return nullptr;
  const auto found = object.object.find(key);
  return found == object.object.end() ? nullptr : &found->second;
}

bool indexValue(const JsonValue& value, std::size_t& index) {
  if (value.type != JsonValue::Type::Number || value.number < 0.0 ||
      std::floor(value.number) != value.number ||
      value.number > static_cast<double>(std::numeric_limits<std::size_t>::max())) return false;
  index = static_cast<std::size_t>(value.number);
  return true;
}

std::uint32_t readU32(const std::vector<char>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  if (offset + sizeof(value) <= bytes.size()) {
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
  }
  return value;
}

bool readJson(const std::filesystem::path& output, std::string& json, std::string& error) {
  std::ifstream input(output, std::ios::binary);
  if (!input) {
    error = "output file does not exist or cannot be read";
    return false;
  }
  const std::vector<char> bytes((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    error = "output file is empty";
    return false;
  }

  std::string extension = output.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  if (extension != ".glb") {
    json.assign(bytes.begin(), bytes.end());
    return true;
  }

  if (bytes.size() < 20) {
    error = "GLB is shorter than its required header and JSON chunk";
    return false;
  }
  if (readU32(bytes, 0) != 0x46546C67) {
    error = "invalid GLB magic";
    return false;
  }
  if (readU32(bytes, 4) != 2) {
    error = "unsupported GLB version";
    return false;
  }
  if (readU32(bytes, 8) != bytes.size()) {
    error = "GLB declared length does not match file size";
    return false;
  }
  if (readU32(bytes, 16) != 0x4E4F534A) {
    error = "GLB first chunk is not JSON";
    return false;
  }
  const std::size_t jsonLength = readU32(bytes, 12);
  if (jsonLength == 0 || jsonLength > bytes.size() - 20) {
    error = "GLB JSON chunk length is invalid";
    return false;
  }
  json.assign(bytes.data() + 20, bytes.data() + 20 + jsonLength);
  while (!json.empty() && (json.back() == ' ' || json.back() == '\0')) json.pop_back();
  return true;
}

bool requireArray(const JsonValue& root, const char* name, const JsonValue*& array,
                  std::string& error) {
  array = member(root, name);
  if (!array || array->type != JsonValue::Type::Array) {
    error = std::string("GLTF has no ") + name + " array";
    return false;
  }
  return true;
}

bool isFallbackPartName(const std::string& name) {
  if (name.size() < 8 || name.rfind("Part ", 0) != 0) return false;
  return std::all_of(name.begin() + 5, name.end(),
                     [](unsigned char value) { return std::isdigit(value) != 0; });
}

}  // namespace

bool validate(const std::filesystem::path& output,
              const Expectations& expectations,
              Summary& summary,
              std::string& error) {
  summary = {};
  error.clear();
  std::string json;
  if (!readJson(output, json, error)) return false;

  JsonValue root;
  JsonParser parser(json);
  if (!parser.parse(root, error)) {
    error = "invalid GLTF JSON: " + error;
    return false;
  }
  if (root.type != JsonValue::Type::Object) {
    error = "GLTF JSON root is not an object";
    return false;
  }

  const JsonValue* scenes = nullptr;
  const JsonValue* nodes = nullptr;
  const JsonValue* meshes = nullptr;
  if (!requireArray(root, "scenes", scenes, error) ||
      !requireArray(root, "nodes", nodes, error) ||
      !requireArray(root, "meshes", meshes, error)) return false;
  summary.sceneCount = scenes->array.size();
  summary.nodeCount = nodes->array.size();
  summary.meshCount = meshes->array.size();
  if (summary.sceneCount == 0) { error = "GLTF has no scenes"; return false; }
  if (summary.nodeCount == 0) { error = "GLTF has no nodes"; return false; }
  if (summary.meshCount == 0) { error = "GLTF has no meshes"; return false; }

  std::size_t activeScene = 0;
  if (const JsonValue* selected = member(root, "scene")) {
    if (!indexValue(*selected, activeScene) || activeScene >= scenes->array.size()) {
      error = "GLTF active scene index is invalid";
      return false;
    }
  }
  const JsonValue& scene = scenes->array[activeScene];
  const JsonValue* sceneNodes = member(scene, "nodes");
  if (!sceneNodes || sceneNodes->type != JsonValue::Type::Array || sceneNodes->array.empty()) {
    error = "GLTF active scene has no root nodes";
    return false;
  }

  std::vector<std::vector<std::size_t>> children(summary.nodeCount);
  std::vector<bool> renderable(summary.nodeCount, false);
  std::vector<std::size_t> roots;
  for (const JsonValue& rootIndex : sceneNodes->array) {
    std::size_t index = 0;
    if (!indexValue(rootIndex, index) || index >= summary.nodeCount) {
      error = "GLTF scene root node index is invalid";
      return false;
    }
    roots.push_back(index);
  }
  summary.sceneRootCount = roots.size();

  for (std::size_t nodeIndex = 0; nodeIndex < nodes->array.size(); ++nodeIndex) {
    const JsonValue& node = nodes->array[nodeIndex];
    if (node.type != JsonValue::Type::Object) {
      error = "GLTF node is not an object";
      return false;
    }
    if (const JsonValue* name = member(node, "name")) {
      if (name->type != JsonValue::Type::String) {
        error = "GLTF node name is not a string";
        return false;
      }
      summary.nodeNames.push_back(name->string);
      if (!name->string.empty()) {
        ++summary.namedNodeCount;
        if (isFallbackPartName(name->string)) ++summary.fallbackPartNameCount;
      }
    } else {
      summary.nodeNames.emplace_back();
    }
    if (const JsonValue* mesh = member(node, "mesh")) {
      std::size_t meshIndex = 0;
      if (!indexValue(*mesh, meshIndex) || meshIndex >= summary.meshCount) {
        error = "GLTF node mesh index is invalid";
        return false;
      }
      renderable[nodeIndex] = true;
    }
    if (const JsonValue* nodeChildren = member(node, "children")) {
      if (nodeChildren->type != JsonValue::Type::Array) {
        error = "GLTF node children is not an array";
        return false;
      }
      for (const JsonValue& child : nodeChildren->array) {
        std::size_t childIndex = 0;
        if (!indexValue(child, childIndex) || childIndex >= summary.nodeCount) {
          error = "GLTF child index is invalid";
          return false;
        }
        children[nodeIndex].push_back(childIndex);
        ++summary.hierarchyEdgeCount;
      }
    }
  }

  std::vector<int> cycleState(summary.nodeCount, 0);
  std::function<bool(std::size_t)> detectCycle;
  detectCycle = [&](std::size_t nodeIndex) {
    if (cycleState[nodeIndex] == 1) {
      error = "GLTF node hierarchy contains a cycle";
      return false;
    }
    if (cycleState[nodeIndex] == 2) return true;
    cycleState[nodeIndex] = 1;
    for (std::size_t child : children[nodeIndex]) {
      if (!detectCycle(child)) return false;
    }
    cycleState[nodeIndex] = 2;
    return true;
  };
  for (std::size_t nodeIndex = 0; nodeIndex < summary.nodeCount; ++nodeIndex) {
    if (!detectCycle(nodeIndex)) return false;
  }

  std::vector<int> state(summary.nodeCount, 0);
  std::vector<bool> reachable(summary.nodeCount, false);
  std::function<bool(std::size_t, std::size_t)> visit;
  visit = [&](std::size_t nodeIndex, std::size_t depth) {
    if (state[nodeIndex] == 1) {
      error = "GLTF node hierarchy contains a cycle";
      return false;
    }
    if (state[nodeIndex] == 2) {
      reachable[nodeIndex] = true;
      summary.maxDepth = std::max(summary.maxDepth, depth);
      return true;
    }
    state[nodeIndex] = 1;
    reachable[nodeIndex] = true;
    summary.maxDepth = std::max(summary.maxDepth, depth);
    for (std::size_t child : children[nodeIndex]) {
      if (!visit(child, depth + 1)) return false;
    }
    state[nodeIndex] = 2;
    return true;
  };
  for (std::size_t rootIndex : roots) {
    if (!visit(rootIndex, 1)) return false;
  }
  for (std::size_t index = 0; index < summary.nodeCount; ++index) {
    if (reachable[index] && renderable[index]) ++summary.renderableNodeCount;
  }
  if (summary.renderableNodeCount == 0) {
    error = "GLTF active scene has no renderable mesh nodes";
    return false;
  }

  if (expectations.sourceRootCount > 0 &&
      summary.sceneRootCount < expectations.sourceRootCount) {
    error = "GLTF has fewer assembly roots than the imported CAD document";
    return false;
  }
  if (expectations.sourceComponentCount > 0) {
    const std::size_t minimumNodes = expectations.sourceRootCount + expectations.sourceComponentCount;
    if (summary.nodeCount < minimumNodes) {
      error = "GLTF has fewer component nodes than the imported CAD assembly";
      return false;
    }
    if (summary.hierarchyEdgeCount < expectations.sourceComponentCount || summary.maxDepth < 2) {
      error = "GLTF assembly hierarchy was flattened";
      return false;
    }
  }
  if (expectations.sourceLeafCount > 0 &&
      summary.renderableNodeCount < expectations.sourceLeafCount) {
    error = "GLTF has fewer renderable part nodes than the imported CAD assembly";
    return false;
  }
  if (expectations.requiredFallbackPartNames > 0 &&
      summary.fallbackPartNameCount < expectations.requiredFallbackPartNames) {
    error = "GLTF is missing required IGES fallback part names";
    return false;
  }

  std::multiset<std::string> availableNames;
  for (const std::string& name : summary.nodeNames) {
    if (!name.empty()) availableNames.insert(name);
  }
  for (const std::string& required : expectations.requiredNodeNames) {
    const auto found = availableNames.find(required);
    if (found == availableNames.end()) {
      error = "GLTF is missing required CAD node name: " + required;
      return false;
    }
    availableNames.erase(found);
  }
  return true;
}

}  // namespace gltf_validation
