// TexPeekThumb.dll - Windows Gezgini için DDS küçük resim (thumbnail) eklentisi.
//
// Windows'un kendi DDS desteği yalnızca eski formatları (BC1-BC3) tanır. Bu eklenti DirectXTex
// kullanarak BC4, BC5, BC6H, BC7 ve sıkıştırmasız formatlar dahil hepsinin küçük resmini üretir.
//
// Nasıl çalışır: Gezgin bir .dds dosyasının küçük resmine ihtiyaç duyunca bu DLL'i (ayrı, korumalı
// bir yardımcı süreçte) yükler, ThumbProvider nesnesi oluşturur, Initialize ile dosyanın içeriğini
// verir, sonra GetThumbnail ile istediği boyutta bir bitmap ister.
// DLL'i Windows'a tanıtan kayıtlar texpeek.exe tarafından yazılır (bkz. ShellIntegration.cpp).

#include <windows.h>
#include <objbase.h>
#include <propsys.h>      // IInitializeWithStream
#include <thumbcache.h>   // IThumbnailProvider

#include <DirectXTex.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <vector>

#include "ThumbClsid.h"

using namespace DirectX;

namespace
{
    std::atomic<long> g_liveObjects{ 0 };   // DLL'in bellekten atılıp atılamayacağını bilmek için

    // Bundan büyük dosyalara küçük resim üretmiyoruz: Gezgin'in bir klasörü açarken
    // gigabaytlarca bellek harcayıp donması, eksik bir önizlemeden daha kötü.
    const ULONGLONG kMaxFileBytes = 512ull * 1024 * 1024;

    HRESULT ReadWholeStream(IStream* stream, std::vector<uint8_t>& out)
    {
        STATSTG stat = {};
        HRESULT hr = stream->Stat(&stat, STATFLAG_NONAME);
        if (FAILED(hr))
            return hr;
        const ULONGLONG size = stat.cbSize.QuadPart;
        if (size == 0 || size > kMaxFileBytes)
            return E_FAIL;

        const LARGE_INTEGER start = {};
        hr = stream->Seek(start, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr))
            return hr;

        out.resize((size_t)size);
        size_t done = 0;
        while (done < out.size())
        {
            const ULONG want = (ULONG)std::min<size_t>(out.size() - done, 16 * 1024 * 1024);
            ULONG got = 0;
            hr = stream->Read(out.data() + done, want, &got);
            if (FAILED(hr))
                return hr;
            if (got == 0)
                return E_FAIL;   // dosya beklenenden kısa
            done += got;
        }
        return S_OK;
    }

    // DDS dosyasından, en uzun kenarı en fazla "size" piksel olan 32-bit bir bitmap üretir.
    HRESULT MakeThumbnail(const std::vector<uint8_t>& file, UINT size, HBITMAP* bitmap)
    {
        TexMetadata meta = {};
        ScratchImage image;
        HRESULT hr = LoadFromDDSMemory(file.data(), file.size(), DDS_FLAGS_NONE, &meta, image);
        if (FAILED(hr))
            return hr;

        // İstenen boyuttan küçük olmayan en küçük mip'i seç: hem hızlı hem de küçültme kalitesi iyi olur.
        size_t mip = 0;
        while (mip + 1 < meta.mipLevels &&
            std::max(meta.width >> (mip + 1), meta.height >> (mip + 1)) >= size)
            ++mip;

        const Image* img = image.GetImage(mip, 0, 0);   // cubemap / dizilerde ilk yüz
        if (!img)
            return E_FAIL;

        ScratchImage decoded;
        if (IsCompressed(img->format))
        {
            hr = Decompress(*img, DXGI_FORMAT_UNKNOWN, decoded);
            if (FAILED(hr))
                return hr;
            img = decoded.GetImage(0, 0, 0);
        }

        // Windows bitmap'i BGRA sırasında 8-bit ister. sRGB ve düz veriler olduğu gibi kopyalanır;
        // HDR (float) veriler ekranda doğru görünsün diye sRGB'ye çevrilir.
        const bool gammaEncode = IsSRGB(img->format) || FormatDataType(img->format) == FORMAT_TYPE_FLOAT;
        const DXGI_FORMAT target = gammaEncode ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
        ScratchImage converted;
        if (img->format != target)
        {
            hr = Convert(*img, target, TEX_FILTER_FORCE_NON_WIC, TEX_THRESHOLD_DEFAULT, converted);
            if (FAILED(hr))
                return hr;
            img = converted.GetImage(0, 0, 0);
        }

        // Hâlâ büyükse (mip'i olmayan dosyalar) küçült
        ScratchImage resized;
        const size_t longest = std::max(img->width, img->height);
        if (longest > size)
        {
            const size_t w = std::max<size_t>(1, img->width * size / longest);
            const size_t h = std::max<size_t>(1, img->height * size / longest);
            // Triangle filtresi büyük küçültmelerde temiz sonuç verir ama dev görüntülerde yavaştır.
            const bool huge = img->width * img->height > size_t(4096) * 4096;
            const TEX_FILTER_FLAGS filter = (huge ? TEX_FILTER_LINEAR : TEX_FILTER_TRIANGLE) | TEX_FILTER_FORCE_NON_WIC;
            hr = Resize(*img, w, h, filter, resized);
            if (FAILED(hr))
                return hr;
            img = resized.GetImage(0, 0, 0);
        }

        if (img->width > 0x4000 || img->height > 0x4000)
            return E_FAIL;

        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = (LONG)img->width;
        info.bmiHeader.biHeight = -(LONG)img->height;   // eksi: satırlar yukarıdan aşağı
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP result = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!result || !bits)
            return E_OUTOFMEMORY;

        // BC5: yalnızca X ve Y saklanan normal map. Eksik Z'yi hesaplayıp maviye yazıyoruz ki
        // küçük resim sarı değil, alışık olunan mavi-mor normal map gibi görünsün.
        const bool twoChannelNormal = meta.format == DXGI_FORMAT_BC5_UNORM || meta.format == DXGI_FORMAT_BC5_SNORM;

        for (size_t y = 0; y < img->height; ++y)
        {
            const uint8_t* src = img->pixels + y * img->rowPitch;
            uint8_t* dst = static_cast<uint8_t*>(bits) + y * img->width * 4;
            std::memcpy(dst, src, img->width * 4);
            for (size_t x = 0; x < img->width; ++x)
            {
                uint8_t* px = dst + x * 4;   // B, G, R, A
                if (twoChannelNormal)
                {
                    const float nx = px[2] / 255.0f * 2.0f - 1.0f;
                    const float ny = px[1] / 255.0f * 2.0f - 1.0f;
                    const float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
                    px[0] = (uint8_t)std::lround((nz * 0.5f + 0.5f) * 255.0f);
                }
                // Alfayı yok sayıyoruz: oyun texture'larında alfaya çoğu zaman başka veri
                // (pürüzsüzlük vb.) paketlenir ve saydamlık sayılırsa küçük resim boş görünür.
                px[3] = 255;
            }
        }

        *bitmap = result;
        return S_OK;
    }

    // Gezgin'in konuştuğu nesne.
    class ThumbProvider final : public IInitializeWithStream, public IThumbnailProvider
    {
    public:
        ThumbProvider() { ++g_liveObjects; }

        // IUnknown
        IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
        {
            if (!out)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IInitializeWithStream))
                *out = static_cast<IInitializeWithStream*>(this);
            else if (riid == __uuidof(IThumbnailProvider))
                *out = static_cast<IThumbnailProvider*>(this);
            else
            {
                *out = nullptr;
                return E_NOINTERFACE;
            }
            AddRef();
            return S_OK;
        }
        IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)++m_refs; }
        IFACEMETHODIMP_(ULONG) Release() override
        {
            const long refs = --m_refs;
            if (refs == 0)
                delete this;
            return (ULONG)refs;
        }

        // IInitializeWithStream: Gezgin dosyanın içeriğini verir
        IFACEMETHODIMP Initialize(IStream* stream, DWORD) override
        {
            if (!stream)
                return E_INVALIDARG;
            if (m_stream)
                return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
            m_stream = stream;
            m_stream->AddRef();
            return S_OK;
        }

        // IThumbnailProvider: Gezgin "cx" piksellik küçük resim ister
        IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override
        {
            if (!bitmap || !alpha)
                return E_POINTER;
            *bitmap = nullptr;
            *alpha = WTSAT_RGB;
            if (!m_stream || cx == 0)
                return E_UNEXPECTED;

            // Bozuk bir dosya yüzünden yardımcı süreç çökmesin: her hata "küçük resim yok" demek.
            try
            {
                std::vector<uint8_t> file;
                const HRESULT hr = ReadWholeStream(m_stream, file);
                if (FAILED(hr))
                    return hr;
                return MakeThumbnail(file, cx, bitmap);
            }
            catch (...)
            {
                return E_FAIL;
            }
        }

    private:
        ~ThumbProvider()
        {
            if (m_stream)
                m_stream->Release();
            --g_liveObjects;
        }

        std::atomic<long> m_refs{ 1 };
        IStream* m_stream = nullptr;
    };

    // Windows nesneyi doğrudan değil, bu "fabrika" üzerinden oluşturur.
    class ClassFactory final : public IClassFactory
    {
    public:
        IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
        {
            if (!out)
                return E_POINTER;
            if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory))
            {
                *out = static_cast<IClassFactory*>(this);
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        // Fabrika programla birlikte yaşayan tek bir nesne; sayaç tutmaya gerek yok.
        IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
        IFACEMETHODIMP_(ULONG) Release() override { return 1; }

        IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override
        {
            if (!out)
                return E_POINTER;
            *out = nullptr;
            if (outer)
                return CLASS_E_NOAGGREGATION;
            ThumbProvider* provider = new (std::nothrow) ThumbProvider();
            if (!provider)
                return E_OUTOFMEMORY;
            const HRESULT hr = provider->QueryInterface(riid, out);
            provider->Release();
            return hr;
        }
        IFACEMETHODIMP LockServer(BOOL lock) override
        {
            if (lock) ++g_liveObjects; else --g_liveObjects;
            return S_OK;
        }
    };

    ClassFactory g_factory;
}

// ---- Windows'un DLL'den çağırdığı iki fonksiyon ----
#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out)
{
    if (!out)
        return E_POINTER;
    *out = nullptr;
    if (clsid != CLSID_TexPeekThumbnail)
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(riid, out);
}

STDAPI DllCanUnloadNow()
{
    return g_liveObjects == 0 ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
