@echo off
REM Issue #13 co-version smoke. Build from repo root in a VS2022 x64 dev shell (or this calls vcvars).
REM   VFX = Maxine VideoFX SDK src   AR = Maxine AR SDK src   OF = Optical Flow SDK root
REM   CUDA = CUDA toolkit (for driver-API cuda.h + cuda.lib)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not defined VFX  set VFX=C:\actions-runner\_sdk\VideoFX
if not defined AR   set AR=C:\actions-runner\_sdk\Maxine-AR-SDK-1.1.1.0
if not defined OF   set OF=C:\actions-runner\_sdk\Optical_Flow_SDK_5.0.7\Optical_Flow_SDK_5.0.7
if not defined CUDA set CUDA=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
set OUT=%~1
if "%OUT%"=="" set OUT=native\shim\smoke\of_fruc_smoke.exe
cl /nologo /EHsc /std:c++17 /DCOS_HAS_MAXINE /DCOS_HAS_MAXINE_AR ^
  /I "%VFX%\nvvfx\include" /I "%VFX%\features\nvvfxgreenscreen\include" /I "%VFX%\features\nvvfxvideosuperres\include" ^
  /I "%AR%\nvar\include" /I "%OF%\NvOFFRUC\Interface" /I "%CUDA%\include" ^
  native\shim\smoke\of_fruc_smoke.cpp ^
  native\shim\aigs.cpp native\shim\eyecontact.cpp native\shim\superres.cpp ^
  native\shim\paths.cpp native\shim\vfx_paths.cpp ^
  "%VFX%\nvvfx\src\nvVideoEffectsProxy.cpp" "%VFX%\nvvfx\src\nvCVImageProxy.cpp" "%AR%\nvar\src\nvARProxy.cpp" ^
  Psapi.lib "%CUDA%\lib\x64\cuda.lib" ^
  /Fo"native\shim\smoke\\" /Fe"%OUT%"
exit /b %ERRORLEVEL%
