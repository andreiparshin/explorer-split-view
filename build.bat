@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "VCVARS="
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
)

if "%VCVARS%"=="" (
    echo [ERROR] Could not locate Visual Studio vcvars64.bat
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to initialize MSVC x64 environment
    exit /b 1
)

if not exist "%ROOT%\bin" mkdir "%ROOT%\bin"
if not exist "%ROOT%\build_obj" mkdir "%ROOT%\build_obj"

REM Clean unlocked old backup binaries, and safely move aside any currently loaded DLL/EXE
del /Q "%ROOT%\build_obj\*.old_*" >nul 2>&1
if exist "%ROOT%\bin\ExplorerSplitHook.dll" (
    move /Y "%ROOT%\bin\ExplorerSplitHook.dll" "%ROOT%\build_obj\ExplorerSplitHook.old_%RANDOM%.dll" >nul 2>&1
)
if exist "%ROOT%\bin\ExplorerSplitController.exe" (
    move /Y "%ROOT%\bin\ExplorerSplitController.exe" "%ROOT%\build_obj\ExplorerSplitController.old_%RANDOM%.exe" >nul 2>&1
)

echo [1/2] Compiling ExplorerSplitHook.dll ...
cl.exe /nologo /std:c++20 /utf-8 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE ^
    /I"%ROOT%\include" ^
    /Fo"%ROOT%\build_obj\\" ^
    /LD ^
    "%ROOT%\src\HookDll\DllMain.cpp" ^
    "%ROOT%\src\HookDll\TabSplitManager.cpp" ^
    "%ROOT%\src\HookDll\RightPaneView.cpp" ^
    "%ROOT%\src\HookDll\ShellHelpers.cpp" ^
    /link /DEF:"%ROOT%\src\HookDll\HookDll.def" ^
    /OUT:"%ROOT%\bin\ExplorerSplitHook.dll" ^
    /IMPLIB:"%ROOT%\build_obj\ExplorerSplitHook.lib" ^
    user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib shlwapi.lib comctl32.lib uxtheme.lib dwmapi.lib advapi32.lib
if errorlevel 1 (
    echo [ERROR] Failed to build ExplorerSplitHook.dll
    exit /b 1
)

echo [2/2] Compiling ExplorerSplitController.exe ...
cl.exe /nologo /std:c++20 /utf-8 /O2 /EHsc /W4 /MT /DUNICODE /D_UNICODE ^
    /I"%ROOT%\include" ^
    /Fo"%ROOT%\build_obj\\" ^
    "%ROOT%\src\Controller\ControllerMain.cpp" ^
    /link /SUBSYSTEM:WINDOWS ^
    /OUT:"%ROOT%\bin\ExplorerSplitController.exe" ^
    user32.lib gdi32.lib shell32.lib ole32.lib advapi32.lib dwmapi.lib uxtheme.lib
if errorlevel 1 (
    echo [ERROR] Failed to build ExplorerSplitController.exe
    exit /b 1
)

echo [SUCCESS] Built bin\ExplorerSplitHook.dll and bin\ExplorerSplitController.exe
exit /b 0
