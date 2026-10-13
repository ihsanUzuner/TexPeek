@echo off
rem Builds the two things that go on the GitHub Releases page:
rem   release\TexPeek-Setup-<ver>.exe      installer (needs Inno Setup)
rem   release\TexPeek-<ver>-portable.zip   portable version
rem The compiled program itself is left in release\TexPeek so it can be tried right away.
rem Uses its own build folder (build-release), so the everyday build in build\ is not touched.
setlocal
cd /d "%~dp0"

rem DirectXTex's shader step fails when this variable is set (some terminals set it)
set "NoDefaultCurrentDirectoryInExePath="

rem the version number lives only in the VERSION file
set "VER="
if exist VERSION set /p VER=<VERSION
if defined VER set "VER=%VER: =%"
if not defined VER (echo VERSION file is missing or empty. & goto fail)

echo.
echo === TexPeek %VER% release build ===
echo.

rem cmake: PATH first, then the copy that comes with Visual Studio / Build Tools
set "CMAKE="
where cmake >nul 2>&1 && set "CMAKE=cmake"
if defined CMAKE goto havecmake
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto nocmake
set "VSDIR="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%I"
if not defined VSDIR goto nocmake
set "CMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto nocmake
:havecmake

echo [1/4] Building...
"%CMAKE%" -S . -B build-release -A x64
if errorlevel 1 goto fail
"%CMAKE%" --build build-release --config Release
if errorlevel 1 goto fail

echo [2/4] Preparing the package folder...
if exist release rmdir /s /q release
mkdir release\TexPeek\licenses
copy /y build-release\Release\texpeek.exe release\TexPeek\ >nul
if errorlevel 1 goto fail
copy /y build-release\Release\TexPeekThumb.dll release\TexPeek\ >nul
if errorlevel 1 goto fail
copy /y LICENSE release\TexPeek\ >nul
copy /y THIRD-PARTY-NOTICES.txt release\TexPeek\ >nul
copy /y README.md release\TexPeek\ >nul
copy /y README-TR.md release\TexPeek\ >nul
copy /y licenses\*.txt release\TexPeek\licenses\ >nul
if errorlevel 1 goto fail

echo [3/4] Portable zip...
powershell -NoProfile -Command "Compress-Archive -Path 'release\TexPeek' -DestinationPath 'release\TexPeek-%VER%-portable.zip' -Force"
if errorlevel 1 goto fail

echo [4/4] Setup.exe...
set "ISCC="
where ISCC >nul 2>&1 && set "ISCC=ISCC"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC goto noiscc
"%ISCC%" /Qp /DAppVer=%VER% installer\TexPeek.iss
if errorlevel 1 goto fail
goto done

:noiscc
echo Inno Setup 6 not found. Install it with:  winget install JRSoftware.InnoSetup
rem On the build server a release without Setup.exe must not look like a success
if defined CI goto fail
echo The zip is ready, Setup.exe was skipped.

:done
echo.
echo DONE. Output folder: %~dp0release
dir /b release
if not defined CI pause
exit /b 0

:nocmake
echo cmake not found. Install "Build Tools for Visual Studio" with the C++ desktop workload,
echo or put cmake on the PATH.

:fail
echo.
echo An ERROR occurred. See the message above.
if not defined CI pause
exit /b 1
