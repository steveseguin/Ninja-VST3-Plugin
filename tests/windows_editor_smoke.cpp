// Load the shipped module and exercise its real Win32 editor without a DAW.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <public.sdk/source/vst/hosting/module.h>
#include <public.sdk/source/vst/hosting/plugprovider.h>
#include <public.sdk/source/common/memorystream.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/gui/iplugviewcontentscalesupport.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include "../webrtc_vst/src/ParameterIDs.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Steinberg;
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
void pump(int milliseconds = 150) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    do {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    } while (std::chrono::steady_clock::now() < end);
}
HWND editorWindow(HWND parent) {
    HWND child = GetWindow(parent, GW_CHILD);
    require(child != nullptr, "Editor did not create a native child window");
    return child;
}
void click(HWND parent, int x, int y) {
    HWND child = editorWindow(parent);
    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(child, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    pump();
}
std::wstring editText(HWND parent, int x, int y, const wchar_t* replacement = nullptr) {
    click(parent, x, y);
    HWND edit = GetFocus();
    wchar_t name[128]{};
    GetClassNameW(edit, name, 128);
    require(_wcsicmp(name, L"EDIT") == 0, "Field did not become an editable native text control");
    wchar_t value[512]{};
    GetWindowTextW(edit, value, 512);
    if (replacement) {
        SetWindowTextW(edit, replacement);
        SendMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
        pump();
    }
    return value;
}
void capture(HWND window, const std::filesystem::path& path) {
    pump(350);
    RECT rect{};
    GetClientRect(window, &rect);
    HDC source = GetDC(window), memory = CreateCompatibleDC(source);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right;
    info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    require(bitmap && pixels, "Cannot allocate screenshot bitmap");
    auto old = SelectObject(memory, bitmap);
    const auto* colors = static_cast<const uint32_t*>(pixels);
    int different = 0;
    // Direct2D may not have presented its first frame yet on a busy desktop.
    // Wait for actual pixels with a bound; a blank editor must still fail.
    for (int attempt = 0; attempt < 5; ++attempt) {
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        pump(100);
        require(PrintWindow(window, memory, PW_CLIENTONLY | 2 /* PW_RENDERFULLCONTENT */) != 0,
                "Could not capture editor pixels");
        GdiFlush();
        different = 0;
        const auto background = colors[0] & 0x00ffffff;
        for (int y = 0; y < rect.bottom; y += 5)
            for (int x = 0; x < rect.right; x += 5)
                different += (colors[y * rect.right + x] & 0x00ffffff) != background;
        if (different > 300) break;
    }
    if (!path.empty()) {
        UINT count = 0, bytes = 0;
        Gdiplus::GetImageEncodersSize(&count, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto* codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
        Gdiplus::GetImageEncoders(count, bytes, codecs);
        bool saved = false;
        {
            Gdiplus::Bitmap image(bitmap, nullptr);
            for (UINT i = 0; i < count; ++i) {
                if (wcscmp(codecs[i].MimeType, L"image/png") == 0) {
                    saved = image.Save(path.c_str(), &codecs[i].Clsid) == Gdiplus::Ok;
                    break;
                }
            }
        }
        require(saved, "Could not save editor screenshot");
    }
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(window, source);
    require(different > 300, "Editor capture is blank");
}
class Frame final : public IPlugFrame {
public:
    HWND window{};
    tresult PLUGIN_API queryInterface(const TUID, void** obj) override {
        *obj = nullptr; return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override {
        RECT outer{0, 0, size->getWidth(), size->getHeight()};
        AdjustWindowRectEx(&outer, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
        SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return view->onSize(size);
    }
};
}

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Gdiplus::GdiplusStartupInput graphics;
    ULONG_PTR graphicsToken{};
    Gdiplus::GdiplusStartup(&graphicsToken, &graphics, nullptr);
    int result = 0;
    try {
        const std::filesystem::path gallery = argc > 2
            ? std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[2]))) : std::filesystem::path{};
        if (!gallery.empty()) std::filesystem::create_directories(gallery);
        static Vst::HostApplication host;
        Vst::PluginContextFactory::instance().setPluginContext(&host);
        std::string error;
        auto module = VST3::Hosting::Module::create(argc > 1 ? argv[1] : WEBRTC_VST_DEFAULT_PLUGIN_PATH, error);
        require(module != nullptr, error.c_str());
        auto classes = module->getFactory().classInfos();
        require(!classes.empty(), "No plugin classes");
        Vst::PlugProvider provider(module->getFactory(), classes.front(), true);
        require(provider.initialize(), "Cannot initialize plugin");
        auto controller = provider.getControllerPtr();
        require(controller != nullptr, "No controller");
        std::string state = R"({"streamId":"gallery-demo","roomName":"","password":"","mode":"play"})";
        MemoryStream stream(state.data(), static_cast<TSize>(state.size()));
        require(controller->setComponentState(&stream) == kResultOk, "Cannot set demo state");

        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"WebRTCEditorSmokeHost";
        RegisterClassW(&wc);
        for (int cycle = 0; cycle < 5; ++cycle) {
            IPtr<IPlugView> view(controller->createView(Vst::ViewType::kEditor), false);
            require(view != nullptr, "No plugin editor");
            require(view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue, "HWND editor unsupported");
            ViewRect size;
            require(view->getSize(&size) == kResultOk, "Cannot query editor size before attach");
            std::cout << "Editor before attach: " << size.getWidth() << 'x' << size.getHeight() << '\n';
            require(size.getWidth() == 720 && size.getHeight() == 340,
                    "Expected 720x340 editor before attach; UI resource may be missing");
            Frame frame;
            frame.window = CreateWindowW(wc.lpszClassName, L"VDO.Ninja WebRTC Bridge - native VST3 test host",
                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, 80, 80, 740, 380, nullptr, nullptr, wc.hInstance, nullptr);
            require(frame.window != nullptr, "Cannot create host window");
            view->setFrame(&frame);
            frame.resizeView(view, &size);
            require(view->attached(frame.window, kPlatformTypeHWND) == kResultOk, "Cannot attach editor");
            ShowWindow(frame.window, SW_SHOWNORMAL);
            // execute_process/CI can inherit STARTF_USESHOWWINDOW=SW_HIDE;
            // Windows consumes that override on the first ShowWindow call.
            if (!IsWindowVisible(frame.window)) ShowWindow(frame.window, SW_SHOWNORMAL);
            require(IsWindowVisible(frame.window) != 0, "Host window did not become visible");
            SetForegroundWindow(frame.window);
            view->onFocus(true);
            pump(400);
            std::cout << "Attached cycle " << cycle << '\n';
            auto shot = [&](const char* name) {
                capture(frame.window, cycle == 0 && !gallery.empty() ? gallery / name : std::filesystem::path{});
            };
            shot("play.png");
            std::cout << "Rendered Play\n";
            for (int toggle = 0; toggle < 4; ++toggle) {
                click(frame.window, 650, 30);
                require(editText(frame.window, 450, 110) == L"https://vdo.ninja/", "Wrong advanced domain field");
                click(frame.window, 180, 75); // commit/blur the text edit
                if (toggle == 0) shot("advanced.png");
                click(frame.window, 650, 30);
            }
            std::cout << "Advanced controls passed\n";
            require(editText(frame.window, 350, 110, L"gallery-demo") == L"gallery-demo", "Stream field is not editable");
            controller->setParamNormalized(webrtc_vst::kParamMode, 1.0);
            pump(400);
            shot("publish.png");
            std::cout << "Rendered Publish\n";
            controller->setParamNormalized(webrtc_vst::kParamMode, 0.0);
            // Exercise host content scaling and size negotiation, including tiny host requests.
            FUnknownPtr<IPlugViewContentScaleSupport> scale(view);
            require(scale != nullptr, "No host content-scale support");
            for (float factor : {1.25f, 1.5f, 2.0f, 1.0f}) {
                require(scale->setContentScaleFactor(factor) == kResultOk, "Content scaling failed");
                ViewRect requested(0, 0, 64, 64);
                require(view->checkSizeConstraint(&requested) == kResultOk, "Size constraint failed");
                require(requested.getWidth() == static_cast<int>(720 * factor) &&
                        requested.getHeight() == static_cast<int>(340 * factor), "Host got an undersized editor");
                frame.resizeView(view, &requested);
                pump();
            }
            view->onFocus(false);
            std::cout << "Scaling passed\n";
            require(view->removed() == kResultOk, "Cannot remove editor");
            view->setFrame(nullptr);
            DestroyWindow(frame.window);
        }
        std::cout << "PASS: Windows editor size, drawing, fields, Advanced toggle, DPI and five reopen cycles\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    Gdiplus::GdiplusShutdown(graphicsToken);
    CoUninitialize();
    return result;
}
