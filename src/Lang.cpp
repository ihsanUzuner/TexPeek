#include "Lang.h"

#include <windows.h>
#include <atomic>
#include <cwchar>

namespace
{
    // Dosya yükleyen arka plan iş parçacığı da hata metni üretirken okuyor, o yüzden atomic.
    std::atomic<Lang> g_lang{ Lang::English };

    // Seçim HKEY_CURRENT_USER\Software\TexPeek altında "Language" = "tr" / "en" olarak durur.
    // Kurulum programı da seçilen dili aynı yere yazar (installer\TexPeek.iss).
    const wchar_t* kSettingsKey = L"Software\\TexPeek";
    const wchar_t* kLanguageValue = L"Language";
}

Lang CurrentLang()
{
    return g_lang;
}

void InitLang()
{
    wchar_t saved[16] = {};
    DWORD bytes = sizeof(saved) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kLanguageValue, RRF_RT_REG_SZ, nullptr, saved, &bytes) == ERROR_SUCCESS)
    {
        g_lang = wcscmp(saved, L"tr") == 0 ? Lang::Turkish : Lang::English;
        return;
    }
    g_lang = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_TURKISH ? Lang::Turkish : Lang::English;
}

void SetLang(Lang lang)
{
    g_lang = lang;
    const wchar_t* code = lang == Lang::Turkish ? L"tr" : L"en";
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, kLanguageValue, REG_SZ,
        code, (DWORD)((wcslen(code) + 1) * sizeof(wchar_t)));
}

const char* Tr(const char* tr, const char* en)
{
    return g_lang == Lang::Turkish ? tr : en;
}

const wchar_t* Tr(const wchar_t* tr, const wchar_t* en)
{
    return g_lang == Lang::Turkish ? tr : en;
}
