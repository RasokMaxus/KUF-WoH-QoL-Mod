@echo off
rem Builds dinput8.dll (32-bit, static runtime: needs no Visual C++ redistributable).
rem Requires Visual Studio with the "Desktop development with C++" workload.
setlocal
cd /d "%~dp0"
set VCVARS=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat
if not defined VCVARS set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat
call "%VCVARS%" >nul || exit /b 1
cl /nologo /O2 /MT /W4 /EHsc /LD kufmod.cpp economy.cpp icons.cpp lan.cpp nettrace.cpp proxy.cpp /link /DEF:dinput8.def /OUT:dinput8.dll user32.lib gdi32.lib ws2_32.lib iphlpapi.lib || exit /b 1
del /q *.obj *.exp *.lib 2>nul
echo Built %~dp0dinput8.dll
