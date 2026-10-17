
@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
 echo ERROR: VS2022 BuildTools not found
 exit /b 1
)
cd /d C:\dev\agent-bugfix\src
cl /nologo /O2 /LD /EHsc /DUNICODE /D_UNICODE storm4perf.cpp /Fe..\game\xinput9_1_0.dll /link user32.lib kernel32.lib
if errorlevel 1 (
 echo BUILD FAILED
 exit /b 1
)
echo BUILD OK: ..\game\xinput9_1_0.dll
