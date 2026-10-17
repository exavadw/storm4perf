@echo off
rem build.cmd: builds storm4perf -> xinput9_1_0.dll (+ .pdb). Needs the MSVC x64 Build Tools; set VCVARS if they live elsewhere.
rem storm4perf.cpp builds as before; the replays (replay\, C++20) and the MinHook they use (replay\minhook) are compiled
rem separately and linked into the same DLL.
if "%VCVARS%"=="" set VCVARS=C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul || (echo vcvars64.bat not found: set VCVARS & exit /b 1)
cd /d %~dp0
if not exist obj mkdir obj
del /q obj\*.obj 2>nul
(
  cl /nologo /c /O2 /MT /EHsc /W4 /wd4100 /Zi /Fdobj\ /Foobj\ storm4perf.cpp ^
  && cl /nologo /c /O2 /MT /EHsc /std:c++20 /permissive- /utf-8 /W4 /wd4100 /Zi /Fdobj\ /Foobj\ ^
       /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /Ireplay /Ireplay\minhook\include ^
       replay\*.cpp ^
  && cl /nologo /c /O2 /MT /W3 /Zi /Fdobj\ /Foobj\ replay\minhook\src\*.c replay\minhook\src\hde\hde64.c ^
  && link /NOLOGO /DLL /DEBUG /OPT:REF /OPT:ICF /OUT:xinput9_1_0.dll obj\*.obj d3d11.lib
) >build.log 2>&1 || (type build.log & exit /b 1)
type build.log | findstr /i "warning error"
echo BUILD OK
