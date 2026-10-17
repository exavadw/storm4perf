@echo off
rem build-release.cmd: storm4perf only (no test harness). Output: xinput9_1_0.dll + xinput9_1_0.pdb
call C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat >nul
cd /d %~dp0
cl /nologo /O2 /LD /MT /EHsc /W4 /wd4100 /Zi storm4perf.cpp /Fe:xinput9_1_0.dll /link /NOLOGO /DEBUG /OPT:REF /OPT:ICF d3d11.lib >build.log 2>&1 || (type build.log & exit /b 1)
type build.log | findstr /i "warning error"
echo BUILD OK
