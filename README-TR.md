# TexPeek

[English](README.md) | [Türkçe](README-TR.md)

Oyun geliştiriciler için Windows'ta çalışan küçük ve hızlı bir texture görüntüleyici. Başta DDS olmak üzere dosyanın içinde gerçekte ne olduğunu gösterir: kanallar, mip'ler, alfa, normal'ler, HDR değerleri.

**Desteklenen biçimler:** DDS · TGA · HDR · EXR · PNG · JPG · BMP · TIF

## Özellikler
- DDS: BC1–BC7 ve BC6H dahil bütün DXGI formatları, eski (DX9) ve DX10 başlıklarıyla.
- Mip seviyeleri, cubemap yüzleri, texture dizisi katmanları ve 3D texture dilimleri arasında gezinme.
- Kanal görünümü (RGBA / RGB / R / G / B / A) ve fare altındaki pikselin ham değeri.
- HDR görüntüler için pozlama ve ACES tone mapping.
- Alfa eşiği (cutout) önizlemesi ve her mip'te görünür kalan alanın yüzdesi.
- Normal map: iki kanallı verilerde (BC5, DXT5nm) Z'yi hesaplama, yeşil kanalı çevirme.
- İki görüntüyü karşılaştırma: bölme çizgisi, yan yana, geçiş ve fark modları.
- Panoramik (equirectangular) HDRI'ler için 360° görünüm; skybox olarak nasıl duracağını görmek için.
- Tekrarlayan texture'ların ek yerlerini görmek için 3x3 döşeme önizlemesi.
- 16384 pikselden büyük görüntüler (örneğin 24k HDRI); büyük dosyalar arka planda yüklenir.
- Gösterilen mip / katmanı PNG, TGA ya da HDR olarak dışa aktarma.
- İsteğe bağlı Windows bütünleşmesi: DDS dosyalarını çift tıklayarak açma ve Gezgin'de küçük resimler (BC4 / BC5 / BC6H / BC7 dahil).
- Türkçe ve İngilizce arayüz.

## Kurulum
**Releases** sayfasından indir:
- **`TexPeek-Setup-x.y.z.exe`:** Kurulum dosyası. Yönetici izni istemez; DDS çift tıklamasını ve Gezgin küçük resimlerini kendisi kurar.
- **`TexPeek-x.y.z-portable.zip`:** Kurulumsuz sürüm. İstediğin klasöre aç ve `texpeek.exe`'yi çalıştır. Çift tıklama ve küçük resimleri istersen sol paneldeki **Windows > Kur** düğmesini kullan.

**Gereksinimler:** Windows 10/11 (64 bit). Başka bir şey kurmak gerekmez.

> Kurulum dosyası dijital olarak imzalı olmadığı için Windows SmartScreen uyarı verebilir. "Ek bilgi" > "Yine de çalıştır" ile devam edebilirsin.

## Kullanım
- Bir DDS dosyasına çift tıkla, dosyayı pencereye sürükle-bırak yap ya da **Aç** düğmesini kullan.
- Tekerlek yakınlaştırır, sürüklemek kaydırır. Çift tık görüntüyü pencereye sığdırır.
- Karşılaştırmak için iki dosyayı birlikte bırak ya da ikincisini `Shift` tuşuna basılı tutarak bırak.
- Dil seçici (`TR | EN`) araç çubuğunun sağ ucunda.

| Kısayol | İşlev |
|---|---|
| `R` `G` `B` `A` | Tek kanalı göster (tekrar basınca RGBA'ya döner) |
| `F` ya da çift tık | Pencereye sığdır |
| `1` | Gerçek boyut (1:1) |
| `Boşluk` | Geçiş modunda A ile B arasında atla |
| `Ctrl + O` | Dosya aç |
| `Ctrl + E` | Dışa aktar |

## Kaldırma
Windows Ayarlar > Uygulamalar'dan kaldır. Ayarlar dahil her şey silinir.
Kurulumsuz sürümde sol paneldeki **Windows > Kaldır** düğmesine basıp klasörü silmen yeterli.

Gezgin'in daha önce ürettiği küçük resimler bir süre görünmeye devam edebilir: Windows bunları kendi küçük resim önbelleğinde tutar. Disk Temizleme > "Küçük resimler" ile temizlenir.

## Kaynaktan derleme
Gerekenler: [Build Tools for Visual Studio](https://visualstudio.microsoft.com/downloads/) ("C++ ile masaüstü geliştirme" iş yükü, CMake'i de içerir), Git ve kurulum dosyası için Inno Setup:
```
winget install JRSoftware.InnoSetup
build-release.bat
```
Çıktılar `release\` klasörüne gider. Kütüphaneler ilk derlemede otomatik indirilir. Sürüm numarası `VERSION` dosyasında tutulur.

Yalnızca programı derlemek için:
```
cmake -B build
cmake --build build --config Release
```

## Lisans
MIT, bkz. [LICENSE](LICENSE). Üçüncü taraf bileşenler: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
