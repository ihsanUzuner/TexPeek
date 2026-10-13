#pragma once
// TexPeek'nun arayüzü: sol panel (bilgi + ayarlar) ve sağda texture tuvali.

#include "TextureDoc.h"
#include "imgui.h"
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class Viewer
{
public:
    Viewer(ID3D11Device* device, HWND hwnd)
        : m_device(device), m_hwnd(hwnd), m_doc(std::make_unique<TextureDoc>()), m_docB(std::make_unique<TextureDoc>()) {}
    ~Viewer();

    // Dosyayı arka planda yüklemeye başlar; yükleme bitince Draw() yeni görüntüye geçer.
    void Open(const std::wstring& path);
    // Aynısı, ama dosyayı karşılaştırmanın ikinci görüntüsü (B) olarak açar.
    void OpenCompare(const std::wstring& path);
    void Draw();   // her karede çağrılır

private:
    void DrawToolbar();
    void DrawSidePanel();
    void DrawComparePanel();
    void DrawIntegrationSection();   // çift tıkla açma ve Gezgin küçük resimleri: kur / kaldır
    bool Section(const char* label, bool openByDefault, bool forceOpen = false);   // açılır kapanır panel bölümü
    void HandleShortcuts();
    void SetActualSize();   // tuvalin ortasındaki noktayı koruyarak %100'e geç
    void DrawCanvas();
    void DrawCompare(const ImVec2& p0, const ImVec2& p1, bool hovered, bool active);
    void DrawPano(const ImVec2& p0, const ImVec2& p1, bool hovered, bool active);
    void DrawPixelStatus(const ImVec2& p0, const ImVec2& p1);
    void DrawLoadingCard(const ImVec2& p0, const ImVec2& p1);
    void ShowOpenDialog(bool compare);
    void ShowExportDialog();
    void FitToCanvas(const ImVec2& canvasSize);
    void ResetViewSettings();

    // Arka planda yükleme
    struct LoadRequest
    {
        std::wstring path;
        bool compare = false;   // true: B görüntüsü olarak yükle
    };
    bool IsLoading() const { return m_loadThread.joinable(); }
    void RequestLoad(const LoadRequest& request);
    void StartLoad(const LoadRequest& request);
    void PollLoad();   // yükleme bittiyse sonucu devral

    // Karşılaştırma
    bool CompareActive() const { return m_docB->HasView() && m_doc->HasView() && !m_pano; }
    void SyncCompareSelection();   // B'de de A ile aynı mip / katmanı göster

    // Görüntüyü çizen shader (kanal seçimi, pozlama, alfa eşiği, normal map görünümü, karşılaştırma)
    bool EnsureShader();
    void UpdateShaderParams(int compareMode = 0, float compareAmount = 0.0f);
    static void ApplyShader(const ImDrawList* list, const ImDrawCmd* cmd);
    static void BindSecondTexture(const ImDrawList* list, const ImDrawCmd* cmd);

    ID3D11Device* m_device;
    HWND m_hwnd;
    std::unique_ptr<TextureDoc> m_doc;    // ekranda gösterilen (hiç boş kalmaz; dosya yokken içi boş bir TextureDoc)
    std::unique_ptr<TextureDoc> m_docB;   // karşılaştırılan ikinci görüntü (yoksa içi boş)

    // Büyük dosyalar onlarca saniye sürebildiği için okuma ayrı bir iş parçacığında yapılır;
    // böylece pencere donmaz. İş parçacığı yalnızca m_loadingDoc'a dokunur, bitince m_loadDone'ı işaretler.
    std::unique_ptr<TextureDoc> m_loadingDoc;
    std::thread m_loadThread;
    std::atomic<bool> m_loadDone{ false };
    LoadRequest m_loading;                 // şu an yüklenen
    bool m_discardLoad = false;            // bu arada aynı yere başka dosya istendi: sonucu at
    std::vector<LoadRequest> m_loadQueue;  // sırada bekleyenler
    double m_loadStartTime = 0.0;
    unsigned long long m_loadingBytes = 0;

    // Tuval durumu
    float m_zoom = 1.0f;
    ImVec2 m_offset{ 0, 0 };     // görüntünün sol üst köşesinin tuvale göre konumu
    ImVec2 m_canvasSize{ 1, 1 }; // tek görüntünün çizildiği alan (yan yana karşılaştırmada tuvalin yarısı)
    ImVec2 m_lastCanvasSize{ 0, 0 };   // önceki karedeki m_canvasSize: pencere boyutu değişti mi anlamak için
    bool m_fitted = false;       // görüntü sığdırılmış duruyor (kullanıcı sonradan zoom / kaydırma yapmadı)
    int m_fitFrames = 0;         // kalan kare sayısı kadar "sığdır" uygulanır (ilk karede tuval boyutu henüz oturmamış olabiliyor)
    bool m_checkerboard = true;  // alfa için dama tahtası
    bool m_tilePreview = false;  // 3x3 döşeyerek ek yerlerini göster
    bool m_hasHoverPixel = false;
    int m_hoverX = 0, m_hoverY = 0;

    // Fare altındaki pikselin dosyadaki değeri (A ve varsa B)
    bool m_pixelValid = false, m_pixelBValid = false;
    int m_pixelX = -1, m_pixelY = -1;
    int m_pixelBX = 0, m_pixelBY = 0;
    float m_pixel[4] = {}, m_pixelB[4] = {};

    // Görünüm ayarları (yalnızca ekrandaki görüntüyü etkiler, dosyayı değil)
    enum Channel { ChannelRGBA, ChannelRGB, ChannelR, ChannelG, ChannelB, ChannelA };
    enum NormalMode { NormalOff, NormalRG, NormalAG };
    int m_channel = ChannelRGBA;
    int m_normalMode = NormalOff;   // eksik Z kanalını R,G'den (BC5) ya da A,G'den (DXT5nm) hesapla
    bool m_flipGreen = false;       // normal map yönü: DirectX <-> OpenGL
    bool m_linearData = false;      // veri linear: ekrana gamma uygulayarak göster
    bool m_toneMap = false;
    float m_exposure = 0.0f;        // EV (durak): +1 iki kat parlak
    bool m_alphaTest = false;
    float m_alphaThreshold = 0.5f;

    // 360° görünüm: görüntü bir kürenin içine sarılmış gibi, içinden bakılır (equirectangular HDRI)
    bool m_pano = false;
    float m_yaw = 0.0f, m_pitch = 0.0f;   // bakış yönü (radyan): sağa-sola, yukarı-aşağı
    float m_fov = 75.0f;                  // dikey görüş açısı (derece)

    // Karşılaştırma (A = m_doc, B = m_docB). B her zaman A'nın ekrandaki boyutuna gerilerek çizilir.
    enum CompareMode
    {
        CompareSplit,       // sürüklenebilir çizginin solunda A, sağında B
        CompareSideBySide,  // iki ayrı bölme, zoom ve kaydırma ortak
        CompareBlend,       // üst üste, A'dan B'ye soldurma
        CompareDiff,        // piksel farkı
    };
    int m_compareMode = CompareSplit;
    float m_split = 0.5f;        // bölme çizgisinin tuval genişliğine oranı
    bool m_dragSplit = false;
    float m_blend = 0.5f;        // 0 = yalnızca A, 1 = yalnızca B
    float m_diffGain = 8.0f;     // farkı kaç katına çıkararak göster

    // Shader'a her karede gönderilen ayarlar. Düzeni Viewer.cpp'deki HLSL "cbuffer" ile aynı olmalı.
    struct ShaderParams
    {
        int channel;
        int normalMode;
        int flags;
        float exposure;
        float alphaThreshold;
        float yaw;
        float pitch;
        float tanHalfFov;
        float aspect;
        int compareMode;       // 0 yok, 1 soldurma, 2 fark
        float compareAmount;
        float padding;
    };
    ShaderParams m_params{};
    ID3D11PixelShader* m_shader = nullptr;
    ID3D11Buffer* m_paramBuffer = nullptr;
    ID3D11SamplerState* m_panoSampler = nullptr;   // yatayda sarar: 360° görünümde arkadaki ek yeri görünmesin
    bool m_shaderFailed = false;

    std::string m_exportMessage;
    bool m_exportOk = false;
    std::string m_integrationMessage;
    bool m_integrationOk = false;

    // Panel bölümleri yeni dosya açılınca dosyanın türüne göre açılır / kapanır (HDR'de Renk, alfalı
    // texture'da Alfa gibi); sonrasında kullanıcının açıp kapattığı gibi kalır.
    bool m_applySectionDefaults = true;
    bool m_openCompareSection = false;   // B yeni yüklendi: Karşılaştırma bölümünü aç
};
