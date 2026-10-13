#include "TextureDoc.h"
#include "Lang.h"

#include "tinyexr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <vector>

using namespace DirectX;

namespace
{
    std::wstring LowerExtension(const std::wstring& path)
    {
        const size_t dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos)
            return L"";
        std::wstring ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
        return ext;
    }

    std::string HResultText(const char* what, HRESULT hr)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), Tr("%s (hata kodu 0x%08X)", "%s (error code 0x%08X)"), what, (unsigned)hr);
        return buf;
    }

    // EXR dosyasını tinyexr ile okur; sonuç her zaman 32-bit float RGBA olur.
    bool LoadExr(const std::wstring& path, TexMetadata& meta, ScratchImage& image, std::string& error)
    {
        // Dosyayı kendimiz okuyoruz ki Türkçe karakterli yollar sorun çıkarmasın.
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            error = Tr("Dosya açılamadı.", "Could not open the file.");
            return false;
        }
        std::vector<unsigned char> bytes((size_t)file.tellg());
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)bytes.size());

        float* rgba = nullptr;
        int width = 0, height = 0;
        const char* detail = nullptr;
        if (LoadEXRFromMemory(&rgba, &width, &height, bytes.data(), bytes.size(), &detail) != TINYEXR_SUCCESS)
        {
            error = std::string(Tr("EXR okunamadı: ", "Could not read the EXR: ")) + (detail ? detail : Tr("bilinmeyen hata", "unknown error"));
            FreeEXRErrorMessage(detail);
            return false;
        }

        const HRESULT hr = image.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, (size_t)width, (size_t)height, 1, 1);
        if (SUCCEEDED(hr))
        {
            const Image* img = image.GetImage(0, 0, 0);
            const size_t rowBytes = (size_t)width * 4 * sizeof(float);
            for (size_t y = 0; y < (size_t)height; ++y)
                std::memcpy(img->pixels + y * img->rowPitch, reinterpret_cast<const uint8_t*>(rgba) + y * rowBytes, rowBytes);
            meta = image.GetMetadata();
        }
        std::free(rgba);

        if (FAILED(hr))
            error = HResultText(Tr("EXR için bellek ayrılamadı.", "Could not allocate memory for the EXR."), hr);
        return SUCCEEDED(hr);
    }

    // GPU'nun tek bir 2D texture için kabul ettiği en büyük kenar uzunluğu.
    int MaxTextureSize(ID3D11Device* device)
    {
        const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        if (level >= D3D_FEATURE_LEVEL_11_0) return 16384;
        if (level >= D3D_FEATURE_LEVEL_10_0) return 8192;
        if (level >= D3D_FEATURE_LEVEL_9_3) return 4096;
        return 2048;
    }

    // Görüntünün bir dikdörtgenini kopyalamadan gösterebiliyor muyuz?
    bool CanTile(DXGI_FORMAT format)
    {
        if (IsPlanar(format) || IsPacked(format) || IsPalettized(format))
            return false;
        return IsCompressed(format) || BitsPerPixel(format) % 8 == 0;
    }

    // Görüntünün (x, y) köşesinden başlayan w x h'lik parçası. Piksel verisi kopyalanmaz,
    // aynı belleğin içine işaret eder. Sıkıştırılmış formatlarda x ve y 4'ün katı olmalı.
    Image SubImage(const Image& img, size_t x, size_t y, size_t w, size_t h)
    {
        Image tile = img;
        tile.width = w;
        tile.height = h;
        if (IsCompressed(img.format))
        {
            const size_t blockBytes = BitsPerPixel(img.format) * 2;   // 4x4 blok = 16 piksel
            tile.pixels = img.pixels + (y / 4) * img.rowPitch + (x / 4) * blockBytes;
            tile.slicePitch = img.rowPitch * ((h + 3) / 4);
        }
        else
        {
            tile.pixels = img.pixels + y * img.rowPitch + x * (BitsPerPixel(img.format) / 8);
            tile.slicePitch = img.rowPitch * h;
        }
        return tile;
    }

    // Tek bir 2D görüntüyü GPU'ya yükler. GPU formatı kabul etmezse CPU'da RGBA8'e açıp tekrar dener.
    bool CreateTileView(ID3D11Device* device, const Image& img, const TexMetadata& meta,
        ID3D11ShaderResourceView** view, bool& converted, std::string& error)
    {
        TexMetadata single = meta;
        single.width = img.width;
        single.height = img.height;
        single.depth = 1;
        single.arraySize = 1;
        single.mipLevels = 1;
        single.format = img.format;
        single.dimension = TEX_DIMENSION_TEXTURE2D;
        single.miscFlags &= ~TEX_MISC_TEXTURECUBE;

        // IGNORE_SRGB: renkleri dosyadaki ham haliyle göster (ekranımız zaten sRGB).
        HRESULT hr = CreateShaderResourceViewEx(device, &img, 1, single,
            D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0, CREATETEX_IGNORE_SRGB, view);
        if (SUCCEEDED(hr))
            return true;

        // GPU bu formatı doğrudan gösteremiyor (ör. 4x4'ten küçük BC mip'i, eski formatlar).
        // CPU'da sıradan 8-bit RGBA'ya çevirip öyle gösteriyoruz.
        ScratchImage decoded;
        const Image* src = &img;
        if (IsCompressed(img.format))
        {
            hr = Decompress(img, DXGI_FORMAT_UNKNOWN, decoded);
            if (FAILED(hr))
            {
                error = HResultText(Tr("Sıkıştırılmış veri açılamadı.", "Could not decompress the data."), hr);
                return false;
            }
            src = decoded.GetImage(0, 0, 0);
        }

        // sRGB veriyi sRGB hedefe çeviriyoruz ki renk değerleri değişmesin.
        const DXGI_FORMAT target = IsSRGB(src->format) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
        ScratchImage rgba;
        if (src->format != target)
        {
            hr = Convert(*src, target, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, rgba);
            if (FAILED(hr))
            {
                error = HResultText(Tr("Bu format gösterilemiyor.", "This format cannot be displayed."), hr);
                return false;
            }
            src = rgba.GetImage(0, 0, 0);
        }

        single.format = src->format;
        hr = CreateShaderResourceViewEx(device, src, 1, single,
            D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0, CREATETEX_IGNORE_SRGB, view);
        if (FAILED(hr))
        {
            error = HResultText(Tr("GPU'ya yüklenemedi.", "Could not upload to the GPU."), hr);
            return false;
        }
        converted = true;
        return true;
    }
}

bool TextureDoc::Load(ID3D11Device* device, const std::wstring& path)
{
    ReleaseView();
    m_image.Release();
    m_meta = {};
    m_loaded = false;
    m_error.clear();
    m_path = path;
    m_mip = m_item = m_slice = 0;

    const std::wstring ext = LowerExtension(path);
    if (ext == L".exr")
    {
        if (!LoadExr(path, m_meta, m_image, m_error))
        {
            m_image.Release();
            return false;
        }
        m_loaded = true;
        return BuildView(device);
    }

    HRESULT hr;
    if (ext == L".dds")
        hr = LoadFromDDSFile(path.c_str(), DDS_FLAGS_NONE, &m_meta, m_image);
    else if (ext == L".tga")
        hr = LoadFromTGAFile(path.c_str(), TGA_FLAGS_NONE, &m_meta, m_image);
    else if (ext == L".hdr")
        hr = LoadFromHDRFile(path.c_str(), &m_meta, m_image);
    else
        hr = LoadFromWICFile(path.c_str(), WIC_FLAGS_NONE, &m_meta, m_image);   // png, jpg, bmp, tif...

    if (FAILED(hr))
    {
        m_error = HResultText(Tr("Dosya okunamadı. Desteklenmeyen ya da bozuk bir dosya olabilir.",
            "Could not read the file. It may be unsupported or corrupt."), hr);
        m_image.Release();
        return false;
    }

    m_loaded = true;
    return BuildView(device);
}

int TextureDoc::SliceCount() const
{
    if (!m_loaded || !m_meta.IsVolumemap())
        return 1;
    return (int)std::max<size_t>(1, m_meta.depth >> m_mip);
}

void TextureDoc::Select(ID3D11Device* device, int mip, int item, int slice)
{
    if (!m_loaded)
        return;
    m_mip = std::clamp(mip, 0, (int)m_meta.mipLevels - 1);
    m_item = std::clamp(item, 0, (int)m_meta.arraySize - 1);
    m_slice = std::clamp(slice, 0, SliceCount() - 1);
    BuildView(device);
}

bool TextureDoc::BuildView(ID3D11Device* device)
{
    ReleaseView();

    const Image* img = m_image.GetImage(m_mip, m_item, m_slice);
    if (!img)
    {
        m_error = Tr("Seçilen mip / katman bulunamadı.", "The selected mip / layer was not found.");
        return false;
    }

    // Tek bir 2D görüntü olarak GPU'ya yüklüyoruz (cubemap / array / 3D fark etmez).
    // GPU'nun sınırından büyükse (ör. 19k HDRI) küçültmek yerine karolara bölüyoruz.
    const size_t limit = (size_t)MaxTextureSize(device);
    size_t tileSize = std::max(img->width, img->height);
    if (tileSize > limit && CanTile(img->format))
        tileSize = std::min<size_t>(limit, 4096);

    for (size_t y = 0; y < img->height; y += tileSize)
    {
        for (size_t x = 0; x < img->width; x += tileSize)
        {
            const size_t w = std::min(tileSize, img->width - x);
            const size_t h = std::min(tileSize, img->height - y);
            const Image part = SubImage(*img, x, y, w, h);

            ViewTile tile;
            tile.x = (int)x;
            tile.y = (int)y;
            tile.width = (int)w;
            tile.height = (int)h;
            if (!CreateTileView(device, part, m_meta, &tile.view, m_viewConverted, m_error))
            {
                ReleaseView();
                return false;
            }
            m_tiles.push_back(tile);
        }
    }

    m_viewWidth = (int)img->width;
    m_viewHeight = (int)img->height;
    m_error.clear();
    return true;
}

void TextureDoc::ReleaseView()
{
    for (ViewTile& tile : m_tiles)
        tile.view->Release();
    m_tiles.clear();
    if (m_panoView)
    {
        m_panoView->Release();
        m_panoView = nullptr;
    }
    m_panoFailed = false;
    m_viewWidth = m_viewHeight = 0;
    m_viewConverted = false;
    m_alphaState = HistogramState::None;
}

ID3D11ShaderResourceView* TextureDoc::PanoView(ID3D11Device* device)
{
    if (m_tiles.empty())
        return nullptr;
    if (m_tiles.size() == 1)
        return m_tiles[0].view;   // GPU'ya tek parça sığmış, olduğu gibi kullan
    if (m_panoView || m_panoFailed)
        return m_panoView;
    m_panoFailed = true;   // başarısız olursa her karede yeniden deneme

    const Image* src = m_image.GetImage(m_mip, m_item, m_slice);
    if (!src)
        return nullptr;

    ScratchImage decoded;
    if (IsCompressed(src->format))
    {
        if (FAILED(Decompress(*src, DXGI_FORMAT_UNKNOWN, decoded)))
            return nullptr;
        src = decoded.GetImage(0, 0, 0);
    }

    // En uzun kenar 8192 olacak şekilde küçült: ekrandaki 360° görünüm için fazlasıyla yeterli.
    const double scale = 8192.0 / (double)std::max(src->width, src->height);
    const size_t w = std::max<size_t>(1, (size_t)std::lround((double)src->width * scale));
    const size_t h = std::max<size_t>(1, (size_t)std::lround((double)src->height * scale));
    ScratchImage resized;
    if (FAILED(Resize(*src, w, h, TEX_FILTER_LINEAR | TEX_FILTER_FORCE_NON_WIC | TEX_FILTER_WRAP_U, resized)))
        return nullptr;
    const Image* out = resized.GetImage(0, 0, 0);

    // 32-bit float yerine 16-bit float: ekran kartı belleğinin yarısı, görüntüde fark yok.
    ScratchImage half;
    if (out->format == DXGI_FORMAT_R32G32B32A32_FLOAT &&
        SUCCEEDED(Convert(*out, DXGI_FORMAT_R16G16B16A16_FLOAT, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, half)))
        out = half.GetImage(0, 0, 0);

    bool converted = false;
    std::string error;
    if (CreateTileView(device, *out, m_meta, &m_panoView, converted, error))
        m_panoFailed = false;
    return m_panoView;
}

bool TextureDoc::ReadPixel(int x, int y, float rgba[4]) const
{
    const Image* img = m_loaded ? m_image.GetImage(m_mip, m_item, m_slice) : nullptr;
    if (!img || !CanTile(img->format) || x < 0 || y < 0 || (size_t)x >= img->width || (size_t)y >= img->height)
        return false;

    // MakeLinear: sRGB etiketli formatlarda bile değeri çevirmeden, dosyada yazdığı gibi oku.
    const DXGI_FORMAT target = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ScratchImage out;
    const float* src = nullptr;

    if (IsCompressed(img->format))
    {
        // Sıkıştırılmış formatlarda pikseller 4x4 bloklar halinde saklanır; pikselin bloğunu açıyoruz.
        const size_t bx = (size_t)x & ~size_t(3), by = (size_t)y & ~size_t(3);
        Image block = SubImage(*img, bx, by, std::min<size_t>(4, img->width - bx), std::min<size_t>(4, img->height - by));
        block.format = MakeLinear(block.format);
        if (FAILED(Decompress(block, target, out)))
            return false;
        const Image* px = out.GetImage(0, 0, 0);
        src = reinterpret_cast<const float*>(px->pixels + ((size_t)y - by) * px->rowPitch) + ((size_t)x - bx) * 4;
    }
    else
    {
        Image one = SubImage(*img, (size_t)x, (size_t)y, 1, 1);
        one.format = MakeLinear(one.format);
        if (one.format == target)
            src = reinterpret_cast<const float*>(one.pixels);
        else
        {
            if (FAILED(Convert(one, target, TEX_FILTER_FORCE_NON_WIC, TEX_THRESHOLD_DEFAULT, out)))
                return false;
            src = reinterpret_cast<const float*>(out.GetImage(0, 0, 0)->pixels);
        }
    }

    std::copy(src, src + 4, rgba);
    return true;
}

float TextureDoc::AlphaCoverage(float threshold)
{
    if (m_alphaState == HistogramState::None)
    {
        m_alphaState = HistogramState::Failed;
        std::fill(std::begin(m_alphaHistogram), std::end(m_alphaHistogram), size_t(0));

        const Image* img = m_loaded ? m_image.GetImage(m_mip, m_item, m_slice) : nullptr;
        const size_t maxPixels = size_t(8192) * 8192;   // daha büyüğünde arayüz uzun süre donar
        if (img && img->width * img->height <= maxPixels)
        {
            const DXGI_FORMAT target = DXGI_FORMAT_R8G8B8A8_UNORM;
            Image src = *img;
            src.format = MakeLinear(src.format);

            ScratchImage out;
            const Image* rgba = &src;
            HRESULT hr = S_OK;
            if (IsCompressed(src.format))
                hr = Decompress(src, target, out);
            else if (src.format != target)
                hr = Convert(src, target, TEX_FILTER_FORCE_NON_WIC, TEX_THRESHOLD_DEFAULT, out);
            if (SUCCEEDED(hr))
            {
                if (out.GetImageCount() > 0)
                    rgba = out.GetImage(0, 0, 0);
                for (size_t y = 0; y < rgba->height; ++y)
                {
                    const uint8_t* row = rgba->pixels + y * rgba->rowPitch;
                    for (size_t x = 0; x < rgba->width; ++x)
                        ++m_alphaHistogram[row[x * 4 + 3]];
                }
                m_alphaState = HistogramState::Ready;
            }
        }
    }

    if (m_alphaState != HistogramState::Ready)
        return -1.0f;

    // Shader "alfa >= eşik" olanları çiziyor; aynı kuralı 0-255 basamaklarına uyguluyoruz.
    const int first = std::clamp((int)std::ceil(threshold * 255.0f - 0.0001f), 0, 256);
    size_t visible = 0, total = 0;
    for (int i = 0; i < 256; ++i)
    {
        total += m_alphaHistogram[i];
        if (i >= first)
            visible += m_alphaHistogram[i];
    }
    return total > 0 ? (float)visible / (float)total : -1.0f;
}

bool TextureDoc::Export(const std::wstring& path, std::string& message) const
{
    const Image* img = m_loaded ? m_image.GetImage(m_mip, m_item, m_slice) : nullptr;
    if (!img)
    {
        message = Tr("Kaydedilecek görüntü yok.", "There is no image to save.");
        return false;
    }

    const std::wstring ext = LowerExtension(path);
    if (ext != L".png" && ext != L".tga" && ext != L".hdr")
    {
        message = Tr("Dosya adının uzantısı .png, .tga ya da .hdr olmalı.", "The file extension must be .png, .tga or .hdr.");
        return false;
    }

    // Sıkıştırılmış veriyi önce düz piksellere açıyoruz.
    ScratchImage decoded;
    const Image* src = img;
    HRESULT hr;
    if (IsCompressed(img->format))
    {
        hr = Decompress(*img, DXGI_FORMAT_UNKNOWN, decoded);
        if (FAILED(hr))
        {
            message = HResultText(Tr("Sıkıştırılmış veri açılamadı.", "Could not decompress the data."), hr);
            return false;
        }
        src = decoded.GetImage(0, 0, 0);
    }

    // Her dosya türü yalnızca belli piksel formatlarını yazabiliyor; gerekiyorsa en yakınına çeviriyoruz.
    const DXGI_FORMAT f = src->format;
    const bool isFloat = FormatDataType(f) == FORMAT_TYPE_FLOAT;
    const DXGI_FORMAT rgba8 = IsSRGB(f) ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    const bool isRgba8 =
        f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
        f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    DXGI_FORMAT target = f;
    if (ext == L".png")
    {
        const bool direct = isRgba8 || f == DXGI_FORMAT_R8_UNORM || f == DXGI_FORMAT_R16_UNORM || f == DXGI_FORMAT_R16G16B16A16_UNORM;
        if (!direct)
            target = (!isFloat && BitsPerColor(f) > 8) ? DXGI_FORMAT_R16G16B16A16_UNORM : rgba8;
    }
    else if (ext == L".tga")
    {
        if (!isRgba8 && f != DXGI_FORMAT_R8_UNORM)
            target = rgba8;
    }
    else
    {
        if (f != DXGI_FORMAT_R32G32B32A32_FLOAT && f != DXGI_FORMAT_R16G16B16A16_FLOAT)
            target = DXGI_FORMAT_R32G32B32A32_FLOAT;
    }

    ScratchImage converted;
    if (target != f)
    {
        hr = Convert(*src, target, TEX_FILTER_DEFAULT, TEX_THRESHOLD_DEFAULT, converted);
        if (FAILED(hr))
        {
            message = HResultText(Tr("Bu format dışa aktarılamıyor.", "This format cannot be exported."), hr);
            return false;
        }
        src = converted.GetImage(0, 0, 0);
    }

    if (ext == L".png")
        // FORCE_SRGB: PNG'ye "gamma 1.0" etiketi yazılmasın, yoksa çoğu program resmi soluk gösterir.
        hr = SaveToWICFile(*src, WIC_FLAGS_FORCE_SRGB, GetWICCodec(WIC_CODEC_PNG), path.c_str());
    else if (ext == L".tga")
        hr = SaveToTGAFile(*src, TGA_FLAGS_NONE, path.c_str());
    else
        hr = SaveToHDRFile(*src, path.c_str());

    if (FAILED(hr))
    {
        message = HResultText(Tr("Dosya yazılamadı.", "Could not write the file."), hr);
        return false;
    }

    char buf[200];
    std::snprintf(buf, sizeof(buf), Tr("Kaydedildi: %d x %d%s", "Saved: %d x %d%s"), (int)src->width, (int)src->height,
        (isFloat && ext != L".hdr")
            ? Tr(". Uyarı: HDR değerler 0-1 aralığına kırpıldı, tam aralık için .hdr seç.",
                ". Warning: HDR values were clipped to 0-1, choose .hdr for the full range.")
            : "");
    message = buf;
    return true;
}
