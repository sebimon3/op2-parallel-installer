@echo off
setlocal
cd /d "%~dp0"

rem --- locate the MSVC x64 build environment ------------------------------
set "VCVARS="
if defined OP22_VCVARS set "VCVARS=%OP22_VCVARS%"
if not defined VCVARS call :tryvc "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
if not defined VCVARS call :tryvc "C:\Program Files\Microsoft Visual Studio\18\BuildTools"
if not defined VCVARS call :tryvc "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
if not defined VCVARS call :tryvc "C:\Program Files\Microsoft Visual Studio\2022\Community"
if not defined VCVARS call :tryvc "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
if not defined VCVARS (
  echo Cannot find vcvarsall.bat. Install Visual Studio Build Tools with the
  echo Desktop development with C++ workload, or set OP22_VCVARS to its path.
  exit /b 1
)
goto :havevc

:tryvc
if exist "%~1\VC\Auxiliary\Build\vcvarsall.bat" set "VCVARS=%~1\VC\Auxiliary\Build\vcvarsall.bat"
exit /b 0

:havevc
call "%VCVARS%" x64
if errorlevel 1 (
  echo vcvarsall.bat failed: "%VCVARS%"
  exit /b 1
)

rem --- generate the application manifest used by resource.rc -------------
> _op22.manifest echo ^<?xml version="1.0" encoding="UTF-8" standalone="yes"?^>
>>_op22.manifest echo ^<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0"^>
>>_op22.manifest echo   ^<assemblyIdentity version="1.0.0.0" processorArchitecture="amd64" name="OP22.Native" type="win32"/^>
>>_op22.manifest echo   ^<trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"^>
>>_op22.manifest echo     ^<security^>
>>_op22.manifest echo       ^<requestedPrivileges^>
>>_op22.manifest echo         ^<requestedExecutionLevel level="asInvoker" uiAccess="false"/^>
>>_op22.manifest echo       ^</requestedPrivileges^>
>>_op22.manifest echo     ^</security^>
>>_op22.manifest echo   ^</trustInfo^>
>>_op22.manifest echo   ^<compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"^>
>>_op22.manifest echo     ^<application^>
>>_op22.manifest echo       ^<supportedOS Id="{e2011457-1546-43c5-a5fe-008deee3d3f0}"/^>
>>_op22.manifest echo       ^<supportedOS Id="{35138b9a-5d96-4fbd-8e2d-a2440225f93a}"/^>
>>_op22.manifest echo       ^<supportedOS Id="{4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38}"/^>
>>_op22.manifest echo       ^<supportedOS Id="{1f676c76-80e1-4239-95bb-83d0f6d0da78}"/^>
>>_op22.manifest echo       ^<supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/^>
>>_op22.manifest echo     ^</application^>
>>_op22.manifest echo   ^</compatibility^>
>>_op22.manifest echo   ^<application xmlns="urn:schemas-microsoft-com:asm.v3"^>
>>_op22.manifest echo     ^<windowsSettings^>
>>_op22.manifest echo       ^<dpiAwareness xmlns="http://schemas.microsoft.com/SMI/2016/WindowsSettings"^>permonitorv2^</dpiAwareness^>
>>_op22.manifest echo     ^</windowsSettings^>
>>_op22.manifest echo   ^</application^>
>>_op22.manifest echo   ^<dependency^>
>>_op22.manifest echo     ^<dependentAssembly^>
>>_op22.manifest echo       ^<assemblyIdentity type="win32" name="Microsoft.Windows.Common-Controls" version="6.0.0.0" processorArchitecture="*" publicKeyToken="6595b64144ccf1df" language="*"/^>
>>_op22.manifest echo     ^</dependentAssembly^>
>>_op22.manifest echo   ^</dependency^>
>>_op22.manifest echo ^</assembly^>

rem --- the bundled LZMA source expects a 7-Zip Precomp.h; supply a minimal
rem --- one only when the SDK copy is absent, and remove it afterwards.
set PRECOMP_CREATED=
if not exist lzma\Precomp.h (
  echo #include "7zTypes.h" > lzma\Precomp.h
  set PRECOMP_CREATED=1
)

rem --- resources ---------------------------------------------------------
rc /nologo /fo _op22.res resource.rc
if errorlevel 1 goto fail

rem --- compile & link ----------------------------------------------------
if not exist dist mkdir dist
cl /nologo /O2 /MT /W3 /utf-8 /Fe:dist\op-2.2-mt.exe ^
   main.c ui.c core.c lzma\LzmaDec.c _op22.res ^
   /link comctl32.lib shell32.lib ole32.lib Shlwapi.lib user32.lib gdi32.lib advapi32.lib uxtheme.lib
if errorlevel 1 goto fail

del /q _op22.manifest _op22.res >nul 2>&1
if defined PRECOMP_CREATED del /q lzma\Precomp.h >nul 2>&1
echo Built dist\op-2.2-mt.exe
exit /b 0

:fail
echo BUILD FAILED
del /q _op22.manifest _op22.res >nul 2>&1
if defined PRECOMP_CREATED del /q lzma\Precomp.h >nul 2>&1
exit /b 1
