// TexPeek - texture görüntüleyici
// Bu dosya: pencereyi açar, DirectX 11'i hazırlar, her karede arayüzü çizer.

#include "Lang.h"
#include "ShellIntegration.h"
#include "Viewer.h"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <shellapi.h>
#include <memory>
#include <string>

// ---- DirectX 11 nesneleri ----
static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static IDXGISwapChain*         g_swapChain = nullptr;
static ID3D11RenderTargetView* g_renderTarget = nullptr;
static bool                    g_swapChainOccluded = false;
static UINT                    g_resizeWidth = 0, g_resizeHeight = 0;

// Sürükle-bırak ile gelen dosyalar (bir sonraki karede açılır)
static std::wstring            g_pendingOpen;      // ana görüntü (A)
static std::wstring            g_pendingCompare;   // karşılaştırma görüntüsü (B)

static bool CreateDeviceD3D(HWND hwnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Arayüzün görünümü: sakin koyu tema, yuvarlatılmış köşeler, tek bir vurgu rengi (mavi).
// Vurgu rengi Viewer.cpp'deki kAccent ile aynı.
static void ApplyTheme()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(9, 5);
    style.ItemSpacing = ImVec2(8, 7);
    style.ItemInnerSpacing = ImVec2(6, 5);
    style.FrameRounding = 5.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.ScrollbarSize = 11.0f;
    style.GrabMinSize = 10.0f;
    style.FrameBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;

    const ImVec4 accent(0.26f, 0.53f, 0.96f, 1.00f);
    const ImVec4 accentSoft(0.26f, 0.53f, 0.96f, 0.45f);
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.54f, 0.56f, 0.61f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.105f, 0.110f, 0.125f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.135f, 0.155f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.22f, 0.23f, 0.27f, 1.00f);
    c[ImGuiCol_Separator] = ImVec4(0.19f, 0.20f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.165f, 0.172f, 0.195f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.205f, 0.215f, 0.245f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.235f, 0.245f, 0.280f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.185f, 0.193f, 0.220f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.245f, 0.255f, 0.290f, 1.00f);
    c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = ImVec4(0.150f, 0.157f, 0.180f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.195f, 0.205f, 0.235f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.225f, 0.235f, 0.270f, 1.00f);
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.67f, 1.00f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.25f, 0.29f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.30f, 0.31f, 0.36f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.36f, 0.38f, 0.43f, 1.00f);
    c[ImGuiCol_TextSelectedBg] = accentSoft;
    c[ImGuiCol_NavCursor] = accent;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // PNG / JPG okuma (WIC) COM ister
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)))
        return 1;

    InitLang();   // arayüz dili: kayıtlı seçim, yoksa Windows'un dili

    // texpeek.exe --register / --unregister: pencere açmadan Windows kayıtlarını kur ya da kaldır
    // (programın içindeki "Windows > Kur" düğmesiyle aynı iş; kurulum betikleri için).
    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
        const std::wstring first = (argv && argc > 1) ? argv[1] : L"";
        if (argv)
            ::LocalFree(argv);
        if (first == L"--register" || first == L"--unregister")
        {
            std::string message;
            const bool ok = first == L"--register" ? ShellIntegration::Install(message) : ShellIntegration::Uninstall(message);
            CoUninitialize();
            return ok ? 0 : 1;
        }
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    const float dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    // Pencere ve görev çubuğu simgesi: exe'nin içine gömülü 1 numaralı simge (res/texpeek.ico, bkz. res/version.rc.in)
    const HICON appIcon = ::LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, appIcon, ::LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"TexPeekWindow", appIcon };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"TexPeek", WS_OVERLAPPEDWINDOW,
        100, 100, (int)(1280 * dpiScale), (int)(800 * dpiScale), nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, hInstance);
        ::MessageBoxW(nullptr, Tr(L"DirectX 11 başlatılamadı.", L"Could not start DirectX 11."), L"TexPeek", MB_ICONERROR);
        return 1;
    }

    ::DragAcceptFiles(hwnd, TRUE);   // pencereye dosya sürüklenebilsin
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // ---- ImGui kurulumu ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // imgui.ini dosyası oluşturma

    ApplyTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;

    // Windows'un Segoe UI yazı tipi (Türkçe karakterleri de içerir)
    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (::GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF(fontPath, 17.0f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    auto viewer = std::make_unique<Viewer>(g_device, hwnd);

    // texpeek.exe dosya.dds şeklinde açıldıysa (ileride çift tıklama için).
    // İkinci bir dosya verildiyse karşılaştırma görüntüsü olarak açılır: texpeek.exe a.dds b.dds
    int argc = 0;
    if (LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc))
    {
        if (argc > 1)
            viewer->Open(argv[1]);
        if (argc > 2)
            viewer->OpenCompare(argv[2]);
        ::LocalFree(argv);
    }

    // ---- Ana döngü ----
    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Pencere simge durumundaysa boşuna çizme
        if (g_swapChainOccluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_swapChainOccluded = false;

        if (g_resizeWidth != 0 && g_resizeHeight != 0)
        {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateRenderTarget();
        }

        if (!g_pendingOpen.empty())
        {
            viewer->Open(g_pendingOpen);
            g_pendingOpen.clear();
        }
        if (!g_pendingCompare.empty())
        {
            viewer->OpenCompare(g_pendingCompare);
            g_pendingCompare.clear();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        viewer->Draw();

        ImGui::Render();
        const float clearColor[4] = { 0.11f, 0.11f, 0.125f, 1.0f };
        g_context->OMSetRenderTargets(1, &g_renderTarget, nullptr);
        g_context->ClearRenderTargetView(g_renderTarget, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_swapChain->Present(1, 0);   // vsync açık
        g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    viewer.reset();   // texture'lar DirectX kapanmadan önce serbest bırakılsın

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, hInstance);
    CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// DirectX 11 yardımcıları
// ---------------------------------------------------------------------------

static bool CreateDeviceD3D(HWND hwnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
        D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // ekran kartı yoksa yazılımsal sürücüyü dene
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
            D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &level, &g_context);
    if (hr != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context)   { g_context->Release();   g_context = nullptr; }
    if (g_device)    { g_device->Release();    g_device = nullptr; }
}

static void CreateRenderTarget()
{
    ID3D11Texture2D* backBuffer = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTarget);
    backBuffer->Release();
}

static void CleanupRenderTarget()
{
    if (g_renderTarget) { g_renderTarget->Release(); g_renderTarget = nullptr; }
}

// ---------------------------------------------------------------------------
// Windows mesajları
// ---------------------------------------------------------------------------

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_resizeWidth = (UINT)LOWORD(lParam);
        g_resizeHeight = (UINT)HIWORD(lParam);
        return 0;

    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wParam;
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        wchar_t path[MAX_PATH] = {};
        if (count > 0 && ::DragQueryFileW(drop, 0, path, MAX_PATH))
        {
            // Shift basılıyken bırakılan dosya karşılaştırma görüntüsü (B) olur
            if (::GetKeyState(VK_SHIFT) < 0)
                g_pendingCompare = path;
            else
            {
                g_pendingOpen = path;
                // İki dosya birlikte bırakıldıysa ikincisi B olur (fazlası yok sayılır)
                if (count > 1 && ::DragQueryFileW(drop, 1, path, MAX_PATH))
                    g_pendingCompare = path;
            }
        }
        ::DragFinish(drop);
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)   // Alt tuşu menüsünü kapat
            return 0;
        break;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
