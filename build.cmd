@echo off
setlocal
rem Unified optimized Aulay with diagnostic recording.
pushd "%~dp0"
where msbuild >nul 2>nul
if errorlevel 1 (
  echo Run this script from a Visual Studio developer command prompt.
  popd
  exit /b 1
)
if not exist "packages\Microsoft.Windows.ImplementationLibrary.1.0.200519.2\include\wil\common.h" (
  where nuget >nul 2>nul
  if errorlevel 1 (
    echo Restore dependencies first: nuget restore Aulay.sln
    popd
    exit /b 1
  )
  nuget restore Aulay.sln
  if errorlevel 1 (
    popd
    exit /b 1
  )
)
msbuild Aulay.sln /t:Build /m /p:Configuration=Release /p:Platform=x64
set "BUILD_RESULT=%ERRORLEVEL%"
if "%BUILD_RESULT%"=="0" echo Aulay EXE: %CD%\x64\Release\Aulay64.exe
popd
exit /b %BUILD_RESULT%
