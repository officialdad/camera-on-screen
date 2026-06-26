@echo off
REM Task 3 FRUC interpolation smoke build.
REM Compiles fruc_interp_smoke.cpp + fruc.cpp + paths.cpp with /DCOS_HAS_FRUC.
REM Defaults to runner SDK paths; override via environment variables.
REM Run from the repo root in a plain cmd/PowerShell window -- this script calls vcvars64 itself.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not defined OF   set OF=C:\actions-runner\_sdk\Optical_Flow_SDK_5.0.7\Optical_Flow_SDK_5.0.7
if not defined CUDA set CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
set OUT=%~1
if "%OUT%"=="" set OUT=native\shim\smoke\fruc_interp_smoke.exe
cl /nologo /EHsc /std:c++17 /W4 /WX /DCOS_HAS_FRUC ^
  /I "%OF%\NvOFFRUC\Interface" /I "%CUDA%\include" ^
  native\shim\smoke\fruc_interp_smoke.cpp ^
  native\shim\fruc.cpp ^
  native\shim\paths.cpp ^
  Psapi.lib "%CUDA%\lib\x64\cuda.lib" ^
  /Fo"native\shim\smoke\\" /Fe"%OUT%"
exit /b %ERRORLEVEL%
