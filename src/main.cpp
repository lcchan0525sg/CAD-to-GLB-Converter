#include <windows.h>
#include <shobjidl.h>
#include <wrl.h>

#include <AIS_InteractiveContext.hxx>
#include <AIS_ViewController.hxx>
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
#include <Standard_Failure.hxx>
#include <TCollection_AsciiString.hxx>
#include <TCollection_ExtendedString.hxx>
#include <TDocStd_Document.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>
#include <WNT_Window.hxx>
#include <XCAFPrs_AISObject.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cwchar>
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
  int run();

private:
  static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK previewProc(HWND, UINT, WPARAM, LPARAM);
  bool createWindow();
  void createControls();
  void initializePreview();
  void resizePreview();
  void showPreview(const Handle(TDocStd_Document)& document);
  bool handlePreviewMessage(HWND, UINT, WPARAM, LPARAM);
  void setStatus(const std::wstring& message);
  void updateOptionState();
  void applyProfileValues();
  void handleCommand(WPARAM wParam);
  void chooseInput();
  void convertSelected();
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
  HWND previewPanel_{};
  HFONT font_{};
  Handle(Aspect_DisplayConnection) displayConnection_;
  Handle(Graphic3d_GraphicDriver) graphicDriver_;
  Handle(V3d_Viewer) viewer_;
  Handle(V3d_View) view_;
  Handle(AIS_InteractiveContext) previewContext_;
  AIS_ViewController previewController_;
  Handle(WNT_Window) previewWindow_;
  std::vector<Handle(XCAFPrs_AISObject)> previewObjects_;
  WNDPROC previewOriginalProc_{};
  POINT previewLastPoint_{};
  bool previewRotating_ = false;
  bool previewPanning_ = false;
  fs::path selectedInput_;
};

static App* g_app = nullptr;

int App::run() {
  g_app = this;
  if (!createWindow()) return 1;
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
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

  window_ = CreateWindowExW(0, wc.lpszClassName, L"CAD Converter 2 - Interactive Preview - OCCT 8.0.1",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            1440, 780, nullptr, nullptr, instance_, nullptr);
  if (!window_) return false;
  createControls();
  ShowWindow(window_, SW_SHOW);
  UpdateWindow(window_);
  return true;
}

void App::createControls() {
  font_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
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

  label(L"CAD Converter 2", 32, 24, 500, 34);
  label(L"Native Windows desktop app - Open CASCADE Technology 8.0.1", 32, 58, 700, 24);
  inputLabel_ = label(L"No model selected", 32, 98, 780, 28);
  selectButton_ = add(0, L"BUTTON", L"Choose CAD file", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      32, 132, 150, 30, IDC_SELECT);
  convertButton_ = add(0, L"BUTTON", L"Convert", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                       192, 132, 120, 30, IDC_CONVERT);

  label(L"Output format", 32, 184, 180, 22);
  formatCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                     32, 208, 360, 180, IDC_FORMAT);
  SendMessageW(formatCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GLB - binary single file"));
  SendMessageW(formatCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GLTF - JSON + BIN"));
  SendMessageW(formatCombo_, CB_SETCURSEL, 0, 0);

  label(L"Appearance", 432, 184, 180, 22);
  appearanceCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                         432, 208, 360, 180, IDC_APPEARANCE);
  SendMessageW(appearanceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Colors only"));
  SendMessageW(appearanceCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Preserve textures"));
  SendMessageW(appearanceCombo_, CB_SETCURSEL, 0, 0);

  label(L"Compression", 32, 254, 180, 22);
  compressCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                       32, 278, 360, 180, IDC_COMPRESS);
  SendMessageW(compressCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
  SendMessageW(compressCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Draco"));
  SendMessageW(compressCombo_, CB_SETCURSEL, 0, 0);

  label(L"Draco level (0-10)", 432, 254, 180, 22);
  dracoLevelEdit_ = add(WS_EX_CLIENTEDGE, L"EDIT", L"7",
                        WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                        432, 278, 360, 28, IDC_DRACO_LEVEL);

  optimizeCheck_ = add(0, L"BUTTON", L"Optimize mesh", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        32, 328, 180, 26, IDC_OPTIMIZE);
  SendMessageW(optimizeCheck_, BM_SETCHECK, BST_CHECKED, 0);

  label(L"Quality profile", 432, 324, 180, 22);
  profileCombo_ = add(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                      432, 348, 360, 180, IDC_PROFILE);
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Large assembly - recommended"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Viewer balanced"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"CAD faithful"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Fast preview"));
  SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom"));
  SendMessageW(profileCombo_, CB_SETCURSEL, 0, 0);

  label(L"Deflection", 32, 382, 180, 22);
  deflectionEdit_ = add(WS_EX_CLIENTEDGE, L"EDIT", L"1.00",
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                        32, 406, 360, 28, IDC_DEFLECTION);
  label(L"Angular tolerance", 432, 382, 180, 22);
  angularEdit_ = add(WS_EX_CLIENTEDGE, L"EDIT", L"1.00",
                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                     432, 406, 360, 28, IDC_ANGULAR);

  qualityHelp_ = label(L"Large assembly: 1.00 deflection / 1.00 angular. Fewer triangles and faster browser interaction.",
                       32, 450, 760, 42);
  statusLabel_ = label(L"Ready. Select a STEP or IGES file.", 32, 510, 900, 54);
  label(L"Conversion runs in this native process. No browser, server, Python, or Docker is used.",
        32, 580, 900, 24);
  label(L"3D Preview", 830, 24, 500, 28);
  previewPanel_ = add(WS_EX_CLIENTEDGE, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_BLACKRECT | SS_NOTIFY,
                       830, 58, 560, 620, 0);
  label(L"Left drag: rotate  |  Middle drag: pan  |  Wheel: zoom", 830, 690, 560, 24);
  updateOptionState();
  initializePreview();
}

void App::initializePreview() {
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
  if (!view_.IsNull()) {
    view_->MustBeResized();
    view_->Redraw();
  }
}

void App::showPreview(const Handle(TDocStd_Document)& document) {
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

bool App::handlePreviewMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
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

LRESULT CALLBACK App::previewProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (g_app && g_app->handlePreviewMessage(hwnd, message, wParam, lParam)) return 0;
  if (g_app && g_app->previewOriginalProc_) {
    return CallWindowProcW(g_app->previewOriginalProc_, hwnd, message, wParam, lParam);
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
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
  selectedInput_ = path;
  CoTaskMemFree(path);
  SetWindowTextW(inputLabel_, (L"Selected: " + selectedInput_.filename().wstring()).c_str());
  EnableWindow(convertButton_, TRUE);
  setStatus(L"Ready to convert with the selected options.");
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
  } else {
    error = "unsupported input; choose STEP or IGES";
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
    setStatus(L"Error: select a STEP or IGES model first.");
    return;
  }
  const ConvertOptions options = optionsFromControls();
  const std::wstring suffix = options.binary ? L".glb" : L".gltf";
  fs::path output = selectedInput_.parent_path() /
                    (selectedInput_.stem().wstring() + L"-converted" + suffix);
  std::string error;
  std::wstringstream status;
  status << L"Converting with OCCT 8.0.1 - " << (options.binary ? L"GLB" : L"GLTF")
         << L" - " << (options.profile == "faithful" ? L"CAD faithful" :
                         options.profile == "balanced" ? L"Viewer balanced" :
                         options.profile == "preview" ? L"Fast preview" :
                         options.profile == "custom" ? L"Custom" : L"Large assembly")
         << L" - deflection " << options.deflection << L" - angular " << options.angular;
  if (options.draco) status << L" · Draco " << options.dracoLevel;
  setStatus(status.str());
  EnableWindow(convertButton_, FALSE);
  if (!convertCad(selectedInput_, output, options, error)) {
    setStatus(L"Error: " + std::wstring(error.begin(), error.end()));
    EnableWindow(convertButton_, TRUE);
    return;
  }
  std::wstring result = L"Conversion complete:\n" + output.wstring();
  if (options.colorsOnly) result += L"\nAppearance: colors only";
  if (options.draco) result += L"\nCompression: Draco level " + std::to_wstring(options.dracoLevel);
  setStatus(result);
  EnableWindow(convertButton_, TRUE);
}

LRESULT CALLBACK App::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_COMMAND && g_app) g_app->handleCommand(wParam);
  if (message == WM_SIZE && g_app) g_app->resizePreview();
  if (message == WM_DESTROY) PostQuitMessage(0);
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  App app(instance);
  const int result = app.run();
  CoUninitialize();
  return result;
}
