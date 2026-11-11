@echo off
rem Fase 49: compila o launcher e o instalador (clang-cl ou cl.exe, runtime estatico: nao dependem de DLL nenhuma).
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo ERRO: Visual Studio nao encontrado
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%VSROOT%\VC\Tools\Llvm\x64\bin;%PATH%"
set "OUT=%~dp0build"
if not exist "%OUT%" mkdir "%OUT%"
set FLAGS=/nologo /std:c++20 /O2 /MT /EHsc /W3 /DNDEBUG /utf-8
rc /nologo /fo "%OUT%\app.res" "%~dp0app.rc" || exit /b 1
where clang-cl >nul 2>nul
if %ERRORLEVEL% equ 0 (
    set "CC=clang-cl"
) else (
    set "CC=cl.exe"
)
%CC% %FLAGS% "%~dp0launcher.cpp" /Fo"%OUT%\launcher.obj" /Fe"%OUT%\LegoDimensionsLauncher.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "%OUT%\app.res" || exit /b 1
%CC% %FLAGS% "%~dp0transfer.cpp" /Fo"%OUT%\transfer.obj" /Fe"%OUT%\LD_PC_Transfer.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "%OUT%\app.res" || exit /b 1
%CC% %FLAGS% "%~dp0installer.cpp" /Fo"%OUT%\installer.obj" /Fe"%OUT%\installer_stub.exe" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED "%OUT%\app.res" || exit /b 1
echo OK: %OUT%\LegoDimensionsLauncher.exe, LD_PC_Transfer.exe e installer_stub.exe
