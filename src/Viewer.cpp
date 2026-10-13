#include "Viewer.h"
#include "FormatNames.h"
#include "Lang.h"
#include "ShellIntegration.h"
#include "imgui_impl_dx11.h"

#include <commdlg.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    // Windows geniş karakterli metni (UTF-16) ImGui'nin kullandığı UTF-8'e çevirir.
    std::string ToUtf8(const std::wstring& w)
    {
        if (w.empty())
            return {};
        const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
        return s;
    }

    std::wstring FileNameOf(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1);
    }

    const char* CubeFaceName(int face)
    {
        static const char* tr[6] = { "+X (sağ)", "-X (sol)", "+Y (üst)", "-Y (alt)", "+Z (ön)", "-Z (arka)" };
        static const char* en[6] = { "+X (right)", "-X (left)", "+Y (top)", "-Y (bottom)", "+Z (front)", "-Z (back)" };
        return (CurrentLang() == Lang::Turkish ? tr : en)[face % 6];
    }

    ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return { a.x + b.x, a.y + b.y }; }
    ImVec2 operator-(const ImVec2& a, const ImVec2& b) { return { a.x - b.x, a.y - b.y }; }
    ImVec2 operator*(const ImVec2& a, float f) { return { a.x * f, a.y * f }; }

    std::wstring StemOf(const std::wstring& path)
    {
        const std::wstring name = FileNameOf(path);
        const size_t dot = name.find_last_of(L'.');
        return dot == std::wstring::npos ? name : name.substr(0, dot);
    }

    // ShaderParams::flags bitleri (HLSL tarafındaki sayılarla aynı)
    enum ShaderFlags
    {
        FlagLinearToSrgb = 1,
        FlagToneMap = 2,
        FlagAlphaTest = 4,
        FlagFlipGreen = 8,
        FlagSigned = 16,
        FlagPano = 32,
    };

    const float kPi = 3.14159265358979f;

    // Alfa kanalı olan görüntüler için dama tahtası arka plan. Kareler imgMin'e göre hizalanır,
    // yalnızca ekranda görünen visMin..visMax arası çizilir.
    void DrawCheckerboard(ImDrawList* dl, const ImVec2& imgMin, const ImVec2& visMin, const ImVec2& visMax)
    {
        const float cell = 10.0f;
        dl->AddRectFilled(visMin, visMax, IM_COL32(100, 100, 100, 255));
        const int x0 = (int)std::floor((visMin.x - imgMin.x) / cell);
        const int y0 = (int)std::floor((visMin.y - imgMin.y) / cell);
        const int x1 = (int)std::ceil((visMax.x - imgMin.x) / cell);
        const int y1 = (int)std::ceil((visMax.y - imgMin.y) / cell);
        for (int y = y0; y < y1; ++y)
            for (int x = x0 + ((x0 + y) & 1); x < x1; x += 2)
            {
                ImVec2 a(imgMin.x + x * cell, imgMin.y + y * cell);
                ImVec2 b(a.x + cell, a.y + cell);
                a.x = std::max(a.x, visMin.x); a.y = std::max(a.y, visMin.y);
                b.x = std::min(b.x, visMax.x); b.y = std::min(b.y, visMax.y);
                if (a.x < b.x && a.y < b.y)
                    dl->AddRectFilled(a, b, IM_COL32(150, 150, 150, 255));
            }
    }

    // Bir görüntünün karolarını çizer (çok büyük görüntüler karolara bölünmüş olabilir).
    // origin: görüntünün ekrandaki sol üst köşesi, scale: görüntünün bir pikseli ekranda kaç piksel.
    void DrawTiles(ImDrawList* dl, const TextureDoc& doc, const ImVec2& origin, const ImVec2& scale,
        const ImVec2& visMin, const ImVec2& visMax)
    {
        for (const ViewTile& tile : doc.Tiles())
        {
            const ImVec2 a(origin.x + (float)tile.x * scale.x, origin.y + (float)tile.y * scale.y);
            const ImVec2 b(origin.x + (float)(tile.x + tile.width) * scale.x, origin.y + (float)(tile.y + tile.height) * scale.y);
            if (b.x < visMin.x || b.y < visMin.y || a.x > visMax.x || a.y > visMax.y)
                continue;
            dl->AddImage((ImTextureID)(intptr_t)tile.view, a, b);
        }
    }

    // Tuvalin üzerine koyu zeminli küçük bir etiket ("A", "B" gibi) yazar.
    void DrawLabel(ImDrawList* dl, const ImVec2& pos, const char* text)
    {
        const ImVec2 pad(ImGui::GetFontSize() * 0.45f, ImGui::GetFontSize() * 0.2f);
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddRectFilled(pos, ImVec2(pos.x + ts.x + pad.x * 2.0f, pos.y + ts.y + pad.y * 2.0f), IM_COL32(15, 15, 18, 220), 4.0f);
        dl->AddText(pos + pad, IM_COL32(235, 235, 240, 255), text);
    }

    // Piksel değerini durum çubuğu için metne çevirir.
    void FormatPixelValues(char* out, size_t size, DXGI_FORMAT format, bool valid, const float px[4])
    {
        if (!valid)
        {
            std::snprintf(out, size, "%s", Tr("(bu formatta değer okunamıyor)", "(values cannot be read in this format)"));
            return;
        }
        // 8-bit formatlarda alışık olunan 0-255 karşılığını da göster
        const bool showBytes = DirectX::FormatDataType(format) == DirectX::FORMAT_TYPE_UNORM &&
            (DirectX::IsCompressed(format) || DirectX::BitsPerColor(format) <= 8);
        if (showBytes)
        {
            int b[4];
            for (int i = 0; i < 4; ++i)
                b[i] = (int)std::lround(std::clamp(px[i], 0.0f, 1.0f) * 255.0f);
            std::snprintf(out, size, "R %d  G %d  B %d  A %d     (%.3f  %.3f  %.3f  %.3f)",
                b[0], b[1], b[2], b[3], px[0], px[1], px[2], px[3]);
        }
        else
            std::snprintf(out, size, "R %.4f  G %.4f  B %.4f  A %.4f", px[0], px[1], px[2], px[3]);
    }

    // Bakış yönü + ekrandaki konum (0..1) -> equirectangular görüntüdeki konum (0..1).
    // Shader'daki PanoUv ile aynı hesap; fare altındaki pikseli bulmak için CPU'da da gerekiyor.
    ImVec2 PanoUv(const ImVec2& uv, float yaw, float pitch, float tanHalfFov, float aspect)
    {
        float x = (uv.x * 2.0f - 1.0f) * tanHalfFov * aspect;
        float y = -(uv.y * 2.0f - 1.0f) * tanHalfFov;
        float z = 1.0f;
        const float len = std::sqrt(x * x + y * y + z * z);
        x /= len; y /= len; z /= len;

        const float sp = std::sin(pitch), cp = std::cos(pitch);
        const float y1 = y * cp + z * sp, z1 = z * cp - y * sp;
        const float sy = std::sin(yaw), cy = std::cos(yaw);
        const float x2 = x * cy + z1 * sy, z2 = z1 * cy - x * sy;

        return { std::atan2(x2, z2) / (2.0f * kPi) + 0.5f, std::acos(std::clamp(y1, -1.0f, 1.0f)) / kPi };
    }

    // Görüntüyü çizen pixel shader. Girdi düzeni ImGui'nin kendi vertex shader'ının çıktısıyla aynı.
    const char* kImageShader = R"(
cbuffer Params : register(b0)
{
    int channel;         // 0 RGBA, 1 RGB, 2 R, 3 G, 4 B, 5 A
    int normalMode;      // 0 kapalı, 1 Z'yi R,G'den hesapla, 2 Z'yi A,G'den hesapla
    int flags;
    float exposure;      // çarpan (2^EV)
    float alphaThreshold;
    float yaw;           // 360° görünüm: bakış yönü (radyan)
    float pitch;
    float tanHalfFov;
    float aspect;        // tuval genişliği / yüksekliği
    int compareMode;     // 0 yok, 1 texture1'e doğru soldur, 2 texture1 ile farkı göster
    float compareAmount; // soldurma oranı (0..1) ya da fark çarpanı
    float padding;
};

struct PS_INPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

sampler sampler0;
Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);   // karşılaştırmanın ikinci görüntüsü (B)

float3 ToneMapAces(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float3 LinearToSrgb(float3 x)
{
    x = saturate(x);
    return x <= 0.0031308 ? x * 12.92 : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}

// 360° görünüm: ekrandaki nokta -> kameranın o noktadan baktığı yön -> görüntüdeki konum
float2 PanoUv(float2 uv)
{
    float2 ndc = uv * 2.0 - 1.0;
    float3 d = normalize(float3(ndc.x * tanHalfFov * aspect, -ndc.y * tanHalfFov, 1.0));
    float sp = sin(pitch), cp = cos(pitch);
    d = float3(d.x, d.y * cp + d.z * sp, d.z * cp - d.y * sp);
    float sy = sin(yaw), cy = cos(yaw);
    d = float3(d.x * cy + d.z * sy, d.y, d.z * cy - d.x * sy);
    return float2(atan2(d.x, d.z) / 6.2831853 + 0.5, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265);
}

float4 main(PS_INPUT input) : SV_Target
{
    float2 uv = input.uv;
    if (flags & 32)
        uv = PanoUv(uv);
    float4 c = texture0.SampleLevel(sampler0, uv, 0);
    if (compareMode == 1)
        c = lerp(c, texture1.SampleLevel(sampler0, uv, 0), compareAmount);
    else if (compareMode == 2)
        c = abs(c - texture1.SampleLevel(sampler0, uv, 0)) * compareAmount;
    const float sourceAlpha = c.a;

    if (normalMode != 0)
    {
        // İki kanallı normal map: X ve Y saklanır, Z uzunluk 1 olacak şekilde hesaplanır.
        float2 n = normalMode == 2 ? c.ag : c.rg;
        if ((flags & 16) == 0)
            n = n * 2.0 - 1.0;
        if (flags & 8)
            n.y = -n.y;
        const float z = sqrt(saturate(1.0 - dot(n, n)));
        c = float4(float3(n, z) * 0.5 + 0.5, 1.0);
    }
    else if (flags & 8)
    {
        c.g = 1.0 - c.g;
    }

    float3 rgb = c.rgb;
    float alpha = c.a;
    if (channel == 2) rgb = c.rrr;
    else if (channel == 3) rgb = c.ggg;
    else if (channel == 4) rgb = c.bbb;
    else if (channel == 5) rgb = sourceAlpha.xxx;
    if (channel != 0)
        alpha = 1.0;

    if (channel != 5)
    {
        rgb *= exposure;
        if (flags & 2) rgb = ToneMapAces(rgb);
        if (flags & 1) rgb = LinearToSrgb(rgb);
    }

    if (flags & 4)
    {
        // Oyundaki "alpha test": eşiğin altı hiç çizilmez, üstü tamamen opak çizilir.
        if (sourceAlpha < alphaThreshold)
            discard;
        alpha = 1.0;
    }

    // Fark görüntüsünde alfa da bir "fark"tır; saydamlık olarak kullanılırsa sonuç görünmez olur.
    if (compareMode == 2)
        alpha = 1.0;

    return float4(rgb, alpha);
}
)";
}

Viewer::~Viewer()
{
    // Program kapanırken hâlâ dosya yükleniyorsa bitmesini (belki bir dakika) beklemek yerine
    // doğrudan çıkıyoruz. Kaydedilmemiş bir şey yok, belleği Windows geri alır.
    if (IsLoading() && !m_loadDone)
        ::ExitProcess(0);
    if (IsLoading())
        m_loadThread.join();

    if (m_panoSampler) m_panoSampler->Release();
    if (m_paramBuffer) m_paramBuffer->Release();
    if (m_shader) m_shader->Release();
}

bool Viewer::EnsureShader()
{
    if (m_shader)
        return true;
    if (m_shaderFailed)
        return false;
    m_shaderFailed = true;   // başarısız olursa her karede yeniden deneme

    ID3DBlob* code = nullptr;
    if (FAILED(D3DCompile(kImageShader, strlen(kImageShader), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, nullptr)))
        return false;
    const HRESULT hr = m_device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &m_shader);
    code->Release();
    if (FAILED(hr))
        return false;

    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(ShaderParams);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;

    if (FAILED(m_device->CreateBuffer(&desc, nullptr, &m_paramBuffer)) ||
        FAILED(m_device->CreateSamplerState(&sampler, &m_panoSampler)))
    {
        if (m_paramBuffer) { m_paramBuffer->Release(); m_paramBuffer = nullptr; }
        m_shader->Release();
        m_shader = nullptr;
        return false;
    }

    m_shaderFailed = false;
    return true;
}

void Viewer::UpdateShaderParams(int compareMode, float compareAmount)
{
    const DirectX::FORMAT_TYPE type = DirectX::FormatDataType(m_doc->Meta().format);

    m_params = {};
    m_params.compareMode = compareMode;
    m_params.compareAmount = compareAmount;
    m_params.channel = m_channel;
    m_params.normalMode = m_normalMode;
    m_params.exposure = std::pow(2.0f, m_exposure);
    m_params.alphaThreshold = m_alphaThreshold;
    if (m_linearData) m_params.flags |= FlagLinearToSrgb;
    if (m_toneMap) m_params.flags |= FlagToneMap;
    if (m_alphaTest) m_params.flags |= FlagAlphaTest;
    if (m_flipGreen) m_params.flags |= FlagFlipGreen;
    // İşaretli formatlarda değerler zaten -1..1 aralığında gelir
    if (type == DirectX::FORMAT_TYPE_SNORM && !m_doc->ViewWasConverted()) m_params.flags |= FlagSigned;

    if (m_pano)
    {
        m_params.flags |= FlagPano;
        m_params.yaw = m_yaw;
        m_params.pitch = m_pitch;
        m_params.tanHalfFov = std::tan(m_fov * 0.5f * kPi / 180.0f);
        m_params.aspect = m_canvasSize.x / m_canvasSize.y;
    }
}

// ImGui çizim sırası bu noktaya gelince çağrılır: kendi shader'ımızı ve ayarlarını devreye sokar.
void Viewer::ApplyShader(const ImDrawList*, const ImDrawCmd* cmd)
{
    Viewer* self = static_cast<Viewer*>(cmd->UserCallbackData);
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    ID3D11DeviceContext* context = state->DeviceContext;
    context->UpdateSubresource(self->m_paramBuffer, 0, nullptr, &self->m_params, 0, 0);
    context->PSSetShader(self->m_shader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &self->m_paramBuffer);
    if (self->m_params.flags & FlagPano)
        context->PSSetSamplers(0, 1, &self->m_panoSampler);
}

// Sıradaki çizim için shader'ın ikinci texture'ını (B) bağlar. Veri olarak texture'ın kendisi gelir;
// nullptr gelirse bağlantıyı kaldırır.
void Viewer::BindSecondTexture(const ImDrawList*, const ImDrawCmd* cmd)
{
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    ID3D11ShaderResourceView* view = static_cast<ID3D11ShaderResourceView*>(cmd->UserCallbackData);
    state->DeviceContext->PSSetShaderResources(1, 1, &view);
}

void Viewer::ResetViewSettings()
{
    const DXGI_FORMAT format = m_doc->Meta().format;
    m_channel = ChannelRGBA;
    m_flipGreen = false;
    m_toneMap = false;
    m_exposure = 0.0f;
    m_alphaTest = false;
    m_alphaThreshold = 0.5f;
    // HDR (float) veriler linear saklanır; BC5 neredeyse her zaman iki kanallı normal map'tir.
    m_linearData = DirectX::FormatDataType(format) == DirectX::FORMAT_TYPE_FLOAT;
    m_normalMode = (format == DXGI_FORMAT_BC5_UNORM || format == DXGI_FORMAT_BC5_SNORM) ? NormalRG : NormalOff;
    m_pixelX = m_pixelY = -1;
    m_exportMessage.clear();
    m_applySectionDefaults = true;

    // 360° görünüm açıkken başka bir HDRI açılırsa açık kalsın; eni boyunun iki katı olmayan
    // (yani equirectangular olmayan) bir görüntü açılırsa kapansın.
    const DirectX::TexMetadata& meta = m_doc->Meta();
    if (!m_doc->IsLoaded() || meta.width != meta.height * 2)
        m_pano = false;
    m_yaw = m_pitch = 0.0f;
    m_fov = 75.0f;
}

void Viewer::Open(const std::wstring& path)
{
    RequestLoad({ path, false });
}

void Viewer::OpenCompare(const std::wstring& path)
{
    RequestLoad({ path, true });
}

void Viewer::RequestLoad(const LoadRequest& request)
{
    if (!IsLoading())
    {
        StartLoad(request);
        return;
    }

    // Yükleme yarıda kesilemiyor, o yüzden yeni istek sıraya girer. Aynı yere (A ya da B) giden
    // daha eski istekler artık gereksiz: sürenin sonucu atılır, sırada bekleyen silinir.
    if (m_loading.compare == request.compare)
        m_discardLoad = true;
    std::erase_if(m_loadQueue, [&](const LoadRequest& queued) { return queued.compare == request.compare; });
    m_loadQueue.push_back(request);
}

void Viewer::StartLoad(const LoadRequest& request)
{
    // Eski görüntüyü hemen bırak: dev dosyalarda eskisiyle yenisi aynı anda belleğe sığmayabilir.
    (request.compare ? m_docB : m_doc) = std::make_unique<TextureDoc>();
    m_exportMessage.clear();

    const std::wstring& path = request.path;
    m_loadingDoc = std::make_unique<TextureDoc>();
    m_loading = request;
    m_discardLoad = false;
    m_loadDone = false;
    m_loadStartTime = ImGui::GetTime();

    WIN32_FILE_ATTRIBUTE_DATA info = {};
    m_loadingBytes = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)
        ? ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow : 0;

    if (!request.compare)
    {
        const std::wstring title = L"TexPeek - " + FileNameOf(path);
        SetWindowTextW(m_hwnd, title.c_str());
    }

    // D3D11 "device" birden fazla iş parçacığından kullanılabilir (çizim yapan "context" kullanılamaz;
    // yükleme ona dokunmuyor), bu yüzden texture'ı GPU'ya göndermek de arka planda yapılabiliyor.
    m_loadThread = std::thread([doc = m_loadingDoc.get(), device = m_device, path, done = &m_loadDone]
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // PNG / JPG okuma (WIC) için
        doc->Load(device, path);
        if (SUCCEEDED(com))
            CoUninitialize();
        *done = true;
    });
}

void Viewer::PollLoad()
{
    if (!IsLoading() || !m_loadDone)
        return;
    m_loadThread.join();

    if (m_discardLoad)
        m_loadingDoc.reset();
    else if (m_loading.compare)
    {
        m_docB = std::move(m_loadingDoc);
        m_openCompareSection = true;
        SyncCompareSelection();
        m_pixelX = m_pixelY = -1;
        m_fitFrames = 1;
    }
    else
    {
        m_doc = std::move(m_loadingDoc);
        ResetViewSettings();
        SyncCompareSelection();
        m_fitFrames = 2;
    }

    if (!m_loadQueue.empty())
    {
        const LoadRequest next = m_loadQueue.front();
        m_loadQueue.erase(m_loadQueue.begin());
        StartLoad(next);
    }
}

// B'de, A'da seçili olan mip / katman / dilimin karşılığını göster (B'de yoksa en yakını).
void Viewer::SyncCompareSelection()
{
    if (!m_docB->IsLoaded() || !m_doc->IsLoaded())
        return;
    const DirectX::TexMetadata& b = m_docB->Meta();
    const int mip = std::clamp(m_doc->Mip(), 0, (int)b.mipLevels - 1);
    const int item = std::clamp(m_doc->Item(), 0, (int)b.arraySize - 1);
    if (mip != m_docB->Mip() || item != m_docB->Item() || m_doc->Slice() != m_docB->Slice())
        m_docB->Select(m_device, mip, item, m_doc->Slice());
}

void Viewer::ShowOpenDialog(bool compare)
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter = Tr(
        L"Desteklenen dosyalar\0*.dds;*.tga;*.hdr;*.exr;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff\0"
        L"DDS\0*.dds\0"
        L"Tüm dosyalar\0*.*\0",
        L"Supported files\0*.dds;*.tga;*.hdr;*.exr;*.png;*.jpg;*.jpeg;*.bmp;*.tif;*.tiff\0"
        L"DDS\0*.dds\0"
        L"All files\0*.*\0");
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn))
        RequestLoad({ file, compare });
}

void Viewer::ShowExportDialog()
{
    // Önerilen ad: asıl dosya adı (+ mip 0 değilse hangi mip olduğu)
    std::wstring name = StemOf(m_doc->Path());
    if (m_doc->Mip() > 0)
        name += L"_mip" + std::to_wstring(m_doc->Mip());

    wchar_t file[MAX_PATH] = {};
    wcsncpy_s(file, name.c_str(), _TRUNCATE);

    const bool isFloat = DirectX::FormatDataType(m_doc->Meta().format) == DirectX::FORMAT_TYPE_FLOAT;

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = m_hwnd;
    ofn.lpstrFilter =
        L"PNG\0*.png\0"
        L"TGA\0*.tga\0"
        L"HDR (Radiance)\0*.hdr\0";
    ofn.nFilterIndex = isFloat ? 3 : 1;   // HDR veriyi varsayılan olarak .hdr kaydet
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = isFloat ? L"hdr" : L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn))
        m_exportOk = m_doc->Export(file, m_exportMessage);
}

void Viewer::FitToCanvas(const ImVec2& canvasSize)
{
    const float w = (float)m_doc->ViewWidth();
    const float h = (float)m_doc->ViewHeight();
    if (w <= 0 || h <= 0)
        return;
    const float margin = 0.95f;
    const float copies = (m_tilePreview && !CompareActive()) ? 3.0f : 1.0f;   // döşeme önizlemesinde 3x3'ün tamamı sığsın
    m_zoom = std::min(canvasSize.x * margin / (w * copies), canvasSize.y * margin / (h * copies));
    m_zoom = std::min(m_zoom, 1.0f);   // küçük resimleri büyütme, 1:1 göster
    m_offset = { (canvasSize.x - w * m_zoom) * 0.5f, (canvasSize.y - h * m_zoom) * 0.5f };
    m_fitted = true;
}

namespace
{
    const ImU32 kAccent = IM_COL32(66, 135, 245, 255);
    const ImU32 kOnAccent = IM_COL32(255, 255, 255, 255);

    // Bitişik düğmelerden oluşan seçici ("segment"): seçili olan vurgu rengiyle boyanır.
    // width 0 ise her düğme metni kadar, değilse toplam genişlik eşit paylaştırılır.
    // Seçim değiştiyse true döner.
    bool SegmentedControl(const char* id, const char* const* labels, int count, int* value, float width = 0.0f)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float height = ImGui::GetFrameHeight();
        const float gap = 1.0f;
        bool changed = false;

        ImGui::PushID(id);
        ImGui::BeginGroup();
        for (int i = 0; i < count; ++i)
        {
            const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
            const float w = width > 0.0f ? std::floor((width - gap * (float)(count - 1)) / (float)count) : ts.x + style.FramePadding.x * 2.0f;
            if (i > 0)
                ImGui::SameLine(0.0f, gap);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##segment", ImVec2(w, height)) && *value != i)
            {
                *value = i;
                changed = true;
            }
            ImGui::PopID();

            const bool selected = *value == i;
            const ImU32 background = selected ? kAccent
                : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
            ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
            if (count == 1) corners = ImDrawFlags_RoundCornersAll;
            else if (i == 0) corners = ImDrawFlags_RoundCornersLeft;
            else if (i == count - 1) corners = ImDrawFlags_RoundCornersRight;
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + height), background, style.FrameRounding, corners);
            dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (height - ts.y) * 0.5f),
                selected ? kOnAccent : ImGui::GetColorU32(ImGuiCol_Text), labels[i]);
        }
        ImGui::EndGroup();
        ImGui::PopID();
        return changed;
    }

    // Açıkken vurgu rengiyle boyanan aç / kapa düğmesi.
    bool ToggleButton(const char* label, bool* value)
    {
        const bool on = *value;
        if (on)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccent);
            ImGui::PushStyleColor(ImGuiCol_Text, kOnAccent);
        }
        const bool clicked = ImGui::Button(label);
        if (on)
            ImGui::PopStyleColor(3);
        if (clicked)
            *value = !*value;
        return clicked;
    }

    void ErrorText(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 110, 255));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }

    // Soluk renkli, satır sonunda kırılan açıklama metni.
    void NoteText(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }
}

void Viewer::SetActualSize()
{
    const ImVec2 center = m_canvasSize * 0.5f;
    const ImVec2 imagePoint = (center - m_offset) * (1.0f / m_zoom);
    m_offset = center - imagePoint;
    m_zoom = 1.0f;
    m_fitted = false;
}

// Klavye kısayolları. Bir metin kutusuna yazı yazılırken devre dışıdır.
void Viewer::HandleShortcuts()
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput)
        return;

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false))
    {
        ShowOpenDialog(false);
        return;
    }
    if (!m_doc->HasView() || IsLoading())
        return;
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false))
    {
        ShowExportDialog();
        return;
    }
    if (io.KeyCtrl || io.KeyAlt)
        return;

    // R / G / B / A: o kanalı tek başına göster; aynı tuşa tekrar basınca RGBA'ya dön
    auto channelKey = [&](ImGuiKey key, int channel)
    {
        if (ImGui::IsKeyPressed(key, false))
            m_channel = m_channel == channel ? ChannelRGBA : channel;
    };
    channelKey(ImGuiKey_R, ChannelR);
    channelKey(ImGuiKey_G, ChannelG);
    channelKey(ImGuiKey_B, ChannelB);
    channelKey(ImGuiKey_A, ChannelA);

    if (ImGui::IsKeyPressed(ImGuiKey_F, false))
    {
        if (m_pano)
        {
            m_yaw = m_pitch = 0.0f;
            m_fov = 75.0f;
        }
        else
            m_fitFrames = 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_1, false) && !m_pano)
        SetActualSize();
}

void Viewer::Draw()
{
    PollLoad();
    HandleShortcuts();

    // Tüm pencereyi kaplayan, kenarlıksız bir ana alan
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(3);

    // Sol: ayar paneli
    const float panelWidth = ImGui::GetFontSize() * 18.5f;
    ImGui::BeginChild("##panel", ImVec2(panelWidth, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    DrawSidePanel();
    ImGui::EndChild();

    // Sağ: üstte araç çubuğu, altında tuval
    ImGui::SameLine(0, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##right", ImVec2(0, 0), ImGuiChildFlags_None,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    DrawToolbar();
    DrawCanvas();
    ImGui::EndChild();

    ImGui::End();
}

// Tuvalin üstündeki ince şerit: en sık kullanılanlar (kanal, yakınlaştırma, 360°, karşılaştırma modu)
void Viewer::DrawToolbar()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float font = ImGui::GetFontSize();
    const float pad = font * 0.4f;
    const float height = ImGui::GetFrameHeight() + pad * 2.0f;
    const float groupGap = font * 1.1f;

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), ImGui::GetColorU32(ImGuiCol_WindowBg));
    dl->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::SetCursorScreenPos(ImVec2(origin.x + pad * 2.0f, origin.y + pad));

    ImGui::BeginDisabled(!m_doc->HasView() || IsLoading());

    static const char* const channels[] = { "RGBA", "RGB", "R", "G", "B", "A" };
    SegmentedControl("channels", channels, 6, &m_channel);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr(
            "Gösterilen kanal.\nRGB: alfayı yok sayar (alfaya başka veri paketlenmiş texture'lar için).\nKısayol: R, G, B, A tuşları; aynı tuşa tekrar basınca RGBA'ya döner.",
            "Displayed channel.\nRGB: ignores alpha (for textures that pack other data into alpha).\nShortcut: R, G, B, A keys; press the same key again to return to RGBA."));

    ImGui::SameLine(0.0f, groupGap);
    if (m_pano)
    {
        if (ImGui::Button(Tr("Bakışı sıfırla", "Reset view")))
        {
            m_yaw = m_pitch = 0.0f;
            m_fov = 75.0f;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr("Kısayol: F ya da çift tık", "Shortcut: F or double-click"));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(Tr("Görüş açısı %.0f°", "Field of view %.0f°"), m_fov);
    }
    else
    {
        if (ImGui::Button(Tr("Sığdır", "Fit")))
            m_fitFrames = 1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr("Görüntüyü pencereye sığdır.\nKısayol: F ya da çift tık",
                "Fit the image to the window.\nShortcut: F or double-click"));
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
        if (ImGui::Button("1:1"))
            SetActualSize();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr("Gerçek boyut: bir texture pikseli = bir ekran pikseli.\nKısayol: 1",
                "Actual size: one texture pixel = one screen pixel.\nShortcut: 1"));
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(Tr("%%%.0f", "%.0f%%"), m_zoom * 100.0f);   // Türkçede yüzde işareti başta
    }

    ImGui::SameLine(0.0f, groupGap);
    ToggleButton("360°", &m_pano);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr(
            "Eni boyunun iki katı olan panoramik (equirectangular) görüntüleri\niçinde duruyormuş gibi gösterir. Skybox olarak nasıl duracağını görmek için.\nSürükle: etrafa bak, tekerlek: görüş açısı.",
            "Shows panoramic (equirectangular) images, twice as wide as they are tall,\nas if you were standing inside them. To see how it will look as a skybox.\nDrag: look around, wheel: field of view."));

    if (m_docB->HasView() && !m_pano)
    {
        ImGui::SameLine(0.0f, groupGap);
        const char* const modes[] = { Tr("Bölme", "Split"), Tr("Yan yana", "Side by side"), Tr("Geçiş", "Blend"), Tr("Fark", "Difference") };
        int mode = m_compareMode;
        if (SegmentedControl("compare", modes, 4, &mode))
        {
            // Yan yana modunda görüntü tuvalin yarısına çizilir; o moda girip çıkarken yeniden sığdır.
            if (mode == CompareSideBySide || m_compareMode == CompareSideBySide)
                m_fitFrames = 1;
            m_compareMode = mode;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "Karşılaştırma modu.\nBölme: çizginin solunda A, sağında B.\nYan yana: iki ayrı bölme, yakınlaştırma ortak.\nGeçiş: üst üste soldurma (Boşluk: A / B arasında atla).\nFark: piksel farkı.",
                "Compare mode.\nSplit: A left of the line, B right of it.\nSide by side: two panes with shared zoom.\nBlend: cross-fade on top of each other (Space: jump between A / B).\nDifference: per-pixel difference."));
    }

    ImGui::EndDisabled();

    // Sağ uçta arayüz dili. Dosya açık olmasa da kullanılabilsin diye EndDisabled'dan sonra.
    static const char* const langs[] = { "TR", "EN" };
    float langWidth = 1.0f;   // iki düğme arasındaki boşluk
    for (const char* name : langs)
        langWidth += ImGui::CalcTextSize(name).x + style.FramePadding.x * 2.0f;
    ImGui::SameLine(0.0f, groupGap);
    // Pencere daralırsa soldaki düğmelerin üstüne binmesin, onların sağında kalsın
    const float langX = std::max(ImGui::GetCursorScreenPos().x, origin.x + width - langWidth - pad * 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(langX, origin.y + pad));
    int lang = CurrentLang() == Lang::Turkish ? 0 : 1;
    if (SegmentedControl("lang", langs, 2, &lang))
    {
        SetLang(lang == 0 ? Lang::Turkish : Lang::English);
        // Eski dilde yazılmış sonuç mesajları ekranda kalmasın
        m_exportMessage.clear();
        m_integrationMessage.clear();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Arayüz dili / Interface language");

    // Tuval şeridin hemen altından başlasın
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height));
}

bool Viewer::Section(const char* label, bool openByDefault, bool forceOpen)
{
    ImGui::Spacing();
    if (forceOpen)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    else if (m_applySectionDefaults)
        ImGui::SetNextItemOpen(openByDefault, ImGuiCond_Always);
    return ImGui::CollapsingHeader(label);
}

void Viewer::DrawSidePanel()
{
    const ImGuiStyle& style = ImGui::GetStyle();

    // ---- Dosya düğmeleri ----
    const float half = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f;
    if (ImGui::Button(Tr("Aç...", "Open..."), ImVec2(half, 0)))
        ShowOpenDialog(false);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr("Dosya aç (Ctrl+O).\nDosyayı pencereye sürükleyip bırakmak da olur.",
            "Open a file (Ctrl+O).\nYou can also drag and drop a file onto the window."));
    ImGui::SameLine();
    ImGui::BeginDisabled(!m_doc->IsLoaded() || IsLoading());
    if (ImGui::Button(Tr("Dışa Aktar...", "Export..."), ImVec2(half, 0)))
        ShowExportDialog();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", Tr(
            "PNG, TGA ya da HDR olarak kaydet (Ctrl+E).\nŞu an gösterilen mip / katmanı dosyadaki ham haliyle kaydeder;\nkanal, pozlama gibi görünüm ayarları kayda yansımaz.",
            "Save as PNG, TGA or HDR (Ctrl+E).\nSaves the mip / layer currently shown, with the raw data from the file;\nview settings such as channel and exposure are not applied."));
    ImGui::EndDisabled();

    if (!m_exportMessage.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, m_exportOk ? IM_COL32(130, 210, 130, 255) : IM_COL32(255, 120, 110, 255));
        ImGui::TextWrapped("%s", m_exportMessage.c_str());
        ImGui::PopStyleColor();
    }
    if (!m_doc->Error().empty())
        ErrorText(m_doc->Error().c_str());

    if (IsLoading())
    {
        ImGui::Spacing();
        ImGui::TextWrapped(Tr("Yükleniyor: %s", "Loading: %s"), ToUtf8(FileNameOf(m_loading.path)).c_str());
        return;
    }
    if (!m_doc->IsLoaded())
    {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", Tr("Bir dosyayı pencereye sürükleyip bırak ya da 'Aç' ile seç.",
            "Drag and drop a file onto the window, or pick one with 'Open'."));
        NoteText("DDS, TGA, HDR, EXR, PNG, JPG, BMP, TIF");
        DrawIntegrationSection();
        // Varsayılanlar bir kez uygulandı; açık bırakılırsa bölüm her karede yeniden kapanır.
        m_applySectionDefaults = false;
        return;
    }

    const DirectX::TexMetadata& m = m_doc->Meta();
    const bool isFloat = DirectX::FormatDataType(m.format) == DirectX::FORMAT_TYPE_FLOAT;
    const bool isTwoChannelNormal = m.format == DXGI_FORMAT_BC5_UNORM || m.format == DXGI_FORMAT_BC5_SNORM;

    // Bölüm başlıklarındaki "###ad": ImGui bölümün açık / kapalı durumunu bu ada göre hatırlar;
    // böylece dil değişince (başlık metni değişince) bölümler kapanmaz.

    // ---- Dosya ----
    if (Section(Tr("Dosya###file", "File###file"), true))
    {
        ImGui::TextWrapped("%s", ToUtf8(FileNameOf(m_doc->Path())).c_str());

        if (ImGui::BeginTable("##info", 2, ImGuiTableFlags_SizingFixedFit))
        {
            auto row = [](const char* label) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", label);
                ImGui::TableNextColumn();
            };

            row(Tr("Boyut", "Size"));
            if (m.IsVolumemap())
                ImGui::Text("%d x %d x %d", (int)m.width, (int)m.height, (int)m.depth);
            else
                ImGui::Text("%d x %d", (int)m.width, (int)m.height);

            row("Format");
            ImGui::TextUnformatted(FormatName(m.format));

            row(Tr("Tür", "Type"));
            if (m.IsCubemap())
                ImGui::Text("Cubemap (%d)", (int)(m.arraySize / 6));
            else if (m.IsVolumemap())
                ImGui::TextUnformatted(Tr("3D (hacim)", "3D (volume)"));
            else if (m.arraySize > 1)
                ImGui::Text(Tr("Dizi (%d katman)", "Array (%d layers)"), (int)m.arraySize);
            else
                ImGui::TextUnformatted("2D");

            row("Mip");
            ImGui::Text(Tr("%d seviye", m.mipLevels == 1 ? "%d level" : "%d levels"), (int)m.mipLevels);

            row(Tr("Bellek", "Memory"));
            ImGui::Text("%.2f MB", (double)m_doc->MemorySize() / (1024.0 * 1024.0));

            ImGui::EndTable();
        }

        if (const char* desc = FormatDescription(m.format); desc[0] != '\0')
            NoteText(desc);
        if (m_doc->ViewWasConverted())
            NoteText(Tr("Not: GPU bu görüntüyü doğrudan gösteremedi, 8-bit renge çevrilerek gösteriliyor.",
                "Note: the GPU could not show this image directly, it is shown converted to 8-bit color."));
    }

    // ---- Mip / katman / dilim seçimi ----
    if ((m.mipLevels > 1 || m.arraySize > 1 || m.IsVolumemap()) && Section(Tr("Gezinme###navigate", "Navigation###navigate"), true))
    {
        int mip = m_doc->Mip(), item = m_doc->Item(), slice = m_doc->Slice();
        bool changed = false;
        ImGui::PushItemWidth(-FLT_MIN);

        if (m.mipLevels > 1)
            changed |= ImGui::SliderInt("##mip", &mip, 0, (int)m.mipLevels - 1, "Mip %d");
        if (m.IsCubemap())
        {
            const int cubeIndex = item / 6;
            int face = item % 6;
            if (ImGui::SliderInt("##face", &face, 0, 5, CubeFaceName(face)))
            {
                item = cubeIndex * 6 + face;
                changed = true;
            }
            if (m.arraySize > 6)
            {
                int cube = cubeIndex;
                if (ImGui::SliderInt("##cube", &cube, 0, (int)m.arraySize / 6 - 1, "Cubemap %d"))
                {
                    item = cube * 6 + face;
                    changed = true;
                }
            }
        }
        else if (m.arraySize > 1)
            changed |= ImGui::SliderInt("##item", &item, 0, (int)m.arraySize - 1, Tr("Katman %d", "Layer %d"));
        if (m.IsVolumemap() && m_doc->SliceCount() > 1)
            changed |= ImGui::SliderInt("##slice", &slice, 0, m_doc->SliceCount() - 1, Tr("Dilim %d", "Slice %d"));
        ImGui::PopItemWidth();

        if (changed)
        {
            m_doc->Select(m_device, mip, item, slice);
            SyncCompareSelection();
            m_pixelX = m_pixelY = -1;   // piksel değeri yeni görüntüden yeniden okunsun
        }

        ImGui::TextDisabled(Tr("Gösterilen: %d x %d", "Showing: %d x %d"), m_doc->ViewWidth(), m_doc->ViewHeight());
    }

    // ---- Karşılaştırma ----
    if (Section(Tr("Karşılaştırma###compare", "Compare###compare"), m_docB->IsLoaded(), m_openCompareSection))
        DrawComparePanel();
    m_openCompareSection = false;

    // ---- Renk ----
    if (Section(Tr("Renk ve pozlama###color", "Color and exposure###color"), isFloat))
    {
        ImGui::Checkbox(Tr("Linear veri (gamma uygula)###linear", "Linear data (apply gamma)###linear"), &m_linearData);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "Kapalı: değerler ekrana olduğu gibi gönderilir (sRGB renk texture'ları için doğru).\nAçık: veri linear kabul edilip ekrana uygun hale getirilir (HDR için doğru).\nGörüntü olması gerekenden koyu ya da soluk duruyorsa bunu değiştir.",
                "Off: values are sent to the screen as they are (right for sRGB color textures).\nOn: the data is treated as linear and converted for the screen (right for HDR).\nChange this if the image looks darker or more washed out than it should."));
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 4.2f);
        ImGui::SliderFloat("##exposure", &m_exposure, -10.0f, 10.0f, Tr("Pozlama %+.1f EV", "Exposure %+.1f EV"));
        ImGui::SameLine();
        if (ImGui::Button(Tr("Sıfırla", "Reset"), ImVec2(-FLT_MIN, 0)))
            m_exposure = 0.0f;
        ImGui::Checkbox("Tone mapping (ACES)", &m_toneMap);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "Çok parlak yerleri düz beyaza kırpmak yerine yumuşakça sıkıştırır.\nHDR görüntünün oyunda yaklaşık nasıl duracağını gösterir.",
                "Compresses very bright areas smoothly instead of clipping them to flat white.\nShows roughly how the HDR image will look in a game."));
    }

    // ---- Alfa ----
    if (Section(Tr("Alfa###alpha", "Alpha###alpha"), DirectX::HasAlpha(m.format) && !isFloat))
    {
        ImGui::Checkbox(Tr("Dama tahtası arka plan###checker", "Checkerboard background###checker"), &m_checkerboard);
        ImGui::Checkbox(Tr("Alfa eşiği önizlemesi###alphaTest", "Alpha threshold preview###alphaTest"), &m_alphaTest);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "Oyundaki 'alpha test / cutout' gibi: alfası eşiğin altındaki pikseller hiç çizilmez.\nMip seviyelerinde gezip yaprak, çit gibi texture'ların ne kadarının kaybolduğuna bak.",
                "Like 'alpha test / cutout' in a game: pixels with alpha below the threshold are not drawn at all.\nStep through the mip levels to see how much of textures like leaves and fences disappears."));
        if (m_alphaTest)
        {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::SliderFloat("##alphaThreshold", &m_alphaThreshold, 0.0f, 1.0f, Tr("Eşik %.2f", "Threshold %.2f"));
            const float coverage = m_doc->AlphaCoverage(m_alphaThreshold);
            if (coverage >= 0.0f)
                ImGui::Text(Tr("Görünür kalan: %%%.1f", "Still visible: %.1f%%"), coverage * 100.0f);
            else
                ImGui::TextDisabled("%s", Tr("Görünür oran hesaplanamadı.", "Could not compute the visible ratio."));
        }
    }

    // ---- Normal map ----
    if (Section("Normal map###normal", isTwoChannelNormal))
    {
        ImGui::TextDisabled("%s", Tr("Eksik Z (mavi) kanalını hesapla", "Reconstruct the missing Z (blue) channel"));
        const char* const normalModes[] = { Tr("Kapalı", "Off"), "RG", "AG" };
        SegmentedControl("normalMode", normalModes, 3, &m_normalMode, ImGui::GetContentRegionAvail().x);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "RG: BC5 gibi iki kanallı normal map'ler (X kırmızıda, Y yeşilde).\nAG: DXT5nm, Unity'nin eski normal map düzeni (X alfada, Y yeşilde).",
                "RG: two-channel normal maps such as BC5 (X in red, Y in green).\nAG: DXT5nm, Unity's older normal map layout (X in alpha, Y in green)."));
        ImGui::Checkbox(Tr("Yeşil kanalı çevir###flipGreen", "Flip green channel###flipGreen"), &m_flipGreen);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "Normal map'in Y yönünü ters çevirir (DirectX <-> OpenGL düzeni).\nUnity OpenGL (Y+), Unreal DirectX (Y-) düzeni bekler.",
                "Inverts the Y direction of the normal map (DirectX <-> OpenGL convention).\nUnity expects OpenGL (Y+), Unreal expects DirectX (Y-)."));
    }

    // ---- Döşeme ----
    if (Section(Tr("Döşeme###tiling", "Tiling###tiling"), false))
    {
        if (ImGui::Checkbox(Tr("3x3 döşeme önizlemesi###tilePreview", "3x3 tiling preview###tilePreview"), &m_tilePreview))
            m_fitFrames = 1;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "Görüntüyü yan yana 9 kez dizer. Tekrarlayan (tileable) texture'larda\nek yerlerinin belli olup olmadığını görmek için.",
                "Lays the image out 9 times side by side. To check whether the seams\nshow in repeating (tileable) textures."));
    }

    // ---- Kısayollar ----
    if (Section(Tr("Kısayollar###shortcuts", "Shortcuts###shortcuts"), false))
    {
        if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_SizingFixedFit))
        {
            auto key = [](const char* keys, const char* what) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(keys);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", what);
            };
            key(Tr("Tekerlek", "Wheel"), Tr("yakınlaştır", "zoom"));
            key(Tr("Sürükle", "Drag"), Tr("kaydır", "pan"));
            key(Tr("Çift tık, F", "Double-click, F"), Tr("sığdır", "fit"));
            key("1", Tr("gerçek boyut (1:1)", "actual size (1:1)"));
            key("R G B A", Tr("tek kanal", "single channel"));
            key(Tr("Boşluk", "Space"), Tr("A / B arasında atla", "jump between A / B"));
            key("Ctrl+O", Tr("dosya aç", "open file"));
            key("Ctrl+E", Tr("dışa aktar", "export"));
            ImGui::EndTable();
        }
    }

    DrawIntegrationSection();

    m_applySectionDefaults = false;
}

void Viewer::DrawIntegrationSection()
{
    if (!Section("Windows###windows", false))
        return;

    const bool installed = ShellIntegration::IsInstalled();
    NoteText(installed
        ? Tr("Kurulu: .dds dosyaları çift tıklayınca TexPeek ile açılıyor ve Gezgin'de küçük resimleri görünüyor.",
            "Installed: .dds files open with TexPeek on double-click and show thumbnails in Explorer.")
        : Tr("DDS dosyalarını çift tıklayınca TexPeek ile aç ve Gezgin'de küçük resimlerini göster.",
            "Open DDS files with TexPeek on double-click and show their thumbnails in Explorer."));

    if (ImGui::Button(installed ? Tr("Yeniden kur", "Reinstall") : Tr("Kur", "Install")))
        m_integrationOk = ShellIntegration::Install(m_integrationMessage);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr(
            "Yalnızca senin Windows kullanıcına kaydedilir, yönetici izni gerekmez.\nTexPeek'in klasörünü taşırsan yeniden kurman gerekir.",
            "Registered only for your Windows user, no administrator rights needed.\nIf you move the TexPeek folder you need to install again."));
    if (installed)
    {
        ImGui::SameLine();
        if (ImGui::Button(Tr("Kaldır", "Remove")))
            m_integrationOk = ShellIntegration::Uninstall(m_integrationMessage);
    }

    if (!m_integrationMessage.empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, m_integrationOk ? IM_COL32(130, 210, 130, 255) : IM_COL32(255, 120, 110, 255));
        ImGui::TextWrapped("%s", m_integrationMessage.c_str());
        ImGui::PopStyleColor();
    }
}

void Viewer::DrawComparePanel()
{
    if (!m_docB->IsLoaded())
    {
        if (ImGui::Button(Tr("İkinci görüntüyü aç (B)...", "Open second image (B)..."), ImVec2(-FLT_MIN, 0)))
            ShowOpenDialog(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "İkinci bir görüntü açıp bununla karşılaştırır.\nKısa yol: dosyayı Shift tuşuna basılı tutarak pencereye bırak,\nya da iki dosyayı birlikte bırak.",
                "Opens a second image and compares it with this one.\nShortcut: drop the file onto the window while holding Shift,\nor drop two files together."));
        if (!m_docB->Error().empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 100, 255));
            ImGui::TextWrapped("B: %s", m_docB->Error().c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    const DirectX::TexMetadata& a = m_doc->Meta();
    const DirectX::TexMetadata& b = m_docB->Meta();
    ImGui::TextWrapped("B: %s", ToUtf8(FileNameOf(m_docB->Path())).c_str());
    ImGui::TextDisabled("%d x %d, %s", (int)b.width, (int)b.height, FormatName(b.format));
    if (!m_docB->Error().empty())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 100, 255));
        ImGui::TextWrapped("%s", m_docB->Error().c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (a.width != b.width || a.height != b.height)
        ImGui::TextWrapped("%s", Tr("Boyutlar farklı: B, A'nın boyutuna gerilerek gösteriliyor.",
            "Sizes differ: B is shown stretched to the size of A."));
    if (m_pano)
        ImGui::TextWrapped("%s", Tr("360° görünümde karşılaştırma kapalı.", "Compare is off in the 360° view."));
    ImGui::PopStyleColor();

    // Mod seçimi üstteki araç çubuğunda; burada seçili modun ayarları var.
    if (m_compareMode == CompareSplit)
        NoteText(Tr("Bölme: çizgiyi sürükleyerek A | B sınırını kaydır.", "Split: drag the line to move the A | B boundary."));
    else if (m_compareMode == CompareSideBySide)
        NoteText(Tr("Yan yana: yakınlaştırma ve kaydırma iki görüntüde birlikte hareket eder.",
            "Side by side: zoom and pan move both images together."));

    if (m_compareMode == CompareBlend)
    {
        ImGui::TextDisabled("A");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 1.2f);
        ImGui::SliderFloat("##blend", &m_blend, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::TextDisabled("B");
        ImGui::TextDisabled("%s", Tr("Boşluk tuşu: A / B arasında atla", "Space key: jump between A / B"));
    }
    else if (m_compareMode == CompareDiff)
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderFloat("##diffGain", &m_diffGain, 1.0f, 64.0f, Tr("Farkı %.0f kat güçlendir", "Amplify difference %.0fx"), ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", Tr(
                "Sıkıştırma farkları genelde çok küçüktür; görünmeleri için çarpılarak gösterilir.\nGerçek değerler için alttaki durum çubuğundaki A ve B satırlarına bak.",
                "Compression differences are usually tiny; they are multiplied so they become visible.\nFor the real values, see the A and B lines in the status bar at the bottom."));
    }

    if (ImGui::Button(Tr("Başka görüntü aç (B)...", "Open another image (B)..."), ImVec2(-FLT_MIN, 0)))
        ShowOpenDialog(true);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", Tr(
            "B'nin yerine başka bir görüntü açar; A yerinde kalır.\nKısa yol: dosyayı Shift tuşuna basılı tutarak pencereye bırak.",
            "Opens another image in place of B; A stays as it is.\nShortcut: drop the file onto the window while holding Shift."));

    if (ImGui::Button(Tr("A / B yer değiştir", "Swap A / B")))
    {
        std::swap(m_doc, m_docB);
        m_pixelX = m_pixelY = -1;
        const std::wstring title = L"TexPeek - " + FileNameOf(m_doc->Path());
        SetWindowTextW(m_hwnd, title.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("B'yi kapat", "Close B")))
    {
        m_docB = std::make_unique<TextureDoc>();
        if (m_compareMode == CompareSideBySide)
            m_fitFrames = 1;
    }
}

void Viewer::DrawCanvas()
{
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    size.x = std::max(size.x, 1.0f);
    size.y = std::max(size.y, 1.0f);
    const ImVec2 p1 = p0 + size;

    // Yan yana karşılaştırmada her görüntü tuvalin yarısına çizilir
    const bool comparing = !IsLoading() && CompareActive();
    m_canvasSize = size;
    if (comparing && m_compareMode == CompareSideBySide)
        m_canvasSize.x = std::max(1.0f, std::floor(size.x * 0.5f));

    // Fare olaylarını yakalamak için görünmez bir buton tuvali kaplıyor
    ImGui::InvisibleButton("##canvas_input", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(17, 18, 21, 255));

    m_hasHoverPixel = false;
    if (IsLoading())
    {
        DrawLoadingCard(p0, p1);
        return;
    }
    if (!m_doc->HasView())
    {
        const char* hint = Tr("Bir texture dosyasını buraya sürükle", "Drag a texture file here");
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        dl->AddText(p0 + (size - ts) * 0.5f, IM_COL32(140, 140, 150, 255), hint);
        return;
    }

    // Pencere (dolayısıyla tuval) boyutu değişti: sığdırılmış görüntü yeniden sığdırılıp ortalanır;
    // kullanıcı kendi yakınlaştırıp kaydırdıysa tuvalin ortasındaki nokta ortada kalır.
    if (m_canvasSize.x != m_lastCanvasSize.x || m_canvasSize.y != m_lastCanvasSize.y)
    {
        if (m_fitted)
            FitToCanvas(m_canvasSize);
        else
            m_offset = m_offset + (m_canvasSize - m_lastCanvasSize) * 0.5f;
        m_lastCanvasSize = m_canvasSize;
    }

    if (m_fitFrames > 0)
    {
        FitToCanvas(m_canvasSize);
        --m_fitFrames;
    }

    if (m_pano)
    {
        DrawPano(p0, p1, hovered, active);
        DrawPixelStatus(p0, p1);
        return;
    }
    if (comparing)
    {
        DrawCompare(p0, p1, hovered, active);
        DrawPixelStatus(p0, p1);
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Tekerlek: fare imlecinin altındaki nokta sabit kalacak şekilde yakınlaştır
    if (hovered && io.MouseWheel != 0.0f)
    {
        const float newZoom = std::clamp(m_zoom * std::pow(1.25f, io.MouseWheel), 0.01f, 64.0f);
        const ImVec2 mouse = io.MousePos - p0;
        const ImVec2 imagePoint = (mouse - m_offset) * (1.0f / m_zoom);
        m_offset = mouse - imagePoint * newZoom;
        m_zoom = newZoom;
        m_fitted = false;
    }

    // Sürükle: kaydır
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        m_offset = m_offset + io.MouseDelta;
        m_fitted = false;
    }

    // Çift tık: sığdır
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        FitToCanvas(size);

    const ImVec2 imgMin = p0 + m_offset;
    const ImVec2 imgMax = imgMin + ImVec2((float)m_doc->ViewWidth() * m_zoom, (float)m_doc->ViewHeight() * m_zoom);

    dl->PushClipRect(p0, p1, true);

    // Çizilen alan: tek görüntü, ya da döşeme önizlemesinde her yöne birer kopya daha
    const ImVec2 imgSize = imgMax - imgMin;
    const int repeat = m_tilePreview ? 1 : 0;
    const ImVec2 areaMin = imgMin - imgSize * (float)repeat;
    const ImVec2 areaMax = imgMax + imgSize * (float)repeat;

    // Çizilen alanın ekranda görünen kısmı
    const ImVec2 visMin(std::max(areaMin.x, p0.x), std::max(areaMin.y, p0.y));
    const ImVec2 visMax(std::min(areaMax.x, p1.x), std::min(areaMax.y, p1.y));

    if (visMin.x < visMax.x && visMin.y < visMax.y)
    {
        if (m_checkerboard)
            DrawCheckerboard(dl, imgMin, visMin, visMax);

        // Görüntüyü kendi shader'ımızla çiz (kanal, pozlama, alfa eşiği, normal map ayarları)
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        if (EnsureShader())
        {
            UpdateShaderParams();
            dl->AddCallback(ApplyShader, this);
        }
        // Büyütünce pikseller bulanık değil keskin görünsün
        if (m_zoom > 1.0f)
            dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);

        for (int ry = -repeat; ry <= repeat; ++ry)
            for (int rx = -repeat; rx <= repeat; ++rx)
            {
                const ImVec2 origin = imgMin + ImVec2(imgSize.x * (float)rx, imgSize.y * (float)ry);
                DrawTiles(dl, *m_doc, origin, ImVec2(m_zoom, m_zoom), visMin, visMax);
            }

        // ImGui'nin kendi shader ve ayarlarına geri dön
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    }

    dl->AddRect(areaMin - ImVec2(1, 1), areaMax + ImVec2(1, 1), IM_COL32(70, 70, 80, 255));
    dl->PopClipRect();

    // Fare altındaki piksel
    if (hovered)
    {
        const ImVec2 local = (io.MousePos - imgMin) * (1.0f / m_zoom);
        int px = (int)std::floor(local.x);
        int py = (int)std::floor(local.y);
        const int w = m_doc->ViewWidth(), h = m_doc->ViewHeight();
        if (m_tilePreview && px >= -w && py >= -h && px < 2 * w && py < 2 * h)
        {
            // Kopyaların üzerindeyken asıl görüntüdeki karşılık gelen pikseli göster
            px = (px + w) % w;
            py = (py + h) % h;
        }
        if (px >= 0 && py >= 0 && px < w && py < h)
        {
            m_hasHoverPixel = true;
            m_hoverX = px;
            m_hoverY = py;
        }
    }

    DrawPixelStatus(p0, p1);
}

// Karşılaştırma: A ve B aynı yakınlaştırma / kaydırma ile, seçili moda göre çizilir.
// B, A'nın ekrandaki boyutuna gerilir; böylece farklı çözünürlükteki sürümler de üst üste oturur.
void Viewer::DrawCompare(const ImVec2& p0, const ImVec2& p1, bool hovered, bool active)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = p1 - p0;
    const bool split = m_compareMode == CompareSplit;
    const bool sideBySide = m_compareMode == CompareSideBySide;
    const float midX = p0.x + m_canvasSize.x;   // yan yana modunda iki bölmenin sınırı

    // Bölme çizgisi: yakınından tutulup sürüklenir
    const float grab = ImGui::GetFontSize() * 0.5f;
    const bool nearSplit = split && hovered && std::fabs(io.MousePos.x - (p0.x + size.x * m_split)) <= grab;
    if (!split || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
        m_dragSplit = false;
    if (nearSplit && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        m_dragSplit = true;
    if (m_dragSplit)
        m_split = std::clamp((io.MousePos.x - p0.x) / size.x, 0.0f, 1.0f);
    if (nearSplit || m_dragSplit)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    const float splitX = p0.x + size.x * m_split;

    // Farenin üzerinde olduğu bölmenin sol üst köşesi (yan yana modunda sağ bölme B'dir)
    const bool overB = sideBySide && io.MousePos.x >= midX;
    const ImVec2 paneA = p0;
    const ImVec2 paneB(sideBySide ? midX : p0.x, p0.y);
    const ImVec2 paneOrigin = overB ? paneB : paneA;

    // Tekerlek: fare imlecinin altındaki nokta sabit kalacak şekilde yakınlaştır
    if (hovered && io.MouseWheel != 0.0f)
    {
        const float newZoom = std::clamp(m_zoom * std::pow(1.25f, io.MouseWheel), 0.01f, 64.0f);
        const ImVec2 mouse = io.MousePos - paneOrigin;
        const ImVec2 imagePoint = (mouse - m_offset) * (1.0f / m_zoom);
        m_offset = mouse - imagePoint * newZoom;
        m_zoom = newZoom;
        m_fitted = false;
    }

    // Sürükle: kaydır (bölme çizgisi sürüklenmiyorsa)
    if (active && !m_dragSplit && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        m_offset = m_offset + io.MouseDelta;
        m_fitted = false;
    }

    // Çift tık: sığdır
    if (hovered && !nearSplit && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        FitToCanvas(m_canvasSize);

    // Boşluk: geçiş modunda A ile B arasında atla
    if (m_compareMode == CompareBlend && ImGui::IsKeyPressed(ImGuiKey_Space, false))
        m_blend = m_blend < 0.5f ? 1.0f : 0.0f;

    const int aw = m_doc->ViewWidth(), ah = m_doc->ViewHeight();
    const int bw = m_docB->ViewWidth(), bh = m_docB->ViewHeight();
    const ImVec2 dispSize((float)aw * m_zoom, (float)ah * m_zoom);
    const ImVec2 scaleA(m_zoom, m_zoom);
    const ImVec2 scaleB(dispSize.x / (float)bw, dispSize.y / (float)bh);
    const bool shader = EnsureShader();
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    const ImU32 borderColor = IM_COL32(70, 70, 80, 255);

    // Bir görüntüyü, verilen bölmeye kırpılmış olarak çizer.
    auto drawPane = [&](const TextureDoc& doc, const ImVec2& scale, const ImVec2& paneMin, const ImVec2& paneMax, const ImVec2& origin)
    {
        const ImVec2 imgMin = origin + m_offset;
        const ImVec2 imgMax = imgMin + dispSize;
        const ImVec2 visMin(std::max(imgMin.x, paneMin.x), std::max(imgMin.y, paneMin.y));
        const ImVec2 visMax(std::min(imgMax.x, paneMax.x), std::min(imgMax.y, paneMax.y));
        if (visMin.x >= visMax.x || visMin.y >= visMax.y)
            return;

        dl->PushClipRect(paneMin, paneMax, true);
        if (m_checkerboard)
            DrawCheckerboard(dl, imgMin, visMin, visMax);
        if (shader)
        {
            UpdateShaderParams();
            dl->AddCallback(ApplyShader, this);
        }
        if (m_zoom > 1.0f)
            dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
        DrawTiles(dl, doc, imgMin, scale, visMin, visMax);
        dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        dl->AddRect(imgMin - ImVec2(1, 1), imgMax + ImVec2(1, 1), borderColor);
        dl->PopClipRect();
    };

    const float labelGap = ImGui::GetFontSize() * 0.5f;
    dl->PushClipRect(p0, p1, true);

    if (split)
    {
        drawPane(*m_doc, scaleA, p0, ImVec2(splitX, p1.y), p0);
        drawPane(*m_docB, scaleB, ImVec2(splitX, p0.y), p1, p0);

        // Çizgi ve ortasındaki tutamak
        dl->AddLine(ImVec2(splitX, p0.y), ImVec2(splitX, p1.y), IM_COL32(0, 0, 0, 160), 4.0f);
        dl->AddLine(ImVec2(splitX, p0.y), ImVec2(splitX, p1.y), IM_COL32(240, 240, 245, 255), 2.0f);
        const ImVec2 handle(splitX, (p0.y + p1.y) * 0.5f);
        dl->AddCircleFilled(handle, grab * 1.2f, IM_COL32(240, 240, 245, 255));
        dl->AddCircle(handle, grab * 1.2f, IM_COL32(0, 0, 0, 160), 0, 2.0f);

        const float labelWidth = ImGui::CalcTextSize("A").x + ImGui::GetFontSize() * 0.9f;
        DrawLabel(dl, ImVec2(splitX - labelGap - labelWidth, p0.y + labelGap), "A");
        DrawLabel(dl, ImVec2(splitX + labelGap, p0.y + labelGap), "B");
    }
    else if (sideBySide)
    {
        drawPane(*m_doc, scaleA, p0, ImVec2(midX, p1.y), paneA);
        drawPane(*m_docB, scaleB, ImVec2(midX, p0.y), p1, paneB);
        dl->AddLine(ImVec2(midX, p0.y), ImVec2(midX, p1.y), IM_COL32(10, 10, 12, 255), 3.0f);
        DrawLabel(dl, ImVec2(p0.x + labelGap, p0.y + labelGap), "A");
        DrawLabel(dl, ImVec2(midX + labelGap, p0.y + labelGap), "B");
    }
    else
    {
        // Geçiş ve fark: iki görüntü aynı çizimde, shader içinde birleştirilir. Bunun için karoları
        // birebir eşleşmeli: ya ikisi de tek parça, ya da boyutları aynı.
        const std::vector<ViewTile>& ta = m_doc->Tiles();
        const std::vector<ViewTile>& tb = m_docB->Tiles();
        const bool diff = m_compareMode == CompareDiff;
        const bool compatible = shader && ta.size() == tb.size() && (ta.size() == 1 || (aw == bw && ah == bh));
        if (!compatible)
        {
            drawPane(*m_doc, scaleA, p0, p1, p0);
            DrawLabel(dl, ImVec2(p0.x + labelGap, p0.y + labelGap),
                Tr("Bu mod, 16384 pikselden büyük görüntülerde yalnızca boyutlar aynıysa çalışır. Şu an yalnızca A gösteriliyor.",
                    "For images larger than 16384 pixels this mode works only when the sizes match. Only A is shown for now."));
        }
        else
        {
            const ImVec2 imgMin = p0 + m_offset;
            const ImVec2 imgMax = imgMin + dispSize;
            const ImVec2 visMin(std::max(imgMin.x, p0.x), std::max(imgMin.y, p0.y));
            const ImVec2 visMax(std::min(imgMax.x, p1.x), std::min(imgMax.y, p1.y));
            if (visMin.x < visMax.x && visMin.y < visMax.y)
            {
                if (diff)
                    dl->AddRectFilled(visMin, visMax, IM_COL32(0, 0, 0, 255));
                else if (m_checkerboard)
                    DrawCheckerboard(dl, imgMin, visMin, visMax);

                UpdateShaderParams(diff ? 2 : 1, diff ? m_diffGain : m_blend);
                dl->AddCallback(ApplyShader, this);
                if (m_zoom > 1.0f)
                    dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
                for (size_t i = 0; i < ta.size(); ++i)
                {
                    const ImVec2 a = imgMin + ImVec2((float)ta[i].x, (float)ta[i].y) * m_zoom;
                    const ImVec2 b = imgMin + ImVec2((float)(ta[i].x + ta[i].width), (float)(ta[i].y + ta[i].height)) * m_zoom;
                    if (b.x < visMin.x || b.y < visMin.y || a.x > visMax.x || a.y > visMax.y)
                        continue;
                    dl->AddCallback(BindSecondTexture, tb[i].view);
                    dl->AddImage((ImTextureID)(intptr_t)ta[i].view, a, b);
                }
                dl->AddCallback(BindSecondTexture, nullptr);
                dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
            dl->AddRect(imgMin - ImVec2(1, 1), imgMax + ImVec2(1, 1), borderColor);

            char label[64];
            if (diff)
                std::snprintf(label, sizeof(label), Tr("Fark  |A - B| x %.0f", "Difference  |A - B| x %.0f"), m_diffGain);
            else
                std::snprintf(label, sizeof(label), Tr("A %%%.0f   B %%%.0f", "A %.0f%%   B %.0f%%"), (1.0f - m_blend) * 100.0f, m_blend * 100.0f);
            DrawLabel(dl, ImVec2(p0.x + labelGap, p0.y + labelGap), label);
        }
    }

    // Fare altındaki piksel (A'nın piksel koordinatlarında)
    if (hovered && !m_dragSplit)
    {
        const ImVec2 local = (io.MousePos - paneOrigin - m_offset) * (1.0f / m_zoom);
        const int px = (int)std::floor(local.x);
        const int py = (int)std::floor(local.y);
        if (px >= 0 && py >= 0 && px < aw && py < ah)
        {
            m_hasHoverPixel = true;
            m_hoverX = px;
            m_hoverY = py;

            // Yan yana modunda aynı noktayı diğer bölmede de işaretle
            if (sideBySide)
            {
                const ImVec2 otherOrigin = overB ? paneA : paneB;
                const ImVec2 c = otherOrigin + m_offset + ImVec2((float)px + 0.5f, (float)py + 0.5f) * m_zoom;
                const float arm = ImGui::GetFontSize() * 0.6f;
                dl->PushClipRect(ImVec2(overB ? p0.x : midX, p0.y), ImVec2(overB ? midX : p1.x, p1.y), true);
                for (int pass = 0; pass < 2; ++pass)
                {
                    const ImU32 color = pass == 0 ? IM_COL32(0, 0, 0, 200) : IM_COL32(255, 255, 255, 255);
                    const float thickness = pass == 0 ? 3.0f : 1.0f;
                    dl->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), color, thickness);
                    dl->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), color, thickness);
                }
                dl->PopClipRect();
            }
        }
    }

    dl->PopClipRect();
}

// 360° görünüm: tuvalin tamamını kaplayan tek bir dörtgen çizilir, hangi pikselin
// görüntünün neresine baktığını shader hesaplar.
void Viewer::DrawPano(const ImVec2& p0, const ImVec2& p1, bool hovered, bool active)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size = p1 - p0;

    ID3D11ShaderResourceView* view = m_doc->PanoView(m_device);
    if (!view || !EnsureShader())
    {
        const char* text = Tr("360° görünüm hazırlanamadı.", "Could not prepare the 360° view.");
        const ImVec2 ts = ImGui::CalcTextSize(text);
        dl->AddText(p0 + (size - ts) * 0.5f, IM_COL32(255, 110, 100, 255), text);
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    // Tekerlek: görüş açısını daralt / genişlet
    if (hovered && io.MouseWheel != 0.0f)
        m_fov = std::clamp(m_fov * std::pow(0.9f, io.MouseWheel), 10.0f, 140.0f);

    // Sürükle: görüntüyü tutup çekiyormuş gibi etrafa bak
    if (active && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))
    {
        const float radiansPerPixel = (m_fov * kPi / 180.0f) / size.y;
        m_yaw -= io.MouseDelta.x * radiansPerPixel;
        m_pitch += io.MouseDelta.y * radiansPerPixel;
        m_pitch = std::clamp(m_pitch, -kPi * 0.499f, kPi * 0.499f);   // tam tepe / tam dip noktasında takla atmasın
        m_yaw = std::remainder(m_yaw, 2.0f * kPi);
    }

    // Çift tık: bakışı sıfırla
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        m_yaw = m_pitch = 0.0f;
        m_fov = 75.0f;
    }

    UpdateShaderParams();
    dl->AddCallback(ApplyShader, this);
    dl->AddImage((ImTextureID)(intptr_t)view, p0, p1);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    // Fare altındaki piksel: shader'daki hesabın aynısı
    if (hovered)
    {
        const ImVec2 screenUv((io.MousePos.x - p0.x) / size.x, (io.MousePos.y - p0.y) / size.y);
        const ImVec2 uv = PanoUv(screenUv, m_params.yaw, m_params.pitch, m_params.tanHalfFov, m_params.aspect);
        const int w = m_doc->ViewWidth(), h = m_doc->ViewHeight();
        m_hasHoverPixel = true;
        m_hoverX = std::clamp((int)(uv.x * (float)w), 0, w - 1);
        m_hoverY = std::clamp((int)(uv.y * (float)h), 0, h - 1);
    }
}

// Yükleme kartı: tuvalin ortasında dönen gösterge, dosya adı, boyutu ve geçen süre
void Viewer::DrawLoadingCard(const ImVec2& p0, const ImVec2& p1)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float font = ImGui::GetFontSize();
    const double elapsed = ImGui::GetTime() - m_loadStartTime;

    const std::string name = ToUtf8(FileNameOf(m_loading.path));
    char info[96];
    const double mb = (double)m_loadingBytes / (1024.0 * 1024.0);
    if (mb >= 1024.0)
        std::snprintf(info, sizeof(info), Tr("%.2f GB   ·   %d sn", "%.2f GB   ·   %d s"), mb / 1024.0, (int)elapsed);
    else
        std::snprintf(info, sizeof(info), Tr("%.1f MB   ·   %d sn", "%.1f MB   ·   %d s"), mb, (int)elapsed);
    const char* title = m_loading.compare
        ? Tr("Karşılaştırma görüntüsü (B) yükleniyor...", "Loading the compare image (B)...")
        : Tr("Yükleniyor...", "Loading...");
    const char* note = m_loadQueue.empty()
        ? Tr("Büyük dosyalar bir dakikayı bulabilir.", "Large files can take up to a minute.")
        : Tr("Sıradaki dosya bunun ardından açılacak.", "The next file will open after this one.");

    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
    const ImVec2 infoSize = ImGui::CalcTextSize(info);
    const ImVec2 noteSize = ImGui::CalcTextSize(note);

    const float radius = font * 1.1f;
    const float gap = font * 0.45f;
    const float pad = font * 1.4f;
    const float width = std::max({ font * 16.0f, titleSize.x, nameSize.x, noteSize.x }) + pad * 2.0f;
    const float height = pad * 2.0f + radius * 2.0f + gap * 2.0f + titleSize.y + nameSize.y + infoSize.y + noteSize.y + gap * 3.0f;

    const ImVec2 center = (p0 + p1) * 0.5f;
    const ImVec2 cardMin(center.x - width * 0.5f, center.y - height * 0.5f);
    const ImVec2 cardMax(center.x + width * 0.5f, center.y + height * 0.5f);
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(cardMin, cardMax, IM_COL32(38, 38, 44, 255), font * 0.5f);
    dl->AddRect(cardMin, cardMax, IM_COL32(70, 70, 80, 255), font * 0.5f);

    // Dönen gösterge: soluk tam halka + üzerinde dönen parlak yay
    float y = cardMin.y + pad + radius;
    const ImVec2 spinner(center.x, y);
    const float angle = (float)elapsed * 5.0f;
    dl->PathArcTo(spinner, radius, 0.0f, 2.0f * kPi, 48);
    dl->PathStroke(IM_COL32(70, 70, 80, 255), ImDrawFlags_None, font * 0.22f);
    dl->PathArcTo(spinner, radius, angle, angle + kPi * 0.6f, 24);
    dl->PathStroke(IM_COL32(90, 160, 255, 255), ImDrawFlags_None, font * 0.22f);
    y += radius + gap * 2.0f;

    auto line = [&](const char* text, const ImVec2& size, ImU32 color)
    {
        dl->AddText(ImVec2(center.x - size.x * 0.5f, y), color, text);
        y += size.y + gap;
    };
    line(title, titleSize, IM_COL32(235, 235, 240, 255));
    line(name.c_str(), nameSize, IM_COL32(190, 190, 200, 255));
    line(info, infoSize, IM_COL32(235, 235, 240, 255));
    line(note, noteSize, IM_COL32(130, 130, 140, 255));
    dl->PopClipRect();
}

// Durum çubuğu: fare altındaki pikselin konumu ve dosyadaki ham değeri
void Viewer::DrawPixelStatus(const ImVec2& p0, const ImVec2& p1)
{
    if (!m_hasHoverPixel)
        return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool compare = CompareActive();

    // Değerleri yalnızca fare başka bir piksele geçtiğinde yeniden oku
    if (m_hoverX != m_pixelX || m_hoverY != m_pixelY)
    {
        m_pixelX = m_hoverX;
        m_pixelY = m_hoverY;
        m_pixelValid = m_doc->ReadPixel(m_hoverX, m_hoverY, m_pixel);
        m_pixelBValid = false;
        if (compare)
        {
            // B farklı boyuttaysa aynı noktaya denk gelen pikseli bul
            m_pixelBX = (int)((long long)m_hoverX * m_docB->ViewWidth() / m_doc->ViewWidth());
            m_pixelBY = (int)((long long)m_hoverY * m_docB->ViewHeight() / m_doc->ViewHeight());
            m_pixelBValid = m_docB->ReadPixel(m_pixelBX, m_pixelBY, m_pixelB);
        }
    }

    // Tek görüntüde bir satır; karşılaştırmada A ve B alt alta
    char values[160];
    char lines[2][220];
    int lineCount = 1;
    FormatPixelValues(values, sizeof(values), m_doc->Meta().format, m_pixelValid, m_pixel);
    std::snprintf(lines[0], sizeof(lines[0]), "%sX %d  Y %d     %s", compare ? "A     " : "", m_hoverX, m_hoverY, values);
    if (compare)
    {
        FormatPixelValues(values, sizeof(values), m_docB->Meta().format, m_pixelBValid, m_pixelB);
        std::snprintf(lines[1], sizeof(lines[1]), "B     X %d  Y %d     %s", m_pixelBX, m_pixelBY, values);
        lineCount = 2;
    }

    const ImVec2 pad(ImGui::GetFontSize() * 0.5f, ImGui::GetFontSize() * 0.25f);
    const float lineHeight = ImGui::GetTextLineHeight();
    float width = 0.0f;
    for (int i = 0; i < lineCount; ++i)
        width = std::max(width, ImGui::CalcTextSize(lines[i]).x);
    const ImVec2 barMin(p0.x, p1.y - lineHeight * (float)lineCount - pad.y * 2.0f);
    dl->AddRectFilled(barMin, ImVec2(p0.x + width + pad.x * 2.0f, p1.y), IM_COL32(15, 15, 18, 230));
    for (int i = 0; i < lineCount; ++i)
        dl->AddText(ImVec2(barMin.x + pad.x, barMin.y + pad.y + lineHeight * (float)i), IM_COL32(225, 225, 230, 255), lines[i]);
}
