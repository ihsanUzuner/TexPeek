#pragma once
// Açılan bir texture dosyası: diskten okunan veri + ekranda gösterilen GPU kopyası.

#include <d3d11.h>
#include <DirectXTex.h>
#include <string>
#include <vector>

// Ekranda gösterilen görüntünün bir parçası. Çoğu texture tek karodur;
// GPU'nun tek texture sınırını (DX11'de 16384) aşanlar karolara bölünür.
struct ViewTile
{
    ID3D11ShaderResourceView* view = nullptr;
    int x = 0, y = 0;            // karonun görüntü içindeki sol üst köşesi (piksel)
    int width = 0, height = 0;
};

class TextureDoc
{
public:
    ~TextureDoc() { ReleaseView(); }

    // Dosyayı uzantısına göre okur (.dds, .tga, .hdr, .exr, diğerleri WIC: png/jpg/bmp...).
    bool Load(ID3D11Device* device, const std::wstring& path);

    bool IsLoaded() const { return m_loaded; }
    const std::wstring& Path() const { return m_path; }
    const std::string& Error() const { return m_error; }
    const DirectX::TexMetadata& Meta() const { return m_meta; }
    size_t MemorySize() const { return m_image.GetPixelsSize(); }

    // Hangi mip / katman (array veya cubemap yüzü) / derinlik dilimi gösteriliyor
    int Mip() const { return m_mip; }
    int Item() const { return m_item; }
    int Slice() const { return m_slice; }
    int SliceCount() const;   // 3D texture'da seçili mip'in derinliği, diğerlerinde 1
    void Select(ID3D11Device* device, int mip, int item, int slice);

    // Ekranda gösterilen görüntü
    bool HasView() const { return !m_tiles.empty(); }
    const std::vector<ViewTile>& Tiles() const { return m_tiles; }
    int ViewWidth() const { return m_viewWidth; }
    int ViewHeight() const { return m_viewHeight; }
    bool ViewWasConverted() const { return m_viewConverted; }

    // 360° görünüm için görüntünün tek parça GPU kopyası. Karolara bölünmüş dev görüntülerde
    // ilk istendiğinde küçültülmüş bir kopya üretilir (birkaç saniye sürebilir). Üretilemezse nullptr.
    ID3D11ShaderResourceView* PanoView(ID3D11Device* device);

    // Gösterilen görüntünün tek pikselini dosyadaki ham değeriyle okur (sRGB çevrimi yapılmaz).
    bool ReadPixel(int x, int y, float rgba[4]) const;

    // Gösterilen görüntüde alfası eşiğin üstünde kalan piksellerin oranı (0..1).
    // Hesaplanamıyorsa (çok büyük ya da desteklenmeyen format) -1 döner.
    float AlphaCoverage(float threshold);

    // Gösterilen görüntüyü kaydeder; tür uzantıdan anlaşılır (.png, .tga, .hdr).
    // Sonuç ya da hata metni message'a yazılır.
    bool Export(const std::wstring& path, std::string& message) const;

private:
    bool BuildView(ID3D11Device* device);
    void ReleaseView();

    std::wstring m_path;
    std::string m_error;
    bool m_loaded = false;

    DirectX::TexMetadata m_meta{};
    DirectX::ScratchImage m_image;   // dosyadaki orijinal veri, hiç değiştirmeden

    int m_mip = 0, m_item = 0, m_slice = 0;

    std::vector<ViewTile> m_tiles;
    int m_viewWidth = 0, m_viewHeight = 0;
    bool m_viewConverted = false;    // GPU doğrudan gösteremediyse CPU'da açtık mı

    ID3D11ShaderResourceView* m_panoView = nullptr;   // yalnızca karolu görüntülerde ayrı bir kopya
    bool m_panoFailed = false;

    // Alfa dağılımı: ilk istendiğinde hesaplanır, görüntü değişince sıfırlanır
    enum class HistogramState { None, Ready, Failed };
    HistogramState m_alphaState = HistogramState::None;
    size_t m_alphaHistogram[256] = {};
};
