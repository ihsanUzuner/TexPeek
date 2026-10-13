#include "ShellIntegration.h"
#include "Lang.h"
#include "thumb/ThumbClsid.h"

#include <windows.h>
#include <shlobj.h>   // SHChangeNotify

namespace
{
    // Hepsi HKEY_CURRENT_USER\Software\Classes altında
    const wchar_t* kClasses = L"Software\\Classes\\";
    const wchar_t* kProgId = L"TexPeek.Texture";   // "TexPeek ile açılan dosya türü"nün Windows'taki adı
    const wchar_t* kAppKey = L"Applications\\texpeek.exe";
    // Windows'un "küçük resim sağlayıcı" eklenti noktasının sabit kimliği
    const wchar_t* kThumbnailHandler = L"\\ShellEx\\{e357fccd-a995-4576-b01f-234630154e96}";

    // "Birlikte aç" listesinde TexPeek'nun görüneceği türler. Çift tıklama varsayılanı yalnızca .dds için ayarlanır.
    const wchar_t* kExtensions[] = { L".dds", L".tga", L".hdr", L".exr" };

    std::wstring ExePath()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        return path;
    }

    std::wstring OpenCommand()
    {
        return L"\"" + ExePath() + L"\" \"%1\"";
    }

    std::wstring ThumbnailDllPath()
    {
        std::wstring path = ExePath();
        const size_t slash = path.find_last_of(L"\\/");
        return path.substr(0, slash + 1) + TEXPEEK_THUMBNAIL_DLL;
    }

    // name boşsa anahtarın "(Varsayılan)" değerini yazar. Aradaki anahtarlar yoksa oluşturulur.
    bool SetValue(const std::wstring& key, const wchar_t* name, const std::wstring& value)
    {
        const std::wstring full = kClasses + key;
        return RegSetKeyValueW(HKEY_CURRENT_USER, full.c_str(), name, REG_SZ,
            value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }

    std::wstring GetValue(const std::wstring& key, const wchar_t* name)
    {
        const std::wstring full = kClasses + key;
        wchar_t buffer[1024] = {};
        DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
        if (RegGetValueW(HKEY_CURRENT_USER, full.c_str(), name, RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS)
            return {};
        return buffer;
    }

    void DeleteTree(const std::wstring& key)
    {
        const std::wstring full = kClasses + key;
        RegDeleteTreeW(HKEY_CURRENT_USER, full.c_str());
    }

    void DeleteValue(const std::wstring& key, const wchar_t* name)
    {
        const std::wstring full = kClasses + key;
        RegDeleteKeyValueW(HKEY_CURRENT_USER, full.c_str(), name);
    }

    // Gezgin'e "dosya türü kayıtları değişti, simgeleri ve menüleri yenile" der.
    void NotifyShell()
    {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
}

bool ShellIntegration::IsInstalled()
{
    return GetValue(std::wstring(kProgId) + L"\\shell\\open\\command", nullptr) == OpenCommand();
}

bool ShellIntegration::Install(std::string& message)
{
    const std::wstring command = OpenCommand();
    const std::wstring progId = kProgId;
    bool ok = true;

    // 1) Dosya türü: "TexPeek.Texture türündeki dosyalar şu komutla açılır"
    ok &= SetValue(progId, nullptr, L"TexPeek texture");
    ok &= SetValue(progId + L"\\shell\\open\\command", nullptr, command);

    // 2) Uygulama kaydı: sağ tık > "Birlikte aç" listesinde düzgün adla görünmek için
    ok &= SetValue(kAppKey, L"FriendlyAppName", L"TexPeek");
    ok &= SetValue(std::wstring(kAppKey) + L"\\shell\\open\\command", nullptr, command);
    for (const wchar_t* ext : kExtensions)
    {
        ok &= SetValue(std::wstring(kAppKey) + L"\\SupportedTypes", ext, L"");
        ok &= SetValue(std::wstring(ext) + L"\\OpenWithProgids", kProgId, L"");
    }

    // 3) .dds çift tıklanınca TexPeek açılsın
    ok &= SetValue(L".dds", nullptr, progId);

    // 4) Küçük resim eklentisi (DLL exe'nin yanında duruyorsa)
    const std::wstring dll = ThumbnailDllPath();
    const bool hasDll = GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (hasDll)
    {
        const std::wstring clsidKey = std::wstring(L"CLSID\\") + TEXPEEK_THUMBNAIL_CLSID;
        ok &= SetValue(clsidKey, nullptr, L"TexPeek DDS thumbnail provider");
        ok &= SetValue(clsidKey + L"\\InprocServer32", nullptr, dll);
        ok &= SetValue(clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
        // Windows önce dosya türüne, sonra uzantıya bakar; ikisine de yazıyoruz.
        ok &= SetValue(progId + kThumbnailHandler, nullptr, TEXPEEK_THUMBNAIL_CLSID);
        ok &= SetValue(std::wstring(L".dds") + kThumbnailHandler, nullptr, TEXPEEK_THUMBNAIL_CLSID);
    }

    NotifyShell();

    if (!ok)
        message = Tr("Kayıt defterine yazılamadı.", "Could not write to the registry.");
    else if (!hasDll)
        message = Tr("Çift tıkla açma kuruldu. TexPeekThumb.dll bulunamadığı için küçük resimler kurulmadı.",
            "Double-click opening is installed. Thumbnails were not installed because TexPeekThumb.dll was not found.");
    else
        message = Tr("Kuruldu. Küçük resimler hemen görünmezse klasörü yenile ya da oturumu kapatıp aç.",
            "Installed. If thumbnails do not show up right away, refresh the folder or sign out and back in.");
    return ok;
}

bool ShellIntegration::Uninstall(std::string& message)
{
    const std::wstring progId = kProgId;

    // Yalnızca bizim yazdıklarımızı sil: .dds başka bir programa bağlanmışsa ona dokunma.
    if (GetValue(L".dds", nullptr) == progId)
        DeleteValue(L".dds", nullptr);
    if (GetValue(std::wstring(L".dds") + kThumbnailHandler, nullptr) == TEXPEEK_THUMBNAIL_CLSID)
        DeleteTree(std::wstring(L".dds") + kThumbnailHandler);
    for (const wchar_t* ext : kExtensions)
        DeleteValue(std::wstring(ext) + L"\\OpenWithProgids", kProgId);

    DeleteTree(progId);
    DeleteTree(kAppKey);
    DeleteTree(std::wstring(L"CLSID\\") + TEXPEEK_THUMBNAIL_CLSID);

    NotifyShell();
    message = Tr("Kaldırıldı. Dosya türleri Windows'un önceki ayarına döndü.",
        "Removed. File types are back to their previous Windows setting.");
    return true;
}
