#pragma once
#include <dxgiformat.h>

// DXGI formatının okunabilir adı, ör. "BC7_UNORM_SRGB"
const char* FormatName(DXGI_FORMAT fmt);

// Formatın kısa açıklaması, ör. "Yüksek kaliteli RGBA sıkıştırma"
const char* FormatDescription(DXGI_FORMAT fmt);
