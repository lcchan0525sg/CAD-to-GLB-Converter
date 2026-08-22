#include <windows.h>
#include <GL/gl.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl.h>

#include <AIS_InteractiveContext.hxx>
#include <AIS_ViewController.hxx>
#include <Aspect_GradientFillMethod.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <Message_ProgressRange.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <NCollection_IndexedDataMap.hxx>
#include <NCollection_Sequence.hxx>
#include <OpenGl_GraphicDriver.hxx>
#include <RWGltf_CafWriter.hxx>
#include <RWGltf_DracoParameters.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <StlAPI_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDocStd_Document.hxx>
#include <TDataStd_Name.hxx>
#include <TDF_ChildIterator.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <Poly_Triangulation.hxx>
#include <BRepTools.hxx>
#include <Bnd_Box.hxx>
#include <Precision.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include "native_stl.hpp"
#include "gltf_validation.hxx"
#include "version.hpp"
#include "xcaf_structure.hxx"

#ifndef CAD_CONVERTER_HAS_DRACO
#define CAD_CONVERTER_HAS_DRACO 0
#endif
#include <Quantity_Color.hxx>
#include <TopoDS_Compound.hxx>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cwchar>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <set>
#include <functional>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

struct ConvertOptions {
  bool binary = true;
  bool colorsOnly = true;
  bool draco = false;
  bool meshopt = false;
  int dracoLevel = 7;
  bool optimize = true;
  std::string profile = "large";
  double deflection = 1.0;
  double angular = 1.0;
};

struct ConversionResult {
  Handle(TDocStd_Document) document;
  std::vector<native_stl::Triangle> nativeTriangles;
  std::uint64_t outputPolygons = 0;
  std::uint64_t inputPolygons = 0;
  bool useNativePreview = false;
};

struct ConversionJob {
  fs::path input;
  fs::path output;
  fs::path downloadName;
  ConvertOptions options;
  ConversionResult result;
  std::string error;
  bool success = false;
  bool cancelled = false;
  double seconds = 0.0;
  HWND window{};
  std::shared_ptr<std::atomic_bool> cancelRequested = std::make_shared<std::atomic_bool>(false);
  int lastProgressPercent = -1;
  std::wstring lastProgressPhase;
};

struct ConversionProgress {
  std::wstring text;
};

class UiProgressIndicator final : public Message_ProgressIndicator {
  DEFINE_STANDARD_RTTIEXT(UiProgressIndicator, Message_ProgressIndicator)

public:
  UiProgressIndicator(std::shared_ptr<std::atomic_bool> cancelRequested,
                      std::function<void(double)> callback)
      : cancelRequested_(std::move(cancelRequested)), callback_(std::move(callback)) {}

protected:
  bool UserBreak() override {
    return cancelRequested_ && cancelRequested_->load(std::memory_order_relaxed);
  }

  void Show(const Message_ProgressScope&, bool) override {
    if (callback_) callback_(GetPosition());
  }

private:
  std::shared_ptr<std::atomic_bool> cancelRequested_;
  std::function<void(double)> callback_;
};

IMPLEMENT_STANDARD_RTTIEXT(UiProgressIndicator, Message_ProgressIndicator)

constexpr UINT WM_APP_CONVERSION_COMPLETE = WM_APP + 101;
constexpr UINT WM_APP_CONVERSION_PROGRESS = WM_APP + 102;

static void reportProgress(ConversionJob& job, const std::wstring& phase, int percent) {
  percent = std::clamp(percent, 0, 100);
  if (job.lastProgressPercent == percent && job.lastProgressPhase == phase) return;
  job.lastProgressPercent = percent;
  job.lastProgressPhase = phase;
  auto* progress = new ConversionProgress{phase + L" (" + std::to_wstring(percent) + L"%)"};
  if (!PostMessageW(job.window, WM_APP_CONVERSION_PROGRESS, 0,
                    reinterpret_cast<LPARAM>(progress))) {
    delete progress;
  }
}

static bool cancellationRequested(const ConversionJob& job) {
  return job.cancelRequested && job.cancelRequested->load(std::memory_order_relaxed);
}

static bool failIfCancelled(ConversionJob& job, std::string& error) {
  if (!cancellationRequested(job)) return false;
  job.cancelled = true;
  error = "conversion cancelled";
  return true;
}

constexpr LRESULT kCompressionNoneIndex = 0;
#if CAD_CONVERTER_HAS_DRACO
constexpr LRESULT kCompressionDracoIndex = 1;
constexpr LRESULT kCompressionMeshoptIndex = 2;
#else
constexpr LRESULT kCompressionDracoIndex = -1;
constexpr LRESULT kCompressionMeshoptIndex = 1;
#endif

static double clampDouble(double value, double low, double high, double fallback) {
  if (!std::isfinite(value)) return fallback;
  return std::max(low, std::min(high, value));
}

static std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

static double readDouble(HWND control, double fallback, double low, double high) {
  wchar_t text[64]{};
  GetWindowTextW(control, text, static_cast<int>(std::size(text)));
  wchar_t* end = nullptr;
  const double value = std::wcstod(text, &end);
  if (end == text || *end != L'\0') return fallback;
  return clampDouble(value, low, high, fallback);
}

static void writeNumber(HWND control, double value) {
  wchar_t text[32]{};
  swprintf_s(text, L"%.2f", value);
  SetWindowTextW(control, text);
}

static fs::path executableDirectory() {
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  return length == 0 ? fs::current_path() : fs::path(std::wstring(buffer.data(), length)).parent_path();
}

static std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                        static_cast<int>(value.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (length <= 0) return {};
  std::string result(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      result.data(), length, nullptr, nullptr);
  return result;
}

static gltf_validation::Expectations validationExpectations(
    const Handle(TDocStd_Document)& document, bool expectIgesFallbackNames) {
  gltf_validation::Expectations expectations;
  const xcaf_structure::AssemblySignature signature =
      xcaf_structure::assemblySignature(document);
  expectations.sourceRootCount = signature.rootCount;
  expectations.sourceComponentCount = signature.componentCount;
  expectations.sourceLeafCount = signature.leafCount;
  if (expectIgesFallbackNames) {
    expectations.requiredFallbackPartNames = signature.componentCount;
  }
  for (const std::wstring& name : signature.meaningfulNames) {
    const std::string encoded = utf8(name);
    if (!encoded.empty()) expectations.requiredNodeNames.push_back(encoded);
  }
  return expectations;
}

static std::wstring quoted(const fs::path& path) {
  return L"\"" + path.wstring() + L"\"";
}

static bool readTriangleCount(const fs::path& report, std::uint64_t& triangleCount) {
  std::ifstream file(report);
  if (!file) return false;
  const std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  const std::size_t key = content.find("\"triangleCount\"");
  if (key == std::string::npos) return false;
  const std::size_t colon = content.find(':', key);
  if (colon == std::string::npos) return false;
  try {
    triangleCount = std::stoull(content.substr(colon + 1));
    return true;
  } catch (...) {
    return false;
  }
}

static bool readGlbJson(const fs::path& path, std::string& json) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::uint32_t magic = 0, version = 0, totalLength = 0;
  std::uint32_t chunkLength = 0, chunkType = 0;
  file.read(reinterpret_cast<char*>(&magic), 4);
  file.read(reinterpret_cast<char*>(&version), 4);
  file.read(reinterpret_cast<char*>(&totalLength), 4);
  file.read(reinterpret_cast<char*>(&chunkLength), 4);
  file.read(reinterpret_cast<char*>(&chunkType), 4);
  if (!file || magic != 0x46546C67 || version != 2 || chunkType != 0x4E4F534A) return false;
  json.resize(chunkLength);
  file.read(json.data(), chunkLength);
  return static_cast<std::uint32_t>(file.gcount()) == chunkLength;
}

static std::string gltfArray(const std::string& json, const char* key) {
  const std::size_t keyPosition = json.find(std::string("\"") + key + "\"");
  if (keyPosition == std::string::npos) return {};
  const std::size_t open = json.find('[', keyPosition);
  if (open == std::string::npos) return {};
  int depth = 0;
  bool quotedString = false;
  bool escaped = false;
  for (std::size_t i = open; i < json.size(); ++i) {
    const char c = json[i];
    if (quotedString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '\"') quotedString = false;
      continue;
    }
    if (c == '\"') quotedString = true;
    else if (c == '[') ++depth;
    else if (c == ']' && --depth == 0) return json.substr(open + 1, i - open - 1);
  }
  return {};
}

static std::size_t gltfArrayObjectCount(const std::string& array) {
  std::size_t count = 0;
  int depth = 0;
  bool quotedString = false;
  bool escaped = false;
  for (char c : array) {
    if (quotedString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '\"') quotedString = false;
      continue;
    }
    if (c == '\"') quotedString = true;
    else if (c == '{') { if (depth == 0) ++count; ++depth; }
    else if (c == '}') --depth;
  }
  return count;
}

static std::set<std::string> gltfNames(const std::string& array) {
  std::set<std::string> names;
  std::size_t position = 0;
  while ((position = array.find("\"name\"", position)) != std::string::npos) {
    const std::size_t colon = array.find(':', position + 6);
    const std::size_t firstQuote = colon == std::string::npos ? std::string::npos : array.find('\"', colon + 1);
    const std::size_t lastQuote = firstQuote == std::string::npos ? std::string::npos : array.find('\"', firstQuote + 1);
    if (lastQuote == std::string::npos) break;
    names.insert(array.substr(firstQuote + 1, lastQuote - firstQuote - 1));
    position = lastQuote + 1;
  }
  return names;
}

static bool validateSceneStructure(const fs::path& source, const fs::path& optimized) {
  std::string sourceJson, optimizedJson;
  if (!readGlbJson(source, sourceJson) || !readGlbJson(optimized, optimizedJson)) return false;
  for (const char* key : {"scenes", "nodes", "meshes"}) {
    const std::string sourceArray = gltfArray(sourceJson, key);
    const std::string optimizedArray = gltfArray(optimizedJson, key);
    if (gltfArrayObjectCount(sourceArray) != gltfArrayObjectCount(optimizedArray)) return false;
    if (gltfNames(sourceArray) != gltfNames(optimizedArray)) return false;
  }
  return true;
}

static bool runGltfpack(const fs::path& rawInput, const fs::path& output,
                        const ConvertOptions& options, std::uint64_t& outputPolygons,
                        std::string& error, bool allowSimplification = true,
                        bool preserveSceneStructure = false, ConversionJob* job = nullptr) {
  const fs::path tool = executableDirectory() / L"gltfpack.exe";
  if (!fs::is_regular_file(tool)) {
    error = "gltfpack.exe is missing beside the converter";
    return false;
  }
  fs::path report = output;
  report += L".meshopt-report.json";
  std::error_code ignored;
  fs::remove(output, ignored);
  fs::remove(report, ignored);

  double ratio = 1.0;
  double targetError = 0.01;
  bool aggressive = false;
  if (options.optimize && allowSimplification) {
    if (options.profile == "balanced") { ratio = 0.70; targetError = 0.01; aggressive = true; }
    else if (options.profile == "large") { ratio = 0.50; targetError = 0.02; aggressive = true; }
    else if (options.profile == "preview") { ratio = 0.25; targetError = 0.04; aggressive = true; }
    else if (options.profile == "custom") {
      ratio = clampDouble(1.0 / std::max(1.0, options.deflection), 0.10, 1.0, 1.0);
      targetError = clampDouble(options.angular / 100.0, 0.001, 0.05, 0.01);
      aggressive = ratio < 0.999;
    }
  }

  std::wstringstream command;
  command << quoted(tool) << L" -i " << quoted(rawInput) << L" -o " << quoted(output)
          << L" -r " << quoted(report) << L" -kn -km -ke -vp 14 -vn 8";
  if (ratio < 0.999) {
    command << L" -si " << std::fixed << std::setprecision(4) << ratio
            << L" -se " << std::fixed << std::setprecision(4) << targetError
            << L" -sp";
    if (aggressive) command << L" -sa";
  }
  if (options.meshopt) command << L" -c";

  std::wstring mutableCommand = command.str();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL started = CreateProcessW(tool.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, executableDirectory().c_str(),
                                      &startup, &process);
  if (!started) {
    error = "failed to start gltfpack.exe";
    return false;
  }
  bool cancelled = false;
  for (;;) {
    const DWORD waitResult = WaitForSingleObject(process.hProcess, 100);
    if (waitResult == WAIT_OBJECT_0) break;
    if (job && cancellationRequested(*job)) {
      cancelled = true;
      TerminateProcess(process.hProcess, ERROR_CANCELLED);
      WaitForSingleObject(process.hProcess, INFINITE);
      break;
    }
  }
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  const bool counted = readTriangleCount(report, outputPolygons);
  const bool structureValid = !preserveSceneStructure || validateSceneStructure(rawInput, output);
  fs::remove(report, ignored);
  if (cancelled && job) failIfCancelled(*job, error);
  if (cancelled || exitCode != 0 || !fs::is_regular_file(output) || !counted || !structureValid) {
    error = structureValid ? "meshoptimizer post-process failed"
                           : "meshoptimizer changed STEP/IGES assembly structure or names";
    if (cancelled && job) error = "conversion cancelled";
    fs::remove(output, ignored);
    return false;
  }
  return true;
}

struct OcctMeshQuality {
  double deflection = 0.20;
  double angular = 0.50;
  double diagonal = 0.0;
};

static OcctMeshQuality occtMeshQuality(const std::string& profile,
                                       double customDeflection,
                                       double customAngular,
                                       const NCollection_Sequence<TDF_Label>& roots) {
  Bnd_Box bounds;
  for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
    const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(roots.Value(i));
    if (!shape.IsNull()) BRepBndLib::Add(shape, bounds);
  }
  if (bounds.IsVoid()) return {customDeflection, customAngular, 0.0};
  Standard_Real xmin = 0.0, ymin = 0.0, zmin = 0.0;
  Standard_Real xmax = 0.0, ymax = 0.0, zmax = 0.0;
  bounds.Get(xmin, ymin, zmin, xmax, ymax, zmax);
  const double dx = xmax - xmin;
  const double dy = ymax - ymin;
  const double dz = zmax - zmin;
  const double diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
  double ratio = 0.0005;
  double angular = 0.25;
  if (profile == "balanced") { ratio = 0.0010; angular = 0.45; }
  else if (profile == "large") { ratio = 0.0025; angular = 0.70; }
  else if (profile == "preview") { ratio = 0.0050; angular = 1.00; }
  else if (profile == "custom") return {customDeflection, customAngular, diagonal};
  return {std::max(diagonal * ratio, Precision::Confusion() * 10.0), angular, diagonal};
}

enum ControlId {
  IDC_SELECT = 1001,
  IDC_CONVERT,
  IDC_FORMAT,
  IDC_APPEARANCE,
  IDC_COMPRESS,
  IDC_DRACO_LEVEL,
  IDC_OPTIMIZE,
  IDC_PROFILE,
  IDC_DEFLECTION,
  IDC_ANGULAR,
  IDC_MANUAL_HELP,
  IDC_CANCEL,
  IDC_DOWNLOAD,
};

class App {
public:
  explicit App(HINSTANCE instance) : instance_(instance) {}
  int run(const fs::path& autoInput = {});
  int runRegression(const fs::path& input, const fs::path& output);

private:
  static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK previewProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK nativePreviewWndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK dropProc(HWND, UINT, WPARAM, LPARAM);
  bool createWindow();
  void createControls();
  void initializePreview();
  void initializeNativePreview();
  void renderNativePreviewSoftware();
  void renderNativePreviewHardware();
  void showNativePreviewGeometry(const std::vector<native_stl::Triangle>& triangles);
  void showNativePreview(const std::vector<native_stl::Triangle>& triangles);
  std::vector<native_stl::Triangle> collectDocumentTriangles(const Handle(TDocStd_Document)& document) const;
  void resizePreview();
  void showPreview(const Handle(TDocStd_Document)& document);
  void populateAssemblyTree(const Handle(TDocStd_Document)& document);
  void clearAssemblyTree();
  bool handlePreviewMessage(HWND, UINT, WPARAM, LPARAM);
  bool handleNativePreviewMessage(HWND, UINT, WPARAM, LPARAM);
  bool paintBackground(HDC);
  HBRUSH paintControl(HDC, HWND, UINT);
  bool drawButton(const DRAWITEMSTRUCT&);
  void releaseThemeResources();
  void setStatus(const std::wstring& message);
  void updateResultLayout(bool showResults);
  void updateOptionState();
  void applyProfileValues();
  void handleCommand(WPARAM wParam);
  void chooseInput();
  void downloadOutput();
  void openHelp();
  void selectInputPath(const fs::path& path);
  void handleDropFiles(HDROP drop);
  void convertSelected();
  void cancelConversion();
  void handleConversionProgress(ConversionProgress* progress);
  void handleConversionComplete(ConversionJob* job);
  void updateConversionInfo(const fs::path& input, const fs::path& output,
                            const ConvertOptions& options,
                            const Handle(TDocStd_Document)& document,
                            std::uint64_t outputPolygonOverride = 0,
                            std::uint64_t inputPolygonOverride = 0,
                            double processingSeconds = 0.0);
  ConvertOptions optionsFromControls() const;
  bool convertCad(ConversionJob& job, const fs::path& input, const fs::path& output,
                  const ConvertOptions& options, ConversionResult& result, std::string& error);

  HINSTANCE instance_{};
  HWND window_{};
  HWND inputLabel_{};
  HWND formatCombo_{};
  HWND appearanceCombo_{};
  HWND compressCombo_{};
  HWND dracoLevelEdit_{};
  HWND optimizeCheck_{};
  HWND profileCombo_{};
  HWND deflectionEdit_{};
  HWND angularEdit_{};
  HWND selectButton_{};
  HWND convertButton_{};
  HWND cancelButton_{};
  HWND helpButton_{};
  HWND qualityHelp_{};
  HWND statusLabel_{};
  HWND downloadButton_{};
  HWND conversionInfoHeader_{};
  std::array<HWND, 10> conversionStats_{};
  HWND assemblyHeader_{};
  HWND previewHeader_{};
  HWND previewHelp_{};
  HWND assemblyTree_{};
  HWND previewPanel_{};
  HWND nativePreviewPanel_{};
  HWND headingLabel_{};
  HWND subtitleLabel_{};
  HFONT font_{};
  HFONT headingFont_{};
  HFONT buttonFont_{};
  HBRUSH windowBrush_{};
  HBRUSH surfaceBrush_{};
  HBRUSH statsBrush_{};
  Handle(Aspect_DisplayConnection) displayConnection_;
  Handle(Graphic3d_GraphicDriver) graphicDriver_;
  Handle(V3d_Viewer) viewer_;
  Handle(V3d_View) view_;
  Handle(AIS_InteractiveContext) previewContext_;
  AIS_ViewController previewController_;
  Handle(WNT_Window) previewWindow_;
  std::vector<Handle(XCAFPrs_AISObject)> previewObjects_;
  WNDPROC previewOriginalProc_{};

  WNDPROC selectOriginalProc_{};
  POINT previewLastPoint_{};
  bool previewRotating_ = false;
  bool previewPanning_ = false;
  bool nativePreviewActive_ = false;
  HDC nativePreviewDc_{};
  HGLRC nativePreviewGlrc_{};
  std::vector<native_stl::Triangle> nativePreviewTriangles_;
  POINT nativePreviewLastPoint_{};
  bool nativePreviewRotating_ = false;
  bool nativePreviewPanning_ = false;
  float nativePreviewYaw_ = 35.0f;
  float nativePreviewPitch_ = 20.0f;
  float nativePreviewZoom_ = 1.0f;
  float nativePreviewPanX_ = 0.0f;
  float nativePreviewPanY_ = 0.0f;
  ULONGLONG nativePreviewLastRenderTick_ = 0;
  bool conversionSucceeded_ = false;
  bool conversionRunning_ = false;
  std::thread conversionWorker_;
  bool inputSelected_ = false;
  fs::path selectedInput_;
  fs::path completedOutputPath_;
  fs::path completedDownloadName_;
  std::shared_ptr<std::atomic_bool> activeCancelRequested_;
};

static App* g_app = nullptr;

int App::run(const fs::path& autoInput) {
  g_app = this;
  if (!createWindow()) return 1;
  if (!autoInput.empty()) {
    selectInputPath(autoInput);
    convertSelected();
  }
  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

bool App::createWindow() {
  WNDCLASSW wc{};
  wc.lpfnWndProc = &App::windowProc;
  wc.hInstance = instance_;
  wc.lpszClassName = L"CadConverter2Window";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  WNDCLASSW nativePreviewClass{};
  nativePreviewClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
  nativePreviewClass.lpfnWndProc = &App::nativePreviewWndProc;
  nativePreviewClass.hInstance = instance_;
  nativePreviewClass.lpszClassName = L"CadNativePreviewWindow";
  nativePreviewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  nativePreviewClass.hbrBackground = nullptr;
  if (!RegisterClassW(&nativePreviewClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
  WNDCLASSW occtPreviewClass{};
  occtPreviewClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
  occtPreviewClass.lpfnWndProc = &App::previewProc;
  occtPreviewClass.hInstance = instance_;
  occtPreviewClass.lpszClassName = L"CadOcctPreviewWindow";
  occtPreviewClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  occtPreviewClass.hbrBackground = nullptr;
  if (!RegisterClassW(&occtPreviewClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

  window_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName,
                            L"CAD Converter 2 " CAD_CONVERTER_VERSION_TEXT L" - Interactive Preview - OCCT 8.0.1",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            1280, 1260, nullptr, nullptr, instance_, nullptr);
  if (!window_) return false;
  INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_TREEVIEW_CLASSES};
  InitCommonControlsEx(&commonControls);
  DragAcceptFiles(window_, TRUE);
  createControls();
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

void App::createControls() {
  windowBrush_ = CreateSolidBrush(RGB(241, 245, 249));
  surfaceBrush_ = CreateSolidBrush(RGB(255, 255, 255));
  statsBrush_ = CreateSolidBrush(RGB(248, 250, 252));
  font_ = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  headingFont_ = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  buttonFont_ = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  auto add = [&](DWORD exStyle, LPCWSTR className, LPCWSTR text, DWORD style,
                 int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(exStyle, className, text, style,
                                   x, y, width, height, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   instance_, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return control;
  };
  auto label = [&](LPCWSTR text, int x, int y, int width, int height) {
    return add(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, width, height, 0);
  };

  headingLabel_ = label(L"CAD Converter 2 " CAD_CONVERTER_VERSION_TEXT, 44, 24, 500, 34);
  SendMessageW(headingLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
  helpButton_ = add(0, L"BUTTON", L"Help", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_FLAT,
                    848, 28, 72, 30, IDC_MANUAL_HELP);
  SendMessageW(helpButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
#if CAD_CONVERTER_HAS_DRACO
  subtitleLabel_ = label(L"Native CAD conversion with Draco and Meshopt GLB export", 44, 62, 700, 24);
#else
  subtitleLabel_ = label(L"Native CAD conversion with Meshopt GLB export", 44, 62, 700, 24);
#endif
  selectButton_ = add(0, L"BUTTON", L"Drop a CAD file here or click to browse",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      44, 98, 892, 168, IDC_SELECT);
  inputLabel_ = label(L"No model selected", 48, 276, 820, 24);
  convertButton_ = add(0, L"BUTTON", L"Convert", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED,
                       48, 414, 250, 42, IDC_CONVERT);
  cancelButton_ = add(0, L"BUTTON", L"Cancel conversion", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                          BS_OWNERDRAW | WS_DISABLED,
                      310, 414, 250, 42, IDC_CANCEL);
  SendMessageW(selectButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
  SendMessageW(convertButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
  SendMessageW(cancelButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
  DragAcceptFiles(selectButton_, TRUE);
  selectOriginalProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      selectButton_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::dropProc)));

  label(L"Format", 48, 306, 120, 22);
  formatCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                     48, 330, 170, 180, IDC_FORMAT);
  SendMessageW(formatCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L".glb (binary)"));
  SendMessageW(formatCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L".gltf (JSON + BIN)"));
  SendMessageW(formatCombo_, CB_SETCURSEL, 0, 0);

  label(L"Appearance", 238, 306, 120, 22);
  appearanceCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                         238, 330, 220, 180, IDC_APPEARANCE);
  SendMessageW(appearanceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Colors only"));
  SendMessageW(appearanceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preserve textures"));
  SendMessageW(appearanceCombo_, CB_SETCURSEL, 0, 0);

  label(L"Compression", 478, 306, 150, 22);
  compressCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                       478, 330, 160, 180, IDC_COMPRESS);
  SendMessageW(compressCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"none"));
#if CAD_CONVERTER_HAS_DRACO
  SendMessageW(compressCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Draco"));
#endif
  SendMessageW(compressCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Meshopt (STL only)"));
  SendMessageW(compressCombo_, CB_SETCURSEL, 0, 0);

  label(L"Level", 648, 306, 100, 22);
  dracoLevelEdit_ = add(WS_EX_CLIENTEDGE, L"EDIT", L"7",
                        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                        648, 330, 84, 32, IDC_DRACO_LEVEL);

  optimizeCheck_ = add(0, L"BUTTON", L"Optimize mesh", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        48, 376, 150, 26, IDC_OPTIMIZE);
  SendMessageW(optimizeCheck_, BM_SETCHECK, BST_CHECKED, 0);

  label(L"Profile", 218, 370, 100, 22);
  profileCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                      278, 366, 330, 180, IDC_PROFILE);
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Large assembly - recommended"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Viewer balanced"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CAD faithful"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Fast preview"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));
  SendMessageW(profileCombo_, CB_SETCURSEL, 0, 0);

  label(L"Deflection", 628, 370, 100, 22);
  deflectionEdit_ = add(WS_EX_CLIENTEDGE, L"EDIT", L"1.00",
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                        728, 366, 84, 32, IDC_DEFLECTION);
  label(L"Angular", 628, 414, 100, 22);
  angularEdit_ = add(WS_EX_CLIENTEDGE, L"EDIT", L"1.00",
                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                     728, 410, 84, 32, IDC_ANGULAR);

  qualityHelp_ = label(L"Large assembly: 1.00 deflection / 1.00 angular. Fewer triangles and faster browser interaction.",
                       48, 462, 1160, 24);
  statusLabel_ = label(L"Ready. Select a STEP, IGES, or STL file.", 48, 490, 1160, 24);
  downloadButton_ = add(0, L"BUTTON", L"Download", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED,
                        572, 414, 250, 42, IDC_DOWNLOAD);
  ShowWindow(downloadButton_, SW_HIDE);
  conversionInfoHeader_ = label(L"CONVERSION INFORMATION", 48, 522, 1160, 28);
  const int statX[] = {48, 292, 536, 780, 1024};
  const wchar_t* statTitles[] = {L"INPUT", L"OUTPUT", L"COMPRESSION", L"MESH QUALITY", L"PARTS",
                                 L"ORIGINAL B-REP FACES", L"RESULT MESH FACES", L"FACE REDUCTION", L"SIZE SAVED",
                                 L"PROCESSING TIME"};
  for (int i = 0; i < 10; ++i) {
    const int row = i / 5;
    const int column = i % 5;
    conversionStats_[i] = label(statTitles[i], statX[column], 554 + row * 58, 220, 48);
    ShowWindow(conversionStats_[i], SW_HIDE);
  }
  ShowWindow(conversionInfoHeader_, SW_HIDE);
  assemblyHeader_ = label(L"ASSEMBLY TREE", 44, 548, 280, 28);
  assemblyTree_ = add(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS |
                          TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                      44, 580, 280, 636, 0);
  previewHeader_ = label(L"3D Preview", 340, 548, 896, 28);
  previewPanel_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"CadOcctPreviewWindow", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                  340, 580, 896, 636, window_, nullptr, instance_, nullptr);
  nativePreviewPanel_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"CadNativePreviewWindow", L"",
                                        WS_CHILD | WS_TABSTOP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                        340, 580, 896, 636, window_, nullptr, instance_, nullptr);
  ShowWindow(nativePreviewPanel_, SW_HIDE);
  previewHelp_ = label(L"Left drag: rotate  |  Middle drag: pan  |  Wheel: zoom", 340, 1226, 896, 24);
  updateResultLayout(false);
  updateOptionState();
  initializePreview();
}

void App::initializeNativePreview() {
  nativePreviewDc_ = GetDC(nativePreviewPanel_);
  if (!nativePreviewDc_) return;
  PIXELFORMATDESCRIPTOR format{};
  format.nSize = sizeof(format);
  format.nVersion = 1;
  format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  format.iPixelType = PFD_TYPE_RGBA;
  format.cColorBits = 24;
  format.cDepthBits = 24;
  format.iLayerType = PFD_MAIN_PLANE;
  int pixelFormat = GetPixelFormat(nativePreviewDc_);
  if (pixelFormat == 0) {
    pixelFormat = ChoosePixelFormat(nativePreviewDc_, &format);
    if (pixelFormat == 0 || !SetPixelFormat(nativePreviewDc_, pixelFormat, &format)) return;
  }
  nativePreviewGlrc_ = wglCreateContext(nativePreviewDc_);
}

void App::renderNativePreviewSoftware() {
  if (nativePreviewTriangles_.empty()) {
    setStatus(L"Native preview contains no triangles.");
    return;
  }
  RECT client{};
  GetClientRect(nativePreviewPanel_, &client);
  const int width = std::max(1L, client.right - client.left);
  const int height = std::max(1L, client.bottom - client.top);
  float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
  float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
  for (const auto& triangle : nativePreviewTriangles_) {
    for (int i = 0; i < 9; i += 3) {
      minX = std::min(minX, triangle.vertices[i]); maxX = std::max(maxX, triangle.vertices[i]);
      minY = std::min(minY, triangle.vertices[i + 1]); maxY = std::max(maxY, triangle.vertices[i + 1]);
      minZ = std::min(minZ, triangle.vertices[i + 2]); maxZ = std::max(maxZ, triangle.vertices[i + 2]);
    }
  }
  const float centerX = (minX + maxX) * 0.5f;
  const float centerY = (minY + maxY) * 0.5f;
  const float centerZ = (minZ + maxZ) * 0.5f;
  const float extent = std::max({maxX - minX, maxY - minY, maxZ - minZ, 1.0f});
  const float yaw = nativePreviewYaw_ * 3.14159265f / 180.0f;
  const float pitch = nativePreviewPitch_ * 3.14159265f / 180.0f;
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  const float cp = std::cos(pitch), sp = std::sin(pitch);
  const float scale = std::min(width, height) * 0.72f * nativePreviewZoom_ / extent;
  struct ProjectedTriangle { POINT points[3]; float depth; };
  std::vector<ProjectedTriangle> projected;
  const std::size_t step = std::max<std::size_t>(1, nativePreviewTriangles_.size() / 12000);
  projected.reserve((nativePreviewTriangles_.size() + step - 1) / step);
  for (std::size_t index = 0; index < nativePreviewTriangles_.size(); index += step) {
    const auto& triangle = nativePreviewTriangles_[index];
    ProjectedTriangle item{};
    float depth = 0.0f;
    for (int vertex = 0; vertex < 3; ++vertex) {
      const float x = triangle.vertices[vertex * 3] - centerX;
      const float y = triangle.vertices[vertex * 3 + 1] - centerY;
      const float z = triangle.vertices[vertex * 3 + 2] - centerZ;
      const float rx = cy * x + sy * z;
      const float rz = -sy * x + cy * z;
      const float ry = cp * y - sp * rz;
      const float finalZ = sp * y + cp * rz;
      item.points[vertex] = {
          static_cast<LONG>(width * 0.5f + nativePreviewPanX_ * width + rx * scale),
          static_cast<LONG>(height * 0.5f - nativePreviewPanY_ * height - ry * scale)};
      depth += finalZ;
    }
    item.depth = depth / 3.0f;
    projected.push_back(item);
  }
  std::sort(projected.begin(), projected.end(),
            [](const ProjectedTriangle& a, const ProjectedTriangle& b) { return a.depth < b.depth; });
  HDC dc = GetDC(nativePreviewPanel_);
  if (!dc) return;
  RECT drawRect{0, 0, width, height};
  HBRUSH background = CreateSolidBrush(RGB(14, 18, 26));
  FillRect(dc, &drawRect, background);
  DeleteObject(background);
  HBRUSH meshBrush = CreateSolidBrush(RGB(64, 145, 235));
  HPEN meshPen = CreatePen(PS_NULL, 0, RGB(64, 145, 235));
  HGDIOBJ oldBrush = SelectObject(dc, meshBrush);
  HGDIOBJ oldPen = SelectObject(dc, meshPen);
  SetPolyFillMode(dc, ALTERNATE);
  for (const auto& triangle : projected) Polygon(dc, triangle.points, 3);
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(meshPen);
  DeleteObject(meshBrush);
  ReleaseDC(nativePreviewPanel_, dc);
  nativePreviewLastRenderTick_ = GetTickCount64();
}

void App::renderNativePreviewHardware() {
  if (!nativePreviewDc_ || !nativePreviewGlrc_ || nativePreviewTriangles_.empty()) return;
  RECT client{};
  GetClientRect(nativePreviewPanel_, &client);
  const float width = static_cast<float>(std::max(1L, client.right - client.left));
  const float height = static_cast<float>(std::max(1L, client.bottom - client.top));
  float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
  float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
  for (const auto& triangle : nativePreviewTriangles_) {
    for (int i = 0; i < 9; i += 3) {
      minX = std::min(minX, triangle.vertices[i]); maxX = std::max(maxX, triangle.vertices[i]);
      minY = std::min(minY, triangle.vertices[i + 1]); maxY = std::max(maxY, triangle.vertices[i + 1]);
      minZ = std::min(minZ, triangle.vertices[i + 2]); maxZ = std::max(maxZ, triangle.vertices[i + 2]);
    }
  }
  const float centerX = (minX + maxX) * 0.5f;
  const float centerY = (minY + maxY) * 0.5f;
  const float centerZ = (minZ + maxZ) * 0.5f;
  const float extent = std::max({maxX - minX, maxY - minY, maxZ - minZ, 1.0f});
  if (!wglMakeCurrent(nativePreviewDc_, nativePreviewGlrc_)) return;
  glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
  glClearColor(0.78f, 0.88f, 0.97f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrtho(0.0, width, 0.0, height, -1.0, 1.0);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();
  glBegin(GL_QUADS);
  glColor3f(0.92f, 0.97f, 1.0f);
  glVertex2f(0.0f, height);
  glVertex2f(width, height);
  glColor3f(0.68f, 0.84f, 0.96f);
  glVertex2f(width, 0.0f);
  glVertex2f(0.0f, 0.0f);
  glEnd();
  glPopMatrix();
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
  glDisable(GL_CULL_FACE);
  glEnable(GL_LIGHTING);
  glEnable(GL_LIGHT0);
  glEnable(GL_NORMALIZE);
  glShadeModel(GL_FLAT);
  const GLfloat lightPosition[] = {0.35f, 0.65f, 1.0f, 0.0f};
  const GLfloat lightAmbient[] = {0.22f, 0.24f, 0.28f, 1.0f};
  const GLfloat lightDiffuse[] = {0.95f, 0.95f, 0.95f, 1.0f};
  const GLfloat materialAmbient[] = {0.08f, 0.20f, 0.36f, 1.0f};
  const GLfloat materialDiffuse[] = {0.25f, 0.58f, 0.96f, 1.0f};
  const GLfloat materialSpecular[] = {0.45f, 0.55f, 0.70f, 1.0f};
  glLightfv(GL_LIGHT0, GL_AMBIENT, lightAmbient);
  glLightfv(GL_LIGHT0, GL_DIFFUSE, lightDiffuse);
  glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, materialAmbient);
  glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, materialDiffuse);
  glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, materialSpecular);
  glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 36.0f);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  const double aspect = width / height;
  const double view = extent * 1.15;
  glOrtho(-view * aspect, view * aspect, -view, view, -extent * 10.0, extent * 10.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glLightfv(GL_LIGHT0, GL_POSITION, lightPosition);
  glTranslatef(nativePreviewPanX_ * extent, nativePreviewPanY_ * extent, 0.0f);
  glScalef(nativePreviewZoom_, nativePreviewZoom_, nativePreviewZoom_);
  glRotatef(nativePreviewPitch_, 1.0f, 0.0f, 0.0f);
  glRotatef(nativePreviewYaw_, 0.0f, 1.0f, 0.0f);
  glTranslatef(-centerX, -centerY, -centerZ);
  glBegin(GL_TRIANGLES);
  for (const auto& triangle : nativePreviewTriangles_) {
    glNormal3fv(triangle.normal);
    glVertex3fv(triangle.vertices);
    glVertex3fv(triangle.vertices + 3);
    glVertex3fv(triangle.vertices + 6);
  }
  glEnd();
  glDisable(GL_LIGHT0);
  glDisable(GL_LIGHTING);
  glFlush();
  SwapBuffers(nativePreviewDc_);
  wglMakeCurrent(nullptr, nullptr);
  nativePreviewLastRenderTick_ = GetTickCount64();
}

void App::showNativePreviewGeometry(const std::vector<native_stl::Triangle>& triangles) {
  ShowWindow(previewPanel_, SW_HIDE);
  nativePreviewTriangles_ = triangles;
  nativePreviewActive_ = true;
  nativePreviewYaw_ = 35.0f;
  nativePreviewPitch_ = 20.0f;
  nativePreviewZoom_ = 1.0f;
  nativePreviewPanX_ = 0.0f;
  nativePreviewPanY_ = 0.0f;
  SetWindowPos(nativePreviewPanel_, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  InvalidateRect(nativePreviewPanel_, nullptr, FALSE);
  UpdateWindow(nativePreviewPanel_);
  renderNativePreviewHardware();
}

void App::showNativePreview(const std::vector<native_stl::Triangle>& triangles) {
  clearAssemblyTree();
  std::wstring treeText = L"STL mesh (" + std::to_wstring(triangles.size()) + L" faces)";
  TVINSERTSTRUCTW treeItem{};
  treeItem.hParent = TVI_ROOT;
  treeItem.hInsertAfter = TVI_LAST;
  treeItem.item.mask = TVIF_TEXT;
  treeItem.item.pszText = const_cast<LPWSTR>(treeText.c_str());
  TreeView_InsertItem(assemblyTree_, &treeItem);
  showNativePreviewGeometry(triangles);
}

void App::initializePreview() {
  initializeNativePreview();
  try {
    displayConnection_ = new Aspect_DisplayConnection();
    graphicDriver_ = new OpenGl_GraphicDriver(displayConnection_);
    viewer_ = new V3d_Viewer(graphicDriver_);
    viewer_->SetDefaultLights();
    viewer_->SetLightOn();
    previewContext_ = new AIS_InteractiveContext(viewer_);
    previewController_.SetNavigationMode(AIS_NavigationMode_Orbit);
    view_ = viewer_->CreateView();
    previewWindow_ = new WNT_Window(reinterpret_cast<Aspect_Handle>(previewPanel_));
    view_->SetWindow(previewWindow_);
    if (!previewWindow_->IsMapped()) previewWindow_->Map();
    previewOriginalProc_ = nullptr;
    const Quantity_Color gradientTop(0.92, 0.97, 1.0, Quantity_TOC_RGB);
    const Quantity_Color gradientBottom(0.68, 0.84, 0.96, Quantity_TOC_RGB);
    view_->SetBgGradientColors(gradientTop, gradientBottom,
                               Aspect_GradientFillMethod_Vertical, true);
    view_->SetShadingModel(V3d_PHONG);
    view_->Redraw();
  } catch (const Standard_Failure& failure) {
    setStatus(L"3D preview unavailable: OCCT visualization initialization failed.");
  }
}

void App::resizePreview() {
  if (nativePreviewActive_) {
    renderNativePreviewHardware();
    return;
  }
  if (!view_.IsNull()) {
    view_->MustBeResized();
    view_->Redraw();
  }
}

void App::clearAssemblyTree() {
  if (assemblyTree_) TreeView_DeleteAllItems(assemblyTree_);
}

void App::populateAssemblyTree(const Handle(TDocStd_Document)& document) {
  clearAssemblyTree();
  if (document.IsNull()) return;
  const Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  auto insertText = [&](const std::wstring& text, HTREEITEM parent) {
    TVINSERTSTRUCTW item{};
    item.hParent = parent;
    item.hInsertAfter = TVI_LAST;
    item.item.mask = TVIF_TEXT;
    item.item.pszText = const_cast<LPWSTR>(text.c_str());
    return TreeView_InsertItem(assemblyTree_, &item);
  };
  auto fallbackPartName = [](int index) {
    std::wstringstream name;
    name << L"Part " << std::setfill(L'0') << std::setw(3) << index;
    return name.str();
  };
  std::function<HTREEITEM(const TDF_Label&, HTREEITEM, int, bool)> insertLabel;
  insertLabel = [&](const TDF_Label& label, HTREEITEM parent, int fallbackIndex,
                    bool allowShapeFallback) -> HTREEITEM {
    std::wstring text = xcaf_structure::labelName(label);
    TDF_Label referred;
    if (xcaf_structure::isPlaceholderName(text) &&
        XCAFDoc_ShapeTool::GetReferredShape(label, referred)) {
      text = xcaf_structure::labelName(referred);
    }
    if (xcaf_structure::isPlaceholderName(text)) text = fallbackPartName(fallbackIndex);
    const HTREEITEM treeItem = insertText(text, parent);

    const TDF_Label structureLabel = referred.IsNull() ? label : referred;
    NCollection_Sequence<TDF_Label> components;
    if (XCAFDoc_ShapeTool::GetComponents(structureLabel, components, false)) {
      for (Standard_Integer index = 1; index <= components.Length(); ++index) {
        insertLabel(components.Value(index), treeItem, index, false);
      }
    } else if (allowShapeFallback) {
      const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(label);
      int childIndex = 1;
      for (TopoDS_Iterator children(shape); children.More(); children.Next()) {
        insertText(fallbackPartName(childIndex++), treeItem);
      }
    }
    return treeItem;
  };
  for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
    insertLabel(roots.Value(i), TVI_ROOT, i, true);
  }
  if (roots.Length() > 0) {
    TreeView_Expand(assemblyTree_, TreeView_GetRoot(assemblyTree_), TVE_EXPAND);
  }
}

std::vector<native_stl::Triangle> App::collectDocumentTriangles(
    const Handle(TDocStd_Document)& document) const {
  std::vector<native_stl::Triangle> triangles;
  if (document.IsNull()) return triangles;
  const Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  for (Standard_Integer rootIndex = 1; rootIndex <= roots.Length(); ++rootIndex) {
    const TopoDS_Shape rootShape = XCAFDoc_ShapeTool::GetShape(roots.Value(rootIndex));
    for (TopExp_Explorer faces(rootShape, TopAbs_FACE); faces.More(); faces.Next()) {
      TopLoc_Location location;
      const Handle(Poly_Triangulation) triangulation =
          BRep_Tool::Triangulation(TopoDS::Face(faces.Current()), location);
      if (triangulation.IsNull()) continue;
      for (Standard_Integer triangleIndex = 1;
           triangleIndex <= triangulation->NbTriangles(); ++triangleIndex) {
        int n1 = 0, n2 = 0, n3 = 0;
        triangulation->Triangle(triangleIndex).Get(n1, n2, n3);
        const gp_Pnt p1 = triangulation->Node(n1).Transformed(location.Transformation());
        const gp_Pnt p2 = triangulation->Node(n2).Transformed(location.Transformation());
        const gp_Pnt p3 = triangulation->Node(n3).Transformed(location.Transformation());
        const gp_Vec normal = gp_Vec(p1, p2).Crossed(gp_Vec(p1, p3));
        if (normal.SquareMagnitude() <= Precision::Confusion()) continue;
        const double length = std::sqrt(normal.SquareMagnitude());
        native_stl::Triangle result{};
        result.normal[0] = static_cast<float>(normal.X() / length);
        result.normal[1] = static_cast<float>(normal.Y() / length);
        result.normal[2] = static_cast<float>(normal.Z() / length);
        const gp_Pnt points[] = {p1, p2, p3};
        for (int pointIndex = 0; pointIndex < 3; ++pointIndex) {
          result.vertices[pointIndex * 3] = static_cast<float>(points[pointIndex].X());
          result.vertices[pointIndex * 3 + 1] = static_cast<float>(points[pointIndex].Y());
          result.vertices[pointIndex * 3 + 2] = static_cast<float>(points[pointIndex].Z());
        }
        triangles.push_back(result);
      }
    }
  }
  return triangles;
}

void App::showPreview(const Handle(TDocStd_Document)& document) {
  nativePreviewActive_ = false;
  ShowWindow(nativePreviewPanel_, SW_HIDE);
  SetWindowPos(previewPanel_, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  populateAssemblyTree(document);
  const std::vector<native_stl::Triangle> previewTriangles = collectDocumentTriangles(document);
  if (!previewTriangles.empty()) {
    showNativePreviewGeometry(previewTriangles);
    return;
  }
  if (previewContext_.IsNull() || view_.IsNull()) return;
  previewContext_->RemoveAll(false);
  previewObjects_.clear();

  const Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
    Handle(XCAFPrs_AISObject) object = new XCAFPrs_AISObject(roots.Value(i));
    object->SetDisplayMode(AIS_Shaded);
    previewObjects_.push_back(object);
    previewContext_->Display(object, false);
  }
  if (!previewObjects_.empty()) {
    view_->MustBeResized();
    view_->FitAll(0.01, false);
    view_->Redraw();
    InvalidateRect(previewPanel_, nullptr, FALSE);
    UpdateWindow(previewPanel_);
  }
}

bool App::handleNativePreviewMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (!nativePreviewActive_) return false;
  const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
  const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
  switch (message) {
    case WM_ERASEBKGND:
      return true;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(hwnd, &paint);
      EndPaint(hwnd, &paint);
      renderNativePreviewHardware();
      return true;
    }
    case WM_LBUTTONDOWN:
      nativePreviewRotating_ = true;
      nativePreviewPanning_ = false;
      nativePreviewLastPoint_ = {x, y};
      SetCapture(hwnd);
      return true;
    case WM_LBUTTONUP:
      nativePreviewRotating_ = false;
      if (!nativePreviewPanning_) ReleaseCapture();
      renderNativePreviewHardware();
      return true;
    case WM_MBUTTONDOWN:
      nativePreviewPanning_ = true;
      nativePreviewRotating_ = false;
      nativePreviewLastPoint_ = {x, y};
      SetCapture(hwnd);
      return true;
    case WM_MBUTTONUP:
      nativePreviewPanning_ = false;
      if (!nativePreviewRotating_) ReleaseCapture();
      renderNativePreviewHardware();
      return true;
    case WM_MOUSEMOVE:
      if (nativePreviewRotating_) {
        nativePreviewYaw_ += static_cast<float>(x - nativePreviewLastPoint_.x) * 0.5f;
        nativePreviewPitch_ += static_cast<float>(y - nativePreviewLastPoint_.y) * 0.5f;
        nativePreviewPitch_ = std::clamp(nativePreviewPitch_, -89.0f, 89.0f);
        nativePreviewLastPoint_ = {x, y};
        if (GetTickCount64() - nativePreviewLastRenderTick_ >= 33) renderNativePreviewHardware();
        return true;
      }
      if (nativePreviewPanning_) {
        nativePreviewPanX_ += static_cast<float>(x - nativePreviewLastPoint_.x) * 0.002f;
        nativePreviewPanY_ -= static_cast<float>(y - nativePreviewLastPoint_.y) * 0.002f;
        nativePreviewLastPoint_ = {x, y};
        if (GetTickCount64() - nativePreviewLastRenderTick_ >= 33) renderNativePreviewHardware();
        return true;
      }
      break;
    case WM_MOUSEWHEEL: {
      const float direction = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1.12f : (1.0f / 1.12f);
      nativePreviewZoom_ = std::clamp(nativePreviewZoom_ * direction, 0.15f, 8.0f);
      renderNativePreviewHardware();
      return true;
    }
  }
  return false;
}

bool App::handlePreviewMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (handleNativePreviewMessage(hwnd, message, wParam, lParam)) return true;
  if (previewWindow_.IsNull() || previewContext_.IsNull() || view_.IsNull()) return false;
  const int x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
  const int y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
  switch (message) {
    case WM_LBUTTONDOWN:
      previewRotating_ = true;
      previewPanning_ = false;
      previewLastPoint_ = {x, y};
      SetCapture(hwnd);
      view_->StartRotation(x, y);
      return true;
    case WM_LBUTTONUP:
      previewRotating_ = false;
      if (!previewPanning_) ReleaseCapture();
      return true;
    case WM_MBUTTONDOWN:
      previewPanning_ = true;
      previewRotating_ = false;
      previewLastPoint_ = {x, y};
      SetCapture(hwnd);
      return true;
    case WM_MBUTTONUP:
      previewPanning_ = false;
      if (!previewRotating_) ReleaseCapture();
      return true;
    case WM_MOUSEMOVE:
      if (previewRotating_) {
        view_->Rotation(x, y);
        view_->Redraw();
        previewLastPoint_ = {x, y};
        return true;
      }
      if (previewPanning_) {
        view_->Pan(x - previewLastPoint_.x, previewLastPoint_.y - y, 1.0, false);
        view_->Redraw();
        previewLastPoint_ = {x, y};
        return true;
      }
      break;
    case WM_MOUSEWHEEL: {
      const int delta = static_cast<short>(HIWORD(wParam));
      view_->SetZoom(delta > 0 ? 1.15 : 0.87, true);
      view_->Redraw();
      return true;
    }
    case WM_CAPTURECHANGED:
      previewRotating_ = false;
      previewPanning_ = false;
      return true;
  }
  MSG nativeMessage{};
  nativeMessage.hwnd = hwnd;
  nativeMessage.message = message;
  nativeMessage.wParam = wParam;
  nativeMessage.lParam = lParam;
  if (!previewWindow_->ProcessMessage(previewController_, nativeMessage)) return false;
  previewController_.FlushViewEvents(previewContext_, view_, true);
  return true;
}

LRESULT CALLBACK App::nativePreviewWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (g_app && g_app->handleNativePreviewMessage(hwnd, message, wParam, lParam)) return 0;
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::previewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (g_app && g_app->handlePreviewMessage(hwnd, message, wParam, lParam)) return 0;
  if (g_app && g_app->previewOriginalProc_) {
    return CallWindowProcW(g_app->previewOriginalProc_, hwnd, message, wParam, lParam);
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::dropProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_DROPFILES && g_app) {
    g_app->handleDropFiles(reinterpret_cast<HDROP>(wParam));
    return 0;
  }
  if (g_app && g_app->selectOriginalProc_) {
    return CallWindowProcW(g_app->selectOriginalProc_, hwnd, message, wParam, lParam);
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool App::paintBackground(HDC dc) {
  RECT client{};
  GetClientRect(window_, &client);
  FillRect(dc, &client, windowBrush_);

  HBRUSH cardBrush = CreateSolidBrush(RGB(255, 255, 255));
  HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
  HGDIOBJ oldBrush = SelectObject(dc, cardBrush);
  HGDIOBJ oldPen = SelectObject(dc, borderPen);
  const bool showResults = IsWindowVisible(conversionInfoHeader_) != FALSE;
  const int sectionTop = showResults ? 688 : 530;
  RoundRect(dc, 20, 16, client.right - 20, showResults ? 678 : 520, 14, 14);
  RoundRect(dc, 20, sectionTop, client.right - 20, 1242, 14, 14);
  if (IsWindowVisible(conversionStats_[0])) {
    HBRUSH statsCardBrush = CreateSolidBrush(RGB(248, 250, 252));
    HPEN statsBorderPen = CreatePen(PS_SOLID, 1, RGB(213, 221, 232));
    HGDIOBJ oldStatsBrush = SelectObject(dc, statsCardBrush);
    HGDIOBJ oldStatsPen = SelectObject(dc, statsBorderPen);
    const int statX[] = {48, 292, 536, 780, 1024};
    for (int i = 0; i < 10; ++i) {
      const int row = i / 5;
      const int column = i % 5;
      const int x = statX[column];
      const int y = 550 + row * 58;
      RoundRect(dc, x, y, x + 220, y + 52, 8, 8);
    }
    SelectObject(dc, oldStatsPen);
    SelectObject(dc, oldStatsBrush);
    DeleteObject(statsBorderPen);
    DeleteObject(statsCardBrush);
  }
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(borderPen);
  DeleteObject(cardBrush);
  return true;
}

HBRUSH App::paintControl(HDC dc, HWND control, UINT message) {
  if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
    SetTextColor(dc, RGB(15, 23, 42));
    SetBkColor(dc, RGB(255, 255, 255));
    return surfaceBrush_;
  }

  SetBkMode(dc, TRANSPARENT);
  for (HWND stat : conversionStats_) {
    if (control == stat) {
      SetTextColor(dc, RGB(71, 85, 105));
      SetBkColor(dc, RGB(248, 250, 252));
      return statsBrush_;
    }
  }
  if (control == headingLabel_) {
    SetTextColor(dc, RGB(15, 23, 42));
  } else if (control == inputLabel_) {
    SetTextColor(dc, inputSelected_ ? RGB(72, 170, 103) : RGB(15, 23, 42));
  } else if (control == statusLabel_) {
    SetTextColor(dc, conversionSucceeded_ ? RGB(72, 170, 103) : RGB(30, 64, 175));
  } else {
    SetTextColor(dc, RGB(71, 85, 105));
  }
  return surfaceBrush_;
}

bool App::drawButton(const DRAWITEMSTRUCT& draw) {
  if (draw.hwndItem != selectButton_ && draw.hwndItem != convertButton_ &&
      draw.hwndItem != cancelButton_ && draw.hwndItem != downloadButton_) return false;
  if (draw.hwndItem == selectButton_) {
    HBRUSH fillBrush = CreateSolidBrush(RGB(247, 250, 255));
    HPEN borderPen = CreatePen(PS_DASH, 2, RGB(91, 133, 255));
    HGDIOBJ oldBrush = SelectObject(draw.hDC, fillBrush);
    HGDIOBJ oldPen = SelectObject(draw.hDC, borderPen);
    RoundRect(draw.hDC, draw.rcItem.left + 1, draw.rcItem.top + 1,
              draw.rcItem.right - 1, draw.rcItem.bottom - 1, 12, 12);
    SelectObject(draw.hDC, oldPen);
    SelectObject(draw.hDC, oldBrush);
    DeleteObject(borderPen);
    DeleteObject(fillBrush);

    SetBkMode(draw.hDC, TRANSPARENT);
    SetTextColor(draw.hDC, RGB(71, 85, 105));
    const int iconX = (draw.rcItem.left + draw.rcItem.right) / 2;
    const int iconTop = draw.rcItem.top + 36;
    HPEN iconPen = CreatePen(PS_SOLID, 4, RGB(91, 133, 255));
    HGDIOBJ oldIconPen = SelectObject(draw.hDC, iconPen);
    MoveToEx(draw.hDC, iconX, iconTop + 26, nullptr);
    LineTo(draw.hDC, iconX, iconTop);
    MoveToEx(draw.hDC, iconX, iconTop, nullptr);
    LineTo(draw.hDC, iconX - 9, iconTop + 10);
    MoveToEx(draw.hDC, iconX, iconTop, nullptr);
    LineTo(draw.hDC, iconX + 9, iconTop + 10);
    SelectObject(draw.hDC, oldIconPen);
    DeleteObject(iconPen);
    RECT titleRect = draw.rcItem;
    titleRect.top += 86;
    titleRect.bottom = titleRect.top + 26;
    DrawTextW(draw.hDC, L"Drop a CAD file here or click to browse", -1, &titleRect,
              DT_CENTER | DT_SINGLELINE);
    RECT hintRect = draw.rcItem;
    hintRect.top += 116;
    hintRect.bottom = hintRect.top + 22;
    SetTextColor(draw.hDC, RGB(126, 142, 179));
    DrawTextW(draw.hDC, L".step  |  .stp  |  .igs  |  .iges  |  .stl", -1, &hintRect,
              DT_CENTER | DT_SINGLELINE);
    if ((draw.itemState & ODS_FOCUS) != 0) DrawFocusRect(draw.hDC, &draw.rcItem);
    return true;
  }
  const bool isPrimary = draw.hwndItem == convertButton_ || draw.hwndItem == downloadButton_;
  const bool isDisabled = (draw.itemState & ODS_DISABLED) != 0;
  const bool isPressed = (draw.itemState & ODS_SELECTED) != 0;
  const COLORREF fill = isDisabled ? RGB(148, 163, 184)
                                  : isPrimary ? (isPressed ? RGB(29, 78, 216) : RGB(37, 99, 235))
                                              : RGB(255, 255, 255);
  const COLORREF border = isPrimary ? fill : RGB(203, 213, 225);
  HBRUSH fillBrush = CreateSolidBrush(fill);
  HPEN borderPen = CreatePen(PS_SOLID, 1, border);
  HGDIOBJ oldBrush = SelectObject(draw.hDC, fillBrush);
  HGDIOBJ oldPen = SelectObject(draw.hDC, borderPen);
  RoundRect(draw.hDC, draw.rcItem.left, draw.rcItem.top, draw.rcItem.right, draw.rcItem.bottom, 8, 8);
  SelectObject(draw.hDC, oldPen);
  SelectObject(draw.hDC, oldBrush);
  DeleteObject(borderPen);
  DeleteObject(fillBrush);

  wchar_t text[128]{};
  GetWindowTextW(draw.hwndItem, text, static_cast<int>(std::size(text)));
  SetBkMode(draw.hDC, TRANSPARENT);
  SetTextColor(draw.hDC, isPrimary || isDisabled ? RGB(255, 255, 255) : RGB(30, 41, 59));
  DrawTextW(draw.hDC, text, -1, const_cast<RECT*>(&draw.rcItem), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  if ((draw.itemState & ODS_FOCUS) != 0) DrawFocusRect(draw.hDC, &draw.rcItem);
  return true;
}

void App::releaseThemeResources() {
  if (nativePreviewGlrc_) {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(nativePreviewGlrc_);
    nativePreviewGlrc_ = nullptr;
  }
  if (nativePreviewDc_) {
    ReleaseDC(nativePreviewPanel_, nativePreviewDc_);
    nativePreviewDc_ = nullptr;
  }
  if (windowBrush_) { DeleteObject(windowBrush_); windowBrush_ = nullptr; }
  if (surfaceBrush_) { DeleteObject(surfaceBrush_); surfaceBrush_ = nullptr; }
  if (statsBrush_) { DeleteObject(statsBrush_); statsBrush_ = nullptr; }
  if (font_) { DeleteObject(font_); font_ = nullptr; }
  if (headingFont_) { DeleteObject(headingFont_); headingFont_ = nullptr; }
  if (buttonFont_) { DeleteObject(buttonFont_); buttonFont_ = nullptr; }
}

void App::setStatus(const std::wstring& message) {
  SetWindowTextW(statusLabel_, message.c_str());
  ShowWindow(statusLabel_, SW_SHOW);
}

void App::updateResultLayout(bool showResults) {
  ShowWindow(conversionInfoHeader_, showResults ? SW_SHOW : SW_HIDE);
  for (HWND stat : conversionStats_) ShowWindow(stat, showResults ? SW_SHOW : SW_HIDE);

  const int headerY = showResults ? 706 : 548;
  const int panelY = showResults ? 738 : 580;
  const int panelHeight = 1216 - panelY;
  MoveWindow(assemblyHeader_, 44, headerY, 280, 28, TRUE);
  MoveWindow(previewHeader_, 340, headerY, 896, 28, TRUE);
  MoveWindow(assemblyTree_, 44, panelY, 280, panelHeight, TRUE);
  MoveWindow(previewPanel_, 340, panelY, 896, panelHeight, TRUE);
  MoveWindow(nativePreviewPanel_, 340, panelY, 896, panelHeight, TRUE);
  MoveWindow(previewHelp_, 340, 1226, 896, 24, TRUE);
  InvalidateRect(window_, nullptr, TRUE);
  resizePreview();
}

void App::applyProfileValues() {
  const LRESULT profile = SendMessageW(profileCombo_, CB_GETCURSEL, 0, 0);
  if (profile == 0) {
    writeNumber(deflectionEdit_, 1.00);
    writeNumber(angularEdit_, 1.00);
  } else if (profile == 1) {
    writeNumber(deflectionEdit_, 0.50);
    writeNumber(angularEdit_, 0.70);
  } else if (profile == 2) {
    writeNumber(deflectionEdit_, 0.20);
    writeNumber(angularEdit_, 0.50);
  } else if (profile == 3) {
    writeNumber(deflectionEdit_, 2.00);
    writeNumber(angularEdit_, 1.50);
  }
}

void App::updateOptionState() {
  const bool binary = SendMessageW(formatCombo_, CB_GETCURSEL, 0, 0) == 0;
  const LRESULT compression = SendMessageW(compressCombo_, CB_GETCURSEL, 0, 0);
  const bool draco = compression == kCompressionDracoIndex;
  EnableWindow(compressCombo_, binary);
  EnableWindow(dracoLevelEdit_, binary && draco);
  if (!binary) SendMessageW(compressCombo_, CB_SETCURSEL, kCompressionNoneIndex, 0);
  const bool optimize = SendMessageW(optimizeCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  EnableWindow(deflectionEdit_, optimize);
  EnableWindow(angularEdit_, optimize);
  const LRESULT profile = SendMessageW(profileCombo_, CB_GETCURSEL, 0, 0);
  const wchar_t* help = L"Large assembly: 1.00 deflection / 1.00 angular. Fewer triangles and faster browser interaction.";
  if (profile == 1) help = L"Viewer balanced: 0.50 deflection / 0.70 angular. A middle ground between detail and performance.";
  else if (profile == 2) help = L"CAD faithful: 0.20 deflection / 0.50 angular. More small-feature detail, with heavier output.";
  else if (profile == 3) help = L"Fast preview: 2.00 deflection / 1.50 angular. Fastest conversion; small features may disappear.";
  else if (profile == 4) help = L"Custom mesh quality: edit deflection and angular tolerance directly.";
  if (!optimize) help = L"Optimize mesh off: faithful 0.20 deflection / 0.50 angular settings will be used.";
  const bool stlSelected = !selectedInput_.empty() && lower(selectedInput_.extension().string()) == ".stl";
  if (stlSelected) {
    if (!optimize) help = L"STL optimization off: preserve all input polygons; optional Meshopt compression can still reduce file size.";
    else if (profile == 0) help = L"STL large assembly: cleanup, weld, quantize, and target 50% polygons.";
    else if (profile == 1) help = L"STL viewer balanced: cleanup, weld, quantize, and target 70% polygons.";
    else if (profile == 2) help = L"STL CAD faithful: cleanup, weld, and quantize without polygon simplification.";
    else if (profile == 3) help = L"STL fast preview: cleanup, weld, quantize, and target 25% polygons.";
    else help = L"STL custom: deflection controls reduction ratio; angular controls simplification error.";
  } else if (!selectedInput_.empty()) {
    const std::string extension = lower(selectedInput_.extension().string());
    const bool stepOrIges = extension == ".step" || extension == ".stp" ||
                            extension == ".igs" || extension == ".iges";
    if (stepOrIges && optimize) {
      if (profile == 0) help = L"Large assembly: scale-aware 0.25% deflection / 0.70 angular. Fewer triangles and faster browser interaction.";
      else if (profile == 1) help = L"Viewer balanced: scale-aware 0.10% deflection / 0.45 angular.";
      else if (profile == 2) help = L"CAD faithful: scale-aware 0.05% deflection / 0.25 angular. Highest detail.";
      else if (profile == 3) help = L"Fast preview: scale-aware 0.50% deflection / 1.00 angular.";
      else help = L"Custom STEP/IGES: absolute deflection and angular tolerance are used.";
    }
  }
  SetWindowTextW(qualityHelp_, help);
}

void App::handleCommand(WPARAM wParam) {
  const int id = LOWORD(wParam);
  const int code = HIWORD(wParam);
  if (id == IDC_SELECT && code == BN_CLICKED) chooseInput();
  else if (id == IDC_CONVERT && code == BN_CLICKED) convertSelected();
  else if (id == IDC_CANCEL && code == BN_CLICKED) cancelConversion();
  else if (id == IDC_DOWNLOAD && code == BN_CLICKED) downloadOutput();
  else if (id == IDC_MANUAL_HELP && code == BN_CLICKED) openHelp();
  else if (id == IDC_PROFILE && code == CBN_SELCHANGE) { applyProfileValues(); updateOptionState(); }
  else if ((id == IDC_FORMAT || id == IDC_COMPRESS || id == IDC_OPTIMIZE) &&
           (code == CBN_SELCHANGE || code == BN_CLICKED)) {
    updateOptionState();
  }
  else if ((id == IDC_DEFLECTION || id == IDC_ANGULAR) && code == EN_CHANGE) updateOptionState();
}

void App::chooseInput() {
  ComPtr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) return;
  COMDLG_FILTERSPEC filters[] = {
      {L"CAD models", L"*.step;*.stp;*.igs;*.iges;*.stl"},
      {L"All files", L"*.*"},
  };
  dialog->SetFileTypes(2, filters);
  if (FAILED(dialog->Show(window_))) return;
  ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item))) return;
  PWSTR path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
  const fs::path selectedPath(path);
  CoTaskMemFree(path);
  selectInputPath(selectedPath);
}

void App::downloadOutput() {
  if (completedOutputPath_.empty() || !fs::is_regular_file(completedOutputPath_)) {
    setStatus(L"Error: the completed output is no longer available.");
    return;
  }
  ComPtr<IFileSaveDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dialog)))) return;
  const bool binary = completedOutputPath_.extension() == L".glb";
  COMDLG_FILTERSPEC filters[] = {
      {binary ? L"GLB files" : L"GLTF files", binary ? L"*.glb" : L"*.gltf"},
      {L"All files", L"*.*"},
  };
  dialog->SetFileTypes(2, filters);
  dialog->SetDefaultExtension(binary ? L"glb" : L"gltf");
  dialog->SetFileName(completedDownloadName_.filename().c_str());
  FILEOPENDIALOGOPTIONS options = 0;
  if (SUCCEEDED(dialog->GetOptions(&options))) {
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
  }
  if (FAILED(dialog->Show(window_))) return;
  ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item))) return;
  PWSTR path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
  const fs::path destination(path);
  CoTaskMemFree(path);
  std::error_code equivalentError;
  if (fs::equivalent(completedOutputPath_, destination, equivalentError) && !equivalentError) {
    setStatus(L"Output is already saved at the selected location.");
    return;
  }
  if (!CopyFileW(completedOutputPath_.c_str(), destination.c_str(), FALSE)) {
    setStatus(L"Error: Windows could not save the converted file.");
    return;
  }
}

void App::openHelp() {
  const fs::path manual = executableDirectory() / L"docs" / L"CAD-Converter-2-V0.31-User-Manual.pdf";
  if (!fs::is_regular_file(manual)) {
    MessageBoxW(window_, L"The V0.31 user manual was not found in the docs folder.",
                L"CAD Converter 2 Help", MB_OK | MB_ICONWARNING);
    return;
  }
  const HINSTANCE result = ShellExecuteW(window_, L"open", manual.c_str(), nullptr,
                                         manual.parent_path().c_str(), SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    MessageBoxW(window_, L"Windows could not open the PDF manual.",
                L"CAD Converter 2 Help", MB_OK | MB_ICONERROR);
  }
}

void App::selectInputPath(const fs::path& path) {
  const std::string extension = lower(path.extension().string());
  const bool supported = extension == ".step" || extension == ".stp" ||
                         extension == ".igs" || extension == ".iges" ||
                         extension == ".stl";
  std::error_code fileError;
  if (!supported || !fs::is_regular_file(path, fileError)) {
    inputSelected_ = false;
    selectedInput_.clear();
    if (!completedOutputPath_.empty()) {
      std::error_code ignored;
      fs::remove(completedOutputPath_, ignored);
    }
    completedOutputPath_.clear();
    completedDownloadName_.clear();
    conversionSucceeded_ = false;
    EnableWindow(convertButton_, FALSE);
    EnableWindow(downloadButton_, FALSE);
    ShowWindow(downloadButton_, SW_HIDE);
    updateResultLayout(false);
    setStatus(L"Error: drop a STEP, STP, IGES, or STL file.");
    return;
  }
  selectedInput_ = path;
  if (!completedOutputPath_.empty()) {
    std::error_code ignored;
    fs::remove(completedOutputPath_, ignored);
  }
  completedOutputPath_.clear();
  completedDownloadName_.clear();
  SetWindowTextW(inputLabel_, (L"Selected: " + selectedInput_.filename().wstring()).c_str());
  EnableWindow(convertButton_, TRUE);
  EnableWindow(downloadButton_, FALSE);
  ShowWindow(downloadButton_, SW_HIDE);
  inputSelected_ = true;
  conversionSucceeded_ = false;
  updateResultLayout(false);
  InvalidateRect(inputLabel_, nullptr, TRUE);
  updateOptionState();
  setStatus(L"Ready to convert with the selected options.");
}

void App::handleDropFiles(HDROP drop) {
  const UINT fileCount = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  if (fileCount == 0) {
    DragFinish(drop);
    return;
  }
  const UINT pathLength = DragQueryFileW(drop, 0, nullptr, 0);
  std::vector<wchar_t> path(pathLength + 1, L'\0');
  DragQueryFileW(drop, 0, path.data(), pathLength + 1);
  DragFinish(drop);
  selectInputPath(fs::path(path.data()));
}

void App::updateConversionInfo(const fs::path& input, const fs::path& output,
                               const ConvertOptions& options,
                               const Handle(TDocStd_Document)& document,
                               std::uint64_t outputPolygonOverride,
                               std::uint64_t inputPolygonOverride,
                               double processingSeconds) {
  std::error_code inputError;
  std::error_code outputError;
  const std::uintmax_t inputBytes = fs::file_size(input, inputError);
  const std::uintmax_t outputBytes = fs::file_size(output, outputError);
  auto sizeText = [](std::uintmax_t bytes) {
    std::wstringstream text;
    text << std::fixed << std::setprecision(1)
         << (static_cast<double>(bytes) / 1024.0) << L" KB";
    return text.str();
  };

  const bool isStl = lower(input.extension().string()) == ".stl";
  std::uint64_t meshCount = 0;
  const Handle(XCAFDoc_ShapeTool) shapeTool = document.IsNull()
      ? Handle(XCAFDoc_ShapeTool)()
      : XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  if (!shapeTool.IsNull()) {
    shapeTool->GetFreeShapes(roots);
    const xcaf_structure::Summary structure = xcaf_structure::summarize(document);
    meshCount = structure.componentCount > 0
        ? structure.componentCount : structure.rootCount;
  }
  std::uint64_t cadFaceCount = 0;
  std::uint64_t triangleCount = 0;
  if (!shapeTool.IsNull()) {
    for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
      const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(roots.Value(i));
      for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
        ++cadFaceCount;
        TopLoc_Location location;
        const Handle(Poly_Triangulation) triangulation =
            BRep_Tool::Triangulation(TopoDS::Face(faces.Current()), location);
        if (!triangulation.IsNull()) triangleCount += triangulation->NbTriangles();
      }
    }
  }
  const std::uint64_t generatedPolygonCount = triangleCount;
  const std::uint64_t originalMeshFaceCount = inputPolygonOverride != 0
      ? inputPolygonOverride : generatedPolygonCount;
  const std::uint64_t originalFaceCount = isStl ? originalMeshFaceCount : cadFaceCount;
  if (outputPolygonOverride != 0) {
    meshCount = meshCount == 0 ? 1 : meshCount;
    triangleCount = outputPolygonOverride;
  }

  std::wstringstream quality;
  const wchar_t* profileName = options.profile == "large" ? L"large" :
                               options.profile == "balanced" ? L"balanced" :
                               options.profile == "preview" ? L"preview" :
                               options.profile == "custom" ? L"custom" : L"faithful";
  if (isStl) {
    int targetPercent = 100;
    if (options.optimize && options.profile == "large") targetPercent = 50;
    else if (options.optimize && options.profile == "balanced") targetPercent = 70;
    else if (options.optimize && options.profile == "preview") targetPercent = 25;
    else if (options.optimize && options.profile == "custom")
      targetPercent = static_cast<int>(std::round(clampDouble(1.0 / std::max(1.0, options.deflection), 0.10, 1.0, 1.0) * 100.0));
    quality << (options.optimize ? L"optimized profile=" : L"preserved profile=") << profileName
            << L"\r\ntarget=" << targetPercent << L"% polygons";
  } else {
    double reportedDeflection = options.deflection;
    double reportedAngular = options.angular;
    if (options.optimize && !document.IsNull()) {
      const OcctMeshQuality actual = occtMeshQuality(options.profile, options.deflection,
                                                      options.angular, roots);
      reportedDeflection = actual.deflection;
      reportedAngular = actual.angular;
    }
    quality << (options.optimize ? L"optimized profile=" : L"faithful profile=") << profileName
            << L"\r\ndeflection=" << reportedDeflection << L" angular=" << reportedAngular;
  }

  std::wstringstream reduction;
  if (isStl && originalMeshFaceCount > 0) {
    const double percent = (1.0 - static_cast<double>(triangleCount) /
                                      static_cast<double>(originalMeshFaceCount)) * 100.0;
    reduction << std::fixed << std::setprecision(1) << percent << L"%";
  } else {
    reduction << L"n/a (CAD face to mesh face)";
  }

  std::wstringstream saved;
  if (inputBytes > 0 && !inputError && !outputError) {
    const double percent = (1.0 - static_cast<double>(outputBytes) /
                                      static_cast<double>(inputBytes)) * 100.0;
    saved << std::fixed << std::setprecision(1) << percent << L"%";
  } else {
    saved << L"n/a";
  }

  std::wstringstream processing;
  processing << std::fixed << std::setprecision(3) << processingSeconds << L" s";

  const std::wstring values[] = {
      L"INPUT\r\n" + sizeText(inputError ? 0 : inputBytes),
      L"OUTPUT\r\n" + sizeText(outputError ? 0 : outputBytes),
      L"COMPRESSION\r\n" + (options.draco ? L"Draco level " + std::to_wstring(options.dracoLevel) :
                                options.meshopt ? L"Meshopt" : L"none"),
      L"MESH QUALITY\r\n" + quality.str(),
      L"PARTS\r\n" + std::to_wstring(meshCount),
      (isStl ? L"ORIGINAL MESH FACES\r\n" : L"ORIGINAL B-REP FACES\r\n") +
          (originalFaceCount > 0 ? std::to_wstring(originalFaceCount) : L"n/a"),
      L"RESULT MESH FACES\r\n" + std::to_wstring(triangleCount),
      L"FACE REDUCTION\r\n" + reduction.str(),
      L"SIZE SAVED\r\n" + saved.str(),
      L"PROCESSING TIME\r\n" + processing.str(),
  };
  for (std::size_t i = 0; i < conversionStats_.size(); ++i) {
    SetWindowTextW(conversionStats_[i], values[i].c_str());
  }
  updateResultLayout(true);
}

bool App::convertCad(ConversionJob& job, const fs::path& input, const fs::path& output,
                     const ConvertOptions& options, ConversionResult& result, std::string& error) {
  const std::string extension = lower(input.extension().string());
  reportProgress(job, L"Preparing conversion...", 0);
  if (failIfCancelled(job, error)) return false;
  if (options.draco && !CAD_CONVERTER_HAS_DRACO) {
    error = "Draco compression requires the Draco-enabled build";
    return false;
  }
  if (options.meshopt && extension != ".stl") {
    error = "Meshopt compression is available for STL only; use none or Draco for STEP/IGES to preserve assembly structure";
    return false;
  }
  Handle(TDocStd_Document) document;
  XCAFApp_Application::GetApplication()->NewDocument(TCollection_ExtendedString("MDTV-XCAF"), document);

  auto makeProgressIndicator = [&](const wchar_t* phase, int base, int span) {
    const std::wstring phaseText(phase);
    Handle(Message_ProgressIndicator) progress = new UiProgressIndicator(
        job.cancelRequested,
        [&job, phaseText, base, span](double position) {
          reportProgress(job, phaseText,
                         base + static_cast<int>(std::round(clampDouble(position, 0.0, 1.0, 0.0) * span)));
        });
    return progress;
  };

  if (extension == ".step" || extension == ".stp") {
    reportProgress(job, L"Reading STEP...", 5);
    STEPCAFControl_Reader reader;
    reader.SetColorMode(true);
    reader.SetNameMode(true);
    Handle(Message_ProgressIndicator) progress = makeProgressIndicator(L"Reading STEP...", 5, 20);
    if (!reader.Perform(input.string().c_str(), document, progress->Start())) {
      if (failIfCancelled(job, error)) return false;
      error = "OCCT 8.0.1 STEP Perform failed";
      return false;
    }
  } else if (extension == ".igs" || extension == ".iges") {
    reportProgress(job, L"Reading IGES...", 5);
    IGESCAFControl_Reader reader;
    reader.SetColorMode(true);
    reader.SetNameMode(true);
    Handle(Message_ProgressIndicator) progress = makeProgressIndicator(L"Reading IGES...", 5, 20);
    if (!reader.Perform(input.string().c_str(), document, progress->Start())) {
      if (failIfCancelled(job, error)) return false;
      error = "OCCT 8.0.1 IGES Perform failed";
      return false;
    }
    xcaf_structure::normalizeIgesAssembly(document, input.stem().wstring());
  } else if (extension == ".stl") {
    if (options.binary && !options.draco) {
      try {
        reportProgress(job, L"Reading STL...", 5);
        std::vector<native_stl::Triangle> triangles = native_stl::read(input);
        const std::uint64_t inputPolygonCount = triangles.size();
        if (failIfCancelled(job, error)) return false;
        if (options.optimize) triangles = native_stl::cleanup(triangles);
        reportProgress(job, L"Preparing STL mesh...", 35);
        if (failIfCancelled(job, error)) return false;
        std::uint64_t outputPolygons = triangles.size();
        const bool postProcess = options.optimize || options.meshopt;
        fs::path rawOutput = output;
        if (postProcess) rawOutput += L".native-raw.glb";
        reportProgress(job, L"Writing GLB...", 55);
        native_stl::writeGlb(rawOutput, triangles);
        if (postProcess && !runGltfpack(rawOutput, output, options, outputPolygons, error,
                                        true, false, &job)) {
          std::error_code ignored;
          fs::remove(rawOutput, ignored);
          return false;
        }
        if (postProcess) {
          std::error_code ignored;
          fs::remove(rawOutput, ignored);
        }
        result.outputPolygons = outputPolygons;
        result.inputPolygons = inputPolygonCount;
        result.nativeTriangles = std::move(triangles);
        result.useNativePreview = true;
        reportProgress(job, L"Validating output...", 95);
        if (failIfCancelled(job, error)) return false;
        gltf_validation::Summary validationSummary;
        std::string validationError;
        if (!gltf_validation::validate(output, {}, validationSummary, validationError)) {
          error = "output validation failed: " + validationError;
          return false;
        }
        reportProgress(job, L"Conversion complete", 100);
        return true;
      } catch (const std::exception& exception) {
        error = exception.what();
        return false;
      }
    }
    TopoDS_Compound compound;
    BRep_Builder builder;
    builder.MakeCompound(compound);
    StlAPI_Reader reader;
    if (!reader.Read(compound, input.string().c_str())) {
      error = "OCCT 8.0.1 STL read failed";
      return false;
    }
    const Handle(XCAFDoc_ShapeTool) stlShapeTool =
        XCAFDoc_DocumentTool::ShapeTool(document->Main());
    const Handle(XCAFDoc_ColorTool) colorTool =
        XCAFDoc_DocumentTool::ColorTool(document->Main());
    const TDF_Label label = stlShapeTool->NewShape();
    stlShapeTool->SetShape(label, compound);
    const Quantity_Color fallbackColor(0.72, 0.76, 0.84, Quantity_TOC_RGB);
    colorTool->SetColor(label, fallbackColor, XCAFDoc_ColorGen);
  } else {
    error = "unsupported input; choose STEP, IGES, or STL";
    return false;
  }

  if (failIfCancelled(job, error)) return false;
  reportProgress(job, L"Building XCAF assembly...", 28);

  const Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  if (roots.Length() == 0) {
    error = "OCCT imported no XCAF shapes";
    return false;
  }
  double meshDeflection = options.deflection;
  double meshAngular = options.angular;
  if (options.optimize && (extension == ".step" || extension == ".stp" ||
                           extension == ".igs" || extension == ".iges")) {
    const OcctMeshQuality quality = occtMeshQuality(options.profile, options.deflection,
                                                    options.angular, roots);
    meshDeflection = quality.deflection;
    meshAngular = quality.angular;
  }
  Handle(Message_ProgressIndicator) meshProgress = makeProgressIndicator(
      L"Meshing", 30, 40);
  Message_ProgressRange meshRange = meshProgress->Start();
  Message_ProgressScope meshScope(meshRange, "Meshing", roots.Length());
  for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
    if (failIfCancelled(job, error)) return false;
    TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(roots.Value(i));
    if (!shape.IsNull()) {
      BRepTools::Clean(shape);
      BRepMesh_IncrementalMesh mesh(shape, meshDeflection, Standard_False,
                                     meshAngular, Standard_True);
      mesh.Perform(meshScope.Next());
      if (!mesh.IsDone()) {
        if (failIfCancelled(job, error)) return false;
        error = "OCCT triangulation failed";
        return false;
      }
    }
  }

  reportProgress(job, L"Writing GLB...", 75);
  if (failIfCancelled(job, error)) return false;
  RWGltf_CafWriter writer(TCollection_AsciiString(output.string().c_str()), options.binary);
  writer.SetMergeFaces(true);
  writer.SetSplitIndices16(true);
  writer.SetParallel(true);
  writer.SetToEmbedTexturesInGlb(!options.colorsOnly);
  RWGltf_DracoParameters draco;
  draco.DracoCompression = options.draco;
  draco.CompressionLevel = options.dracoLevel;
  writer.SetCompressionParameters(draco);
  NCollection_IndexedDataMap<TCollection_AsciiString, TCollection_AsciiString> metadata;
  Handle(Message_ProgressIndicator) writeProgress = makeProgressIndicator(
      L"Writing GLB...", 75, 20);
  if (!writer.Perform(document, metadata, writeProgress->Start())) {
    if (failIfCancelled(job, error)) return false;
    error = "OCCT 8.0.1 GLB export failed";
    return false;
  }
  reportProgress(job, L"Validating output...", 97);
  if (failIfCancelled(job, error)) return false;
  if ((extension == ".igs" || extension == ".iges") &&
      !xcaf_structure::renameFallbackGltfNodes(output, error)) {
    return false;
  }
  gltf_validation::Expectations expectations;
  if (extension == ".step" || extension == ".stp" ||
      extension == ".igs" || extension == ".iges") {
    expectations = validationExpectations(
        document, extension == ".igs" || extension == ".iges");
  }
  gltf_validation::Summary validationSummary;
  std::string validationError;
  if (!gltf_validation::validate(output, expectations, validationSummary, validationError)) {
    error = "output validation failed: " + validationError;
    return false;
  }
  result.document = document;
  reportProgress(job, L"Conversion complete", 100);
  return true;
}

ConvertOptions App::optionsFromControls() const {
  ConvertOptions options;
  options.binary = SendMessageW(formatCombo_, CB_GETCURSEL, 0, 0) == 0;
  options.colorsOnly = SendMessageW(appearanceCombo_, CB_GETCURSEL, 0, 0) == 0;
  const LRESULT compression = SendMessageW(compressCombo_, CB_GETCURSEL, 0, 0);
  options.draco = options.binary && compression == kCompressionDracoIndex;
  options.meshopt = options.binary && compression == kCompressionMeshoptIndex;
  options.dracoLevel = static_cast<int>(readDouble(dracoLevelEdit_, 7.0, 0.0, 10.0));
  options.optimize = SendMessageW(optimizeCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const LRESULT profile = SendMessageW(profileCombo_, CB_GETCURSEL, 0, 0);
  if (profile == 1) options.profile = "balanced";
  else if (profile == 2) options.profile = "faithful";
  else if (profile == 3) options.profile = "preview";
  else if (profile == 4) options.profile = "custom";
  else options.profile = "large";
  options.deflection = readDouble(deflectionEdit_, 1.0, 0.01, 10.0);
  options.angular = readDouble(angularEdit_, 1.0, 0.05, 5.0);
  if (!options.optimize) {
    options.profile = "faithful";
    options.deflection = 0.20;
    options.angular = 0.50;
  }
  return options;
}

void App::convertSelected() {
  if (conversionRunning_) return;
  if (selectedInput_.empty()) {
    inputSelected_ = false;
    conversionSucceeded_ = false;
    updateResultLayout(false);
    setStatus(L"Error: select a STEP, IGES, or STL model first.");
    return;
  }
  if (!completedOutputPath_.empty()) {
    std::error_code ignored;
    fs::remove(completedOutputPath_, ignored);
  }
  completedOutputPath_.clear();
  completedDownloadName_.clear();
  EnableWindow(downloadButton_, FALSE);
  ShowWindow(downloadButton_, SW_HIDE);
  updateResultLayout(false);
  auto* job = new ConversionJob();
  job->input = selectedInput_;
  job->options = optionsFromControls();
  const std::wstring suffix = job->options.binary ? L".glb" : L".gltf";
  job->downloadName = fs::path(job->input.stem().wstring() + L"-converted" +
                               std::wstring(CAD_CONVERTER_OUTPUT_SUFFIX_TEXT) + suffix);
  std::error_code temporaryDirectoryError;
  const fs::path temporaryDirectory = fs::temp_directory_path(temporaryDirectoryError);
  if (temporaryDirectoryError) {
    setStatus(L"Error: Windows could not prepare temporary output storage.");
    delete job;
    return;
  }
  job->output = temporaryDirectory /
                (L"cad-converter2-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()) + L"-" + job->downloadName.wstring());
  job->window = window_;
  std::wstringstream status;
  const bool nativeStl = lower(job->input.extension().string()) == ".stl" &&
                         job->options.binary && !job->options.draco;
  status << (nativeStl ? L"Converting with native C++ STL pipeline - " : L"Converting with OCCT 8.0.1 - ")
         << (job->options.binary ? L"GLB" : L"GLTF")
         << L" - " << (job->options.profile == "faithful" ? L"CAD faithful" :
                        job->options.profile == "balanced" ? L"Viewer balanced" :
                        job->options.profile == "preview" ? L"Fast preview" :
                        job->options.profile == "custom" ? L"Custom" : L"Large assembly")
         << L" - working in background";
  setStatus(status.str());
  conversionRunning_ = true;
  activeCancelRequested_ = job->cancelRequested;
  EnableWindow(convertButton_, FALSE);
  EnableWindow(selectButton_, FALSE);
  EnableWindow(cancelButton_, TRUE);
  conversionWorker_ = std::thread([this, job] {
    const auto start = std::chrono::steady_clock::now();
    job->success = convertCad(*job, job->input, job->output, job->options, job->result, job->error);
    job->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!PostMessageW(window_, WM_APP_CONVERSION_COMPLETE, 0, reinterpret_cast<LPARAM>(job))) {
      delete job;
    }
  });
}

void App::cancelConversion() {
  if (!conversionRunning_ || !activeCancelRequested_) return;
  activeCancelRequested_->store(true, std::memory_order_relaxed);
  EnableWindow(cancelButton_, FALSE);
  setStatus(L"Cancellation requested. Finishing the current safe step...");
}

void App::handleConversionProgress(ConversionProgress* progress) {
  if (!progress) return;
  if (conversionRunning_) setStatus(progress->text);
  delete progress;
}

void App::handleConversionComplete(ConversionJob* job) {
  if (conversionWorker_.joinable()) conversionWorker_.join();
  conversionRunning_ = false;
  activeCancelRequested_.reset();
  EnableWindow(selectButton_, TRUE);
  EnableWindow(convertButton_, TRUE);
  EnableWindow(cancelButton_, FALSE);
  if (!job->success) {
    conversionSucceeded_ = false;
    std::error_code ignored;
    fs::remove(job->output, ignored);
    completedOutputPath_.clear();
    completedDownloadName_.clear();
    EnableWindow(downloadButton_, FALSE);
    ShowWindow(downloadButton_, SW_HIDE);
    updateResultLayout(false);
    if (job->cancelled) setStatus(L"Conversion cancelled safely.");
    else setStatus(L"Error: " + std::wstring(job->error.begin(), job->error.end()));
    delete job;
    return;
  }
  conversionSucceeded_ = true;
  updateConversionInfo(job->input, job->output, job->options, job->result.document,
                       job->result.outputPolygons, job->result.inputPolygons, job->seconds);
  if (job->result.useNativePreview) showNativePreview(job->result.nativeTriangles);
  else showPreview(job->result.document);
  completedOutputPath_ = job->output;
  completedDownloadName_ = job->downloadName;
  ShowWindow(statusLabel_, SW_HIDE);
  EnableWindow(downloadButton_, TRUE);
  ShowWindow(downloadButton_, SW_SHOW);
  delete job;
}

LRESULT CALLBACK App::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (g_app) {
    switch (message) {
      case WM_DROPFILES:
        g_app->handleDropFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
      case WM_ERASEBKGND:
        return g_app->paintBackground(reinterpret_cast<HDC>(wParam)) ? 1 : 0;
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(g_app->paintControl(reinterpret_cast<HDC>(wParam),
                                                               reinterpret_cast<HWND>(lParam), message));
      case WM_DRAWITEM:
        if (lParam && g_app->drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) return TRUE;
        break;
      case WM_COMMAND:
        g_app->handleCommand(wParam);
        return 0;
      case WM_APP_CONVERSION_PROGRESS:
        g_app->handleConversionProgress(reinterpret_cast<ConversionProgress*>(lParam));
        return 0;
      case WM_APP_CONVERSION_COMPLETE:
        g_app->handleConversionComplete(reinterpret_cast<ConversionJob*>(lParam));
        return 0;
      case WM_CLOSE:
        if (g_app->conversionRunning_) {
          g_app->cancelConversion();
          return 0;
        }
        DestroyWindow(hwnd);
        return 0;
      case WM_SIZE:
        g_app->resizePreview();
        break;
      case WM_DESTROY:
        if (g_app->conversionWorker_.joinable()) g_app->conversionWorker_.join();
        if (!g_app->completedOutputPath_.empty()) {
          std::error_code ignored;
          fs::remove(g_app->completedOutputPath_, ignored);
        }
        g_app->releaseThemeResources();
        PostQuitMessage(0);
        return 0;
    }
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

int App::runRegression(const fs::path& input, const fs::path& output) {
  std::error_code fileError;
  if (!fs::is_regular_file(input, fileError)) return 2;
  if (!output.parent_path().empty()) fs::create_directories(output.parent_path(), fileError);
  if (fileError) return 2;

  ConversionJob job;
  job.input = input;
  job.output = output;
  job.options.binary = lower(output.extension().string()) == ".glb";
  job.options.colorsOnly = true;
  job.options.draco = lower(input.extension().string()) != ".stl" && CAD_CONVERTER_HAS_DRACO;
  job.options.meshopt = false;
  job.options.optimize = true;
  job.options.profile = "large";
  job.options.deflection = 1.0;
  job.options.angular = 1.0;

  std::string error;
  const bool success = convertCad(job, input, output, job.options, job.result, error);
  fs::path errorPath = output;
  errorPath += L".error.txt";
  if (!success) {
    std::ofstream failure(errorPath);
    failure << error;
    return 3;
  }
  fs::remove(errorPath, fileError);
  return 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  App app(instance);
  fs::path autoInput;
  int argumentCount = 0;
  LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
  int result = 0;
  if (arguments && argumentCount == 4 && std::wstring(arguments[1]) == L"--regression") {
    result = app.runRegression(arguments[2], arguments[3]);
  } else {
    const std::wstring command(commandLine ? commandLine : L"");
    if (command.rfind(L"--stl-test ", 0) == 0) autoInput = command.substr(11);
    result = app.run(autoInput);
  }
  if (arguments) LocalFree(arguments);
  CoUninitialize();
  return result;
}
