#pragma once
// Windows ile bütünleşme: .dds dosyalarına çift tıklayınca TexPeek'nun açılması ve
// Gezgin'de DDS küçük resimleri (TexPeekThumb.dll).
//
// Bütün kayıtlar yalnızca o anki kullanıcıya yazılır (HKEY_CURRENT_USER); yönetici izni gerekmez
// ve Uninstall hepsini geri alır. Kayıtlar exe'nin o anki konumunu gösterir: program başka bir
// klasöre taşınırsa Install yeniden çalıştırılmalı.

#include <string>

namespace ShellIntegration
{
    // Kayıtlar şu anki exe'yi gösteriyor mu?
    bool IsInstalled();

    // Sonucu anlatan metin (UTF-8) message'a yazılır.
    bool Install(std::string& message);
    bool Uninstall(std::string& message);
}
