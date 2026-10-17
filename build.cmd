@echo off
rem build.cmd: builds storm4perf -> xinput9_1_0.dll (+ .pdb). Needs the MSVC x64 Build Tools; set VCVARS if they live elsewhere.
if "%VCVARS%"=="" set VCVARS=C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul || (echo vcvars64.bat not found: set VCVARS & exit /b 1)
cd /d %~dp0
cl /nologo /O2 /LD /MT /EHsc /W4 /wd4100 /Zi storm4perf.cpp /Fe:xinput9_1_0.dll /link /NOLOGO /DEBUG /OPT:REF /OPT:ICF d3d11.lib >build.log 2>&1 || (type build.log & exit /b 1)
type build.log | findstr /i "warning error"
echo BUILD OK
