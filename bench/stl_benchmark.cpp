#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Message_ProgressRange.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_Sequence.hxx>
#include <RWGltf_CafWriter.hxx>
#include <RWGltf_DracoParameters.hxx>
#include <StlAPI_Reader.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDocStd_Document.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <Quantity_Color.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <Poly_Triangulation.hxx>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

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

static std::vector<Triangle> readStl(const fs::path& input) {
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

static std::string padJson(std::string json) {
  while (json.size() % 4 != 0) json.push_back(' ');
  return json;
}

static std::uint32_t padded(std::uint32_t value) { return (value + 3u) & ~3u; }

static void writeNativeGlb(const fs::path& output, const std::vector<Triangle>& triangles) {
  std::vector<float> positions;
  std::vector<float> normals;
  positions.reserve(triangles.size() * 9);
  normals.reserve(triangles.size() * 9);
  for (const auto& triangle : triangles) {
    positions.insert(positions.end(), triangle.vertices, triangle.vertices + 9);
    for (int i = 0; i < 3; ++i) {
      normals.insert(normals.end(), triangle.normal, triangle.normal + 3);
    }
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
      "{\"bufferView\":2,\"componentType\":5125,\"count\":" + std::to_string(indexCount) + ",\"type\":\"SCALAR\"}]}" );

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
  if (!file) throw std::runtime_error("native GLB write failed");
}

static std::uint64_t writeOcctGlb(const fs::path& input, const fs::path& output) {
  Handle(TDocStd_Document) document;
  XCAFApp_Application::GetApplication()->NewDocument(TCollection_ExtendedString("MDTV-XCAF"), document);
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  StlAPI_Reader reader;
  if (!reader.Read(compound, input.string().c_str())) throw std::runtime_error("OCCT STL read failed");
  const Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
  const Handle(XCAFDoc_ColorTool) colorTool = XCAFDoc_DocumentTool::ColorTool(document->Main());
  const TDF_Label label = shapeTool->NewShape();
  shapeTool->SetShape(label, compound);
  colorTool->SetColor(label, Quantity_Color(0.72, 0.76, 0.84, Quantity_TOC_RGB), XCAFDoc_ColorGen);
  BRepMesh_IncrementalMesh mesh(compound, 1.0, Standard_False, 1.0, Standard_True);
  if (!mesh.IsDone()) throw std::runtime_error("OCCT mesh failed");
  std::uint64_t polygonCount = 0;
  for (TopExp_Explorer faces(compound, TopAbs_FACE); faces.More(); faces.Next()) {
    TopLoc_Location location;
    const Handle(Poly_Triangulation) triangulation =
        BRep_Tool::Triangulation(TopoDS::Face(faces.Current()), location);
    if (!triangulation.IsNull()) polygonCount += triangulation->NbTriangles();
  }
  RWGltf_CafWriter writer(TCollection_AsciiString(output.string().c_str()), true);
  writer.SetMergeFaces(true);
  writer.SetSplitIndices16(true);
  writer.SetParallel(true);
  RWGltf_DracoParameters draco;
  draco.DracoCompression = false;
  writer.SetCompressionParameters(draco);
  NCollection_IndexedDataMap<TCollection_AsciiString, TCollection_AsciiString> metadata;
  if (!writer.Perform(document, metadata, Message_ProgressRange())) throw std::runtime_error("OCCT GLB write failed");
  return polygonCount;
}

static bool validGlb(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::uint32_t magic = 0;
  file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  return file && magic == 0x46546C67 && fs::file_size(path) > 20;
}

template <typename Function>
static std::vector<double> benchmark(Function&& function, const fs::path& output, int iterations) {
  std::vector<double> times;
  for (int i = 0; i < iterations + 1; ++i) {
    fs::remove(output);
    const auto start = Clock::now();
    function(output);
    const auto end = Clock::now();
    if (!validGlb(output)) throw std::runtime_error("invalid GLB output");
    if (i > 0) times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
  }
  return times;
}

static void printStats(const char* name, std::vector<double> values) {
  std::sort(values.begin(), values.end());
  double sum = 0;
  for (double value : values) sum += value;
  std::cout << name << ",runs=" << values.size()
            << ",min_ms=" << std::fixed << std::setprecision(3) << values.front()
            << ",median_ms=" << values[values.size() / 2]
            << ",avg_ms=" << sum / values.size() << "\n";
}

int main(int argc, char** argv) {
  try {
    if (argc < 2) throw std::runtime_error("usage: stl-benchmark <binary.stl> [runs]");
    const fs::path input = fs::absolute(argv[1]);
    const int runs = argc > 2 ? std::max(3, std::stoi(argv[2])) : 7;
    const auto triangles = readStl(input);
    const fs::path nativeOutput = input.parent_path() / "benchmark-native.glb";
    const fs::path occtOutput = input.parent_path() / "benchmark-occt.glb";
    std::cout << "fixture=" << input.string() << "\nbytes=" << fs::file_size(input)
              << "\ntriangles=" << triangles.size() << "\nruns=" << runs << "\n";
    std::uint64_t occtPolygonCount = 0;
    auto native = benchmark([&](const fs::path& output) {
      writeNativeGlb(output, readStl(input));
    }, nativeOutput, runs);
    auto occt = benchmark([&](const fs::path& output) {
      occtPolygonCount = writeOcctGlb(input, output);
    }, occtOutput, runs);
    std::cout << "native_output_polygons=" << triangles.size() << "\n"
              << "occt_output_polygons=" << occtPolygonCount << "\n"
              << "occt_polygon_delta="
              << static_cast<std::int64_t>(occtPolygonCount) - static_cast<std::int64_t>(triangles.size())
              << "\n";
    printStats("native_direct", native);
    printStats("occt", occt);
    std::cout << "speedup_occt_over_native=" << (occt[occt.size() / 2] / native[native.size() / 2]) << "x\n";
    fs::remove(nativeOutput);
    fs::remove(occtOutput);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ERROR: " << error.what() << "\n";
    return 1;
  }
}
