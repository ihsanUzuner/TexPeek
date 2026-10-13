#pragma once
// Küçük resim eklentisinin Windows'taki kimliği (CLSID). Eklenti DLL'i ve onu kaydeden
// texpeek.exe aynı değeri kullanmalı; ikisi de bu dosyayı içerir.

#include <guiddef.h>

// {4B4F107B-4F8A-4A54-BDEF-2E7DC77F6CF8}
inline constexpr GUID CLSID_TexPeekThumbnail =
    { 0x4b4f107b, 0x4f8a, 0x4a54, { 0xbd, 0xef, 0x2e, 0x7d, 0xc7, 0x7f, 0x6c, 0xf8 } };

#define TEXPEEK_THUMBNAIL_CLSID L"{4B4F107B-4F8A-4A54-BDEF-2E7DC77F6CF8}"
#define TEXPEEK_THUMBNAIL_DLL L"TexPeekThumb.dll"
