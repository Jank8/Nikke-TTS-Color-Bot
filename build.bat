@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

REM Compile resource file
rc.exe color_bot.rc

REM Compile program with icon
cl.exe color_bot.cpp ^
    /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS ^
    /Fe:color_bot.exe ^
    d3d11.lib dxgi.lib winmm.lib user32.lib shell32.lib advapi32.lib ^
    /link /SUBSYSTEM:CONSOLE color_bot.res

if %ERRORLEVEL% == 0 (
    echo.
    echo === Nikke TTS Color Bot - Compiled with icon ===
) else (
    echo.
    echo === Build FAILED ===
)
