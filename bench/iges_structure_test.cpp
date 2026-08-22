#include "xcaf_structure.hxx"

#include <IGESCAFControl_Reader.hxx>
#include <TDataStd_Name.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int wmain(int argc, wchar_t** argv) {
  if (argc != 2) {
    std::wcerr << L"usage: iges-structure-test <fixture.igs>\n";
    return 2;
  }

  const std::filesystem::path gltfTest =
      std::filesystem::temp_directory_path() / L"cad-converter-iges-name-test.gltf";
  {
    std::ofstream output(gltfTest);
    output << R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"name":"GearBox","children":[1,2]},{"name":"=>[0:1:1:1]","mesh":0},{"name":"DEFAULT","mesh":1}],"meshes":[{"name":"=>[0:1:1:1]"},{"name":"DEFAULT"}]})";
  }
  std::string renameError;
  if (!xcaf_structure::renameFallbackGltfNodes(gltfTest, renameError)) {
    std::cerr << renameError << '\n';
    return 4;
  }
  std::ifstream renamedInput(gltfTest);
  const std::string renamedJson((std::istreambuf_iterator<char>(renamedInput)),
                                std::istreambuf_iterator<char>());
  renamedInput.close();
  std::filesystem::remove(gltfTest);
  if (renamedJson.find("Part 001") == std::string::npos ||
      renamedJson.find("Part 002") == std::string::npos) {
    std::cerr << "fallback names missing from GLTF\n";
    return 5;
  }

  Handle(TDocStd_Document) document;
  XCAFApp_Application::GetApplication()->NewDocument(
      TCollection_ExtendedString("MDTV-XCAF"), document);
  IGESCAFControl_Reader reader;
  reader.SetColorMode(true);
  reader.SetNameMode(true);
  if (!reader.Perform(argv[1], document)) {
    std::wcerr << L"IGES import failed\n";
    return 3;
  }

  xcaf_structure::normalizeIgesAssembly(document, L"GearBox");
  const xcaf_structure::Summary summary = xcaf_structure::summarize(document);
  const xcaf_structure::AssemblySignature signature =
      xcaf_structure::assemblySignature(document);

  const Handle(XCAFDoc_ShapeTool) shapeTool =
      XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  const std::wstring rootName = roots.Length() == 1
      ? xcaf_structure::labelName(roots.Value(1)) : L"";

  std::wcout << L"roots=" << summary.rootCount
             << L" components=" << summary.componentCount
             << L" named=" << summary.namedComponentCount
             << L" root=" << rootName << L"\n";

  if (summary.rootCount != 1) return 10;
  if (summary.componentCount != 45) return 11;
  if (summary.namedComponentCount != 45) return 12;
  if (rootName != L"GearBox") return 13;
  if (signature.rootCount != 1 || signature.componentCount != 45 ||
      signature.leafCount != 45) return 14;
  if (signature.meaningfulNames.size() != 1 ||
      signature.meaningfulNames.front() != L"GearBox") return 15;
  return 0;
}
