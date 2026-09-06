@echo off
setlocal
rem Run from a VS x64 developer prompt after building unified Release x64.
pushd "%~dp0.." || exit /b 1
rem Use the Windows SDK C++/WinRT headers selected by the VS developer shell.
set "TEST_OUTPUT=%TEMP%\Aulay-debug-audio-%RANDOM%-%RANDOM%"
mkdir "%TEST_OUTPUT%"
cl /nologo /std:c++20 /EHsc /MD /utf-8 /I"packages\Microsoft.Windows.ImplementationLibrary.1.0.200519.2\include" tests\windows_debug_audio.cpp /Fo:"%TEST_OUTPUT%\audio.obj" /Fe:"%TEST_OUTPUT%\audio.exe" WindowsApp.lib ole32.lib
if not "%ERRORLEVEL%"=="0" goto failed
"%TEST_OUTPUT%\audio.exe"
if not "%ERRORLEVEL%"=="0" goto failed
rmdir /s /q "%TEST_OUTPUT%"
popd
exit /b 0
:failed
rmdir /s /q "%TEST_OUTPUT%"
popd
exit /b 1
