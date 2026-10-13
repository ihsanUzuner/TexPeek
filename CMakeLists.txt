cmake_minimum_required(VERSION 3.21)

# Sürüm numarası tek yerde durur: kökteki VERSION dosyası. Exe'nin özellikleri, build-release.bat
# ve kurulum programı da oradan okur.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" TEXPEEK_VERSION LIMIT_COUNT 1)
string(STRIP "${TEXPEEK_VERSION}" TEXPEEK_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)   # VERSION değişince yeniden yapılandır

project(TexPeek VERSION ${TEXPEEK_VERSION} LANGUAGES C CXX)   # C: OpenEXR'nin çekirdeği C ile yazılmış

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# C++ çalışma kütüphanesini exe / dll'in içine göm (/MT). Böylece program başka bir bilgisayarda
# "Visual C++ Redistributable" kurmadan çalışır. Kütüphanelerden önce durmalı ki onlara da uygulansın.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# ---------------------------------------------------------------------------
# Dış kütüphaneler: ilk derlemede GitHub'dan otomatik indirilir (git gerekir).
# ---------------------------------------------------------------------------

# DirectXTex: DDS / TGA / HDR / PNG okuma, ileride dönüştürme (MIT)
set(BUILD_TOOLS  OFF CACHE BOOL "" FORCE)   # texconv vb. araçlar lazım değil
set(BUILD_SAMPLE OFF CACHE BOOL "" FORCE)   # örnek uygulama lazım değil
set(BUILD_DX12   OFF CACHE BOOL "" FORCE)   # biz DirectX 11 kullanıyoruz
FetchContent_Declare(
    DirectXTex
    GIT_REPOSITORY https://github.com/microsoft/DirectXTex.git
    GIT_TAG        may2026
    GIT_SHALLOW    TRUE
)

# Dear ImGui: arayüz (MIT)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9b
    GIT_SHALLOW    TRUE
)

# OpenEXR: EXR okuma (BSD-3). EXR formatının resmi kütüphanesi; DWAA / DWAB dahil bütün sıkıştırma
# türlerini okur (Poly Haven'ın EXR'leri DWAA'dır; daha önce kullanılan tinyexr bunu desteklemiyordu).
# Yalnızca C ile yazılmış çekirdeğini (OpenEXRCore) kullanıyoruz. SOURCE_SUBDIR ile burada yalnızca
# kaynağı indirtip aşağıda EXCLUDE_FROM_ALL ile ekliyoruz: böylece kütüphanenin kullanmadığımız
# C++ kısımları hiç derlenmez. Kendi bağımlılıklarını (Imath, libdeflate) OpenEXR kendisi indirir.
FetchContent_Declare(
    openexr
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG        v3.3.14
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake-kullanilmiyor
)

FetchContent_MakeAvailable(DirectXTex imgui openexr)

set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)   # yukarıdaki /MT ayarı OpenEXR ve bağımlılıklarına da uygulansın
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(OPENEXR_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_TOOLS OFF CACHE BOOL "" FORCE)
set(OPENEXR_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(OPENEXR_FORCE_INTERNAL_IMATH ON CACHE BOOL "" FORCE)     # bilgisayarda kurulu bir kopya aranmasın
set(OPENEXR_FORCE_INTERNAL_DEFLATE ON CACHE BOOL "" FORCE)
add_subdirectory(${openexr_SOURCE_DIR} ${openexr_BINARY_DIR} EXCLUDE_FROM_ALL)

# ImGui'nin kendi CMake dosyası yok, kaynaklarını biz ekliyoruz.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

# ---------------------------------------------------------------------------
# TexPeek
# ---------------------------------------------------------------------------

# Simge + sürüm bilgisi: res/version.rc.in şablonundan exe ve dll için birer .rc dosyası üretilir.
set(RC_FILENAME    "texpeek.exe")
set(RC_DESCRIPTION "TexPeek texture viewer")
set(RC_FILETYPE    "VFT_APP")
set(RC_ICON_LINE   "1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/res/texpeek.ico\"")
configure_file(res/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/texpeek.rc" @ONLY)

set(RC_FILENAME    "TexPeekThumb.dll")
set(RC_DESCRIPTION "TexPeek DDS thumbnail provider")
set(RC_FILETYPE    "VFT_DLL")
set(RC_ICON_LINE   "")
configure_file(res/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/texpeek_thumb.rc" @ONLY)

add_executable(texpeek WIN32
    src/main.cpp
    src/TextureDoc.cpp
    src/Viewer.cpp
    src/FormatNames.cpp
    src/Lang.cpp
    src/ShellIntegration.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/texpeek.rc"
)

# Gezgin için DDS küçük resim eklentisi. texpeek.exe'nin yanına TexPeekThumb.dll olarak çıkar;
# Windows'a kaydı programın içindeki "Windows > Kur" düğmesi yapar.
add_library(texpeek_thumb SHARED
    src/thumb/ThumbProvider.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/texpeek_thumb.rc"
)
set_target_properties(texpeek_thumb PROPERTIES OUTPUT_NAME "TexPeekThumb")
target_link_libraries(texpeek_thumb PRIVATE DirectXTex ole32)
target_compile_definitions(texpeek_thumb PRIVATE UNICODE _UNICODE NOMINMAX)
add_dependencies(texpeek texpeek_thumb)   # exe derlenince DLL de derlensin

target_link_libraries(texpeek PRIVATE
    DirectXTex
    imgui
    OpenEXR::OpenEXRCore
    d3d11 dxgi
    d3dcompiler # görüntü shader'ını çalışırken derlemek için
    shell32     # sürükle-bırak
    comdlg32    # "Dosya Aç" penceresi
    ole32       # COM (PNG/JPG okuma için gerekli)
)

target_compile_definitions(texpeek PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

if(MSVC)
    # Kaynak dosyalar UTF-8: Türkçe karakterler doğru görünsün
    target_compile_options(texpeek PRIVATE /utf-8 /W4)
    target_compile_options(imgui PRIVATE /utf-8)
    target_compile_options(texpeek_thumb PRIVATE /utf-8 /W4)
endif()
