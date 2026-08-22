#include <windows.h>
#include <GL/gl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl.h>

#include <AIS_InteractiveContext.hxx>
#include <AIS_ViewController.hxx>
#include <BRep_Tool.hxx>
#include <BRep_Builder.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <Aspect_DisplayConnection.hxx>
#include <Graphic3d_GraphicDriver.hxx>
#include <IGESCAFControl_Reader.hxx>
#include <Message_ProgressRange.hxx>
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
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <Poly_Triangulation.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include "native_stl.hpp"
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
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

struct ConvertOptions {
  bool binary = true;
  bool colorsOnly = true;
  bool draco = false;
  int dracoLevel = 7;
  bool optimize = true;
  std::string profile = "large";
  double deflection = 1.0;
  double angular = 1.0;
};

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
};

class App {
public:
  explicit App(HINSTANCE instance) : instance_(instance) {}
  int run(const fs::path& autoInput = {});

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
  void showNativePreview(const std::vector<native_stl::Triangle>& triangles);
  void resizePreview();
  void showPreview(const Handle(TDocStd_Document)& document);
  bool handlePreviewMessage(HWND, UINT, WPARAM, LPARAM);
  bool handleNativePreviewMessage(HWND, UINT, WPARAM, LPARAM);
  bool paintBackground(HDC);
  HBRUSH paintControl(HDC, HWND, UINT);
  bool drawButton(const DRAWITEMSTRUCT&);
  void releaseThemeResources();
  void setStatus(const std::wstring& message);
  void updateOptionState();
  void applyProfileValues();
  void handleCommand(WPARAM wParam);
  void chooseInput();
  void selectInputPath(const fs::path& path);
  void handleDropFiles(HDROP drop);
  void convertSelected();
  void updateConversionInfo(const fs::path& input, const fs::path& output,
                            const ConvertOptions& options,
                            const Handle(TDocStd_Document)& document,
                            std::uint64_t outputPolygonOverride = 0);
  ConvertOptions optionsFromControls() const;
  bool convertCad(const fs::path& input, const fs::path& output, const ConvertOptions& options,
                  std::string& error);

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
  HWND qualityHelp_{};
  HWND statusLabel_{};
  HWND outputFileLabel_{};
  HWND processingTimeLabel_{};
  HWND conversionInfoHeader_{};
  std::array<HWND, 8> conversionStats_{};
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
  bool inputSelected_ = false;
  fs::path selectedInput_;
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

  window_ = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName,
                            L"CAD Converter 2 - Interactive Preview - OCCT 8.0.1",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            980, 1260, nullptr, nullptr, instance_, nullptr);
  if (!window_) return false;
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

  headingLabel_ = label(L"CAD Converter 2", 44, 24, 500, 34);
  SendMessageW(headingLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(headingFont_), TRUE);
  subtitleLabel_ = label(L"Native CAD conversion and Draco-ready GLB export", 44, 62, 700, 24);
  selectButton_ = add(0, L"BUTTON", L"Drop a CAD file here or click to browse",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                      44, 98, 892, 168, IDC_SELECT);
  inputLabel_ = label(L"No model selected", 48, 276, 820, 24);
  convertButton_ = add(0, L"BUTTON", L"Convert", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | WS_DISABLED,
                       48, 414, 520, 42, IDC_CONVERT);
  SendMessageW(selectButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
  SendMessageW(convertButton_, WM_SETFONT, reinterpret_cast<WPARAM>(buttonFont_), TRUE);
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

  label(L"Draco", 478, 306, 100, 22);
  compressCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                       478, 330, 120, 180, IDC_COMPRESS);
  SendMessageW(compressCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"none"));
  SendMessageW(compressCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Draco"));
  SendMessageW(compressCombo_, CB_SETCURSEL, 0, 0);

  label(L"Level", 618, 306, 100, 22);
  dracoLevelEdit_ = add(WS_EX_CLIENTEDGE, L"EDIT", L"7",
                        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                        618, 330, 84, 32, IDC_DRACO_LEVEL);

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
                       48, 458, 850, 42);
  statusLabel_ = label(L"Ready. Select a STEP or IGES file.", 48, 500, 850, 24);
  outputFileLabel_ = label(L"", 48, 526, 850, 24);
  processingTimeLabel_ = label(L"", 48, 552, 850, 24);
  ShowWindow(outputFileLabel_, SW_HIDE);
  ShowWindow(processingTimeLabel_, SW_HIDE);
  conversionInfoHeader_ = label(L"CONVERSION INFORMATION", 48, 584, 850, 28);
  const int statX[] = {48, 264, 480, 696};
  const wchar_t* statTitles[] = {L"INPUT", L"OUTPUT", L"COMPRESSION", L"MESH QUALITY",
                                 L"SIZE SAVED", L"MESHES", L"FACES", L"OUTPUT POLYGONS"};
  for (int i = 0; i < 8; ++i) {
    const int row = i / 4;
    conversionStats_[i] = label(statTitles[i], statX[i % 4], 618 + row * 58, 194, 48);
    ShowWindow(conversionStats_[i], SW_HIDE);
  }
  ShowWindow(conversionInfoHeader_, SW_HIDE);
  label(L"3D Preview", 44, 768, 500, 28);
  previewPanel_ = add(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_BLACKRECT | SS_NOTIFY,
                       44, 804, 892, 390, 0);
  nativePreviewPanel_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"CadNativePreviewWindow", L"",
                                        WS_CHILD | WS_TABSTOP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                        44, 804, 892, 390, window_, nullptr, instance_, nullptr);
  ShowWindow(nativePreviewPanel_, SW_HIDE);
  label(L"Left drag: rotate  |  Middle drag: pan  |  Wheel: zoom", 44, 1204, 892, 24);
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

void App::showNativePreview(const std::vector<native_stl::Triangle>& triangles) {
  ShowWindow(previewPanel_, SW_HIDE);
  SetWindowPos(nativePreviewPanel_, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  nativePreviewTriangles_ = triangles;
  nativePreviewActive_ = true;
  nativePreviewYaw_ = 35.0f;
  nativePreviewPitch_ = 20.0f;
  nativePreviewZoom_ = 1.0f;
  nativePreviewPanX_ = 0.0f;
  nativePreviewPanY_ = 0.0f;
  renderNativePreviewHardware();
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
    previewOriginalProc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        previewPanel_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&App::previewProc)));
    view_->SetBackgroundColor(Quantity_NOC_BLACK);
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

void App::showPreview(const Handle(TDocStd_Document)& document) {
  nativePreviewActive_ = false;
  ShowWindow(nativePreviewPanel_, SW_HIDE);
  ShowWindow(previewPanel_, SW_SHOW);
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
    view_->FitAll(0.01, false);
    view_->Redraw();
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
  RoundRect(dc, 20, 16, client.right - 20, 738, 14, 14);
  RoundRect(dc, 20, 748, client.right - 20, 1242, 14, 14);
  if (IsWindowVisible(conversionStats_[0])) {
    HBRUSH statsCardBrush = CreateSolidBrush(RGB(248, 250, 252));
    HPEN statsBorderPen = CreatePen(PS_SOLID, 1, RGB(213, 221, 232));
    HGDIOBJ oldStatsBrush = SelectObject(dc, statsCardBrush);
    HGDIOBJ oldStatsPen = SelectObject(dc, statsBorderPen);
    const int statX[] = {48, 264, 480, 696};
    for (int row = 0; row < 2; ++row) {
      for (int column = 0; column < 4; ++column) {
        const int x = statX[column];
        const int y = 608 + row * 58;
        RoundRect(dc, x, y, x + 194, y + 52, 8, 8);
      }
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
  } else if (control == outputFileLabel_) {
    SetTextColor(dc, RGB(72, 170, 103));
  } else if (control == processingTimeLabel_) {
    SetTextColor(dc, RGB(100, 116, 139));
  } else if (control == statusLabel_) {
    SetTextColor(dc, conversionSucceeded_ ? RGB(72, 170, 103) : RGB(30, 64, 175));
  } else {
    SetTextColor(dc, RGB(71, 85, 105));
  }
  return surfaceBrush_;
}

bool App::drawButton(const DRAWITEMSTRUCT& draw) {
  if (draw.hwndItem != selectButton_ && draw.hwndItem != convertButton_) return false;
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
  const bool isPrimary = draw.hwndItem == convertButton_;
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
  const bool draco = SendMessageW(compressCombo_, CB_GETCURSEL, 0, 0) == 1;
  EnableWindow(compressCombo_, binary);
  EnableWindow(dracoLevelEdit_, binary && draco);
  if (!binary) SendMessageW(compressCombo_, CB_SETCURSEL, 0, 0);
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
  SetWindowTextW(qualityHelp_, help);
}

void App::handleCommand(WPARAM wParam) {
  const int id = LOWORD(wParam);
  const int code = HIWORD(wParam);
  if (id == IDC_SELECT && code == BN_CLICKED) chooseInput();
  else if (id == IDC_CONVERT && code == BN_CLICKED) convertSelected();
  else if (id == IDC_PROFILE && code == CBN_SELCHANGE) { applyProfileValues(); updateOptionState(); }
  else if ((id == IDC_FORMAT || id == IDC_COMPRESS || id == IDC_OPTIMIZE) &&
           (code == CBN_SELCHANGE || code == BN_CLICKED)) updateOptionState();
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

void App::selectInputPath(const fs::path& path) {
  const std::string extension = lower(path.extension().string());
  const bool supported = extension == ".step" || extension == ".stp" ||
                         extension == ".igs" || extension == ".iges" ||
                         extension == ".stl";
  std::error_code fileError;
  if (!supported || !fs::is_regular_file(path, fileError)) {
    inputSelected_ = false;
    conversionSucceeded_ = false;
    EnableWindow(convertButton_, FALSE);
    ShowWindow(outputFileLabel_, SW_HIDE);
    ShowWindow(processingTimeLabel_, SW_HIDE);
    setStatus(L"Error: drop a STEP, STP, IGES, or STL file.");
    return;
  }
  selectedInput_ = path;
  SetWindowTextW(inputLabel_, (L"Selected: " + selectedInput_.filename().wstring()).c_str());
  EnableWindow(convertButton_, TRUE);
  inputSelected_ = true;
  conversionSucceeded_ = false;
  ShowWindow(outputFileLabel_, SW_HIDE);
  ShowWindow(processingTimeLabel_, SW_HIDE);
  InvalidateRect(inputLabel_, nullptr, TRUE);
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
                               std::uint64_t outputPolygonOverride) {
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

  std::uint64_t meshCount = 0;
  const Handle(XCAFDoc_ShapeTool) shapeTool = document.IsNull()
      ? Handle(XCAFDoc_ShapeTool)()
      : XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  if (!shapeTool.IsNull()) {
    shapeTool->GetFreeShapes(roots);
    meshCount = roots.Length();
  }
  std::uint64_t faceCount = 0;
  std::uint64_t triangleCount = 0;
  if (!shapeTool.IsNull()) {
    for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
      const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(roots.Value(i));
      for (TopExp_Explorer faces(shape, TopAbs_FACE); faces.More(); faces.Next()) {
        ++faceCount;
        TopLoc_Location location;
        const Handle(Poly_Triangulation) triangulation =
            BRep_Tool::Triangulation(TopoDS::Face(faces.Current()), location);
        if (!triangulation.IsNull()) triangleCount += triangulation->NbTriangles();
      }
    }
  }
  if (outputPolygonOverride != 0) {
    meshCount = meshCount == 0 ? 1 : meshCount;
    faceCount = faceCount == 0 ? outputPolygonOverride : faceCount;
    triangleCount = outputPolygonOverride;
  }

  std::wstringstream quality;
  quality << (options.optimize ? L"optimized profile=" : L"faithful profile=")
          << (options.profile == "large" ? L"large" :
              options.profile == "balanced" ? L"balanced" :
              options.profile == "preview" ? L"preview" :
              options.profile == "custom" ? L"custom" : L"faithful")
          << L"\r\ndeflection=" << options.deflection
          << L" angular=" << options.angular;

  std::wstringstream saved;
  if (inputBytes > 0 && !inputError && !outputError) {
    const double percent = (1.0 - static_cast<double>(outputBytes) /
                                      static_cast<double>(inputBytes)) * 100.0;
    saved << std::fixed << std::setprecision(1) << percent << L"%";
  } else {
    saved << L"n/a";
  }

  const std::wstring values[] = {
      L"INPUT\r\n" + sizeText(inputError ? 0 : inputBytes),
      L"OUTPUT\r\n" + sizeText(outputError ? 0 : outputBytes),
      L"COMPRESSION\r\n" + (options.draco ? L"Draco level " + std::to_wstring(options.dracoLevel) : L"none"),
      L"MESH QUALITY\r\n" + quality.str(),
      L"SIZE SAVED\r\n" + saved.str(),
      L"MESHES\r\n" + std::to_wstring(meshCount),
      L"FACES\r\n" + std::to_wstring(faceCount),
      L"OUTPUT POLYGONS\r\n" + std::to_wstring(triangleCount),
  };
  ShowWindow(conversionInfoHeader_, SW_SHOW);
  for (std::size_t i = 0; i < conversionStats_.size(); ++i) {
    SetWindowTextW(conversionStats_[i], values[i].c_str());
    ShowWindow(conversionStats_[i], SW_SHOW);
  }
  InvalidateRect(window_, nullptr, TRUE);
}

bool App::convertCad(const fs::path& input, const fs::path& output, const ConvertOptions& options,
                     std::string& error) {
  Handle(TDocStd_Document) document;
  XCAFApp_Application::GetApplication()->NewDocument(TCollection_ExtendedString("MDTV-XCAF"), document);

  const std::string extension = lower(input.extension().string());
  if (extension == ".step" || extension == ".stp") {
    STEPCAFControl_Reader reader;
    reader.SetColorMode(true);
    reader.SetNameMode(true);
    if (!reader.Perform(input.string().c_str(), document)) {
      error = "OCCT 8.0.1 STEP Perform failed";
      return false;
    }
  } else if (extension == ".igs" || extension == ".iges") {
    IGESCAFControl_Reader reader;
    reader.SetColorMode(true);
    reader.SetNameMode(true);
    if (!reader.Perform(input.string().c_str(), document)) {
      error = "OCCT 8.0.1 IGES Perform failed";
      return false;
    }
  } else if (extension == ".stl") {
    if (options.binary) {
      try {
        const std::vector<native_stl::Triangle> triangles = native_stl::read(input);
        native_stl::writeGlb(output, triangles);
        updateConversionInfo(input, output, options, document, triangles.size());
        showNativePreview(triangles);
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

  const Handle(XCAFDoc_ShapeTool) shapeTool = XCAFDoc_DocumentTool::ShapeTool(document->Main());
  NCollection_Sequence<TDF_Label> roots;
  shapeTool->GetFreeShapes(roots);
  for (Standard_Integer i = 1; i <= roots.Length(); ++i) {
    TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(roots.Value(i));
    if (!shape.IsNull()) {
      BRepMesh_IncrementalMesh mesh(shape, options.deflection, Standard_False,
                                     options.angular, Standard_True);
      if (!mesh.IsDone()) {
        error = "OCCT triangulation failed";
        return false;
      }
    }
  }

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
  if (!writer.Perform(document, metadata, Message_ProgressRange())) {
    error = "OCCT 8.0.1 GLB export failed";
    return false;
  }
  updateConversionInfo(input, output, options, document);
  showPreview(document);
  return true;
}

ConvertOptions App::optionsFromControls() const {
  ConvertOptions options;
  options.binary = SendMessageW(formatCombo_, CB_GETCURSEL, 0, 0) == 0;
  options.colorsOnly = SendMessageW(appearanceCombo_, CB_GETCURSEL, 0, 0) == 0;
  options.draco = options.binary && SendMessageW(compressCombo_, CB_GETCURSEL, 0, 0) == 1;
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
  if (selectedInput_.empty()) {
    inputSelected_ = false;
    conversionSucceeded_ = false;
    ShowWindow(outputFileLabel_, SW_HIDE);
    ShowWindow(processingTimeLabel_, SW_HIDE);
    setStatus(L"Error: select a STEP, IGES, or STL model first.");
    return;
  }
  const ConvertOptions options = optionsFromControls();
  const std::wstring suffix = options.binary ? L".glb" : L".gltf";
  fs::path output = selectedInput_.parent_path() /
                    (selectedInput_.stem().wstring() + L"-converted" + suffix);
  std::string error;
  std::wstringstream status;
  const bool nativeStl = lower(selectedInput_.extension().string()) == ".stl" && options.binary;
  status << (nativeStl ? L"Converting with native C++ STL pipeline - " : L"Converting with OCCT 8.0.1 - ")
         << (options.binary ? L"GLB" : L"GLTF")
         << L" - " << (options.profile == "faithful" ? L"CAD faithful" :
                         options.profile == "balanced" ? L"Viewer balanced" :
                         options.profile == "preview" ? L"Fast preview" :
                         options.profile == "custom" ? L"Custom" : L"Large assembly")
         << L" - deflection " << options.deflection << L" - angular " << options.angular;
  if (options.draco) status << L" | Draco " << options.dracoLevel;
  setStatus(status.str());
  EnableWindow(convertButton_, FALSE);
  const auto conversionStart = std::chrono::steady_clock::now();
  const bool converted = convertCad(selectedInput_, output, options, error);
  const auto conversionEnd = std::chrono::steady_clock::now();
  const double processingSeconds = std::chrono::duration<double>(conversionEnd - conversionStart).count();
  if (!converted) {
    conversionSucceeded_ = false;
    ShowWindow(outputFileLabel_, SW_HIDE);
    ShowWindow(processingTimeLabel_, SW_HIDE);
    setStatus(L"Error: " + std::wstring(error.begin(), error.end()));
    EnableWindow(convertButton_, TRUE);
    return;
  }
  conversionSucceeded_ = true;
  SetWindowTextW(outputFileLabel_, (L"Output file: " + output.filename().wstring()).c_str());
  ShowWindow(outputFileLabel_, SW_SHOW);
  std::wstringstream processing;
  processing << L"Processing time: " << std::fixed << std::setprecision(3)
             << processingSeconds << L" s";
  SetWindowTextW(processingTimeLabel_, processing.str().c_str());
  ShowWindow(processingTimeLabel_, SW_SHOW);
  std::wstring result = L"Done - " + output.filename().wstring() + L" - ready to preview";
  setStatus(result);
  EnableWindow(convertButton_, TRUE);
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
      case WM_SIZE:
        g_app->resizePreview();
        break;
      case WM_DESTROY:
        g_app->releaseThemeResources();
        PostQuitMessage(0);
        return 0;
    }
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  App app(instance);
  fs::path autoInput;
  const std::wstring command(commandLine ? commandLine : L"");
  if (command.rfind(L"--stl-test ", 0) == 0) autoInput = command.substr(11);
  const int result = app.run(autoInput);
  CoUninitialize();
  return result;
}
