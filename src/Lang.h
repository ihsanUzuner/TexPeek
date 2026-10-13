#pragma once
// Arayüz dili: Türkçe ya da İngilizce.
//
// Metinler ayrı bir çeviri tablosunda değil, kullanıldıkları yerde iki dilde birlikte yazılır:
//     ImGui::Button(Tr("Aç...", "Open..."))
// Tr seçili dile göre ikisinden birini döndürür.

enum class Lang { Turkish, English };

Lang CurrentLang();

// Program açılırken bir kez çağrılır: kayıtlı seçimi okur; yoksa Windows'un diline bakar
// (Türkçe Windows'ta Türkçe, diğer hepsinde İngilizce).
void InitLang();

// Dili değiştirir ve bir sonraki açılış için kaydeder.
void SetLang(Lang lang);

const char* Tr(const char* tr, const char* en);
const wchar_t* Tr(const wchar_t* tr, const wchar_t* en);   // Windows pencereleri için (UTF-16)
