@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
set VFX=C:\dev\VideoFX
set AR=C:\dev\Maxine-AR-SDK
set OUT=%~1
cl /nologo /EHsc /std:c++17 /DCOS_HAS_MAXINE /DCOS_HAS_MAXINE_AR /I "%VFX%\nvvfx\include" /I "%VFX%\features\nvvfxgreenscreen\include" /I "%AR%\nvar\include" native\shim\smoke\bundle_probe.cpp native\shim\aigs.cpp native\shim\eyecontact.cpp native\shim\paths.cpp "%VFX%\nvvfx\src\nvVideoEffectsProxy.cpp" "%VFX%\nvvfx\src\nvCVImageProxy.cpp" "%AR%\nvar\src\nvARProxy.cpp" /Fo"native\shim\smoke\\" /Fe"%OUT%"
exit /b %ERRORLEVEL%
