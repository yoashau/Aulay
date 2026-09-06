@echo off
setlocal
rem Run from a Visual Studio developer command environment after building x64.
pushd "%~dp0.." || exit /b 1
rem Use the Windows SDK C++/WinRT headers selected by the VS developer shell.
set "TEST_OUTPUT=%TEMP%\Aulay-native-tests-%RANDOM%-%RANDOM%"
mkdir "%TEST_OUTPUT%"
cl /nologo /std:c++20 /EHsc /MD /utf-8 /I"packages\Microsoft.Windows.ImplementationLibrary.1.0.200519.2\include" tests\windows_hardening.cpp /Fo:"%TEST_OUTPUT%\windows_hardening.obj" /Fe:"%TEST_OUTPUT%\windows_hardening.exe" WindowsApp.lib comctl32.lib shell32.lib advapi32.lib user32.lib
if not "%ERRORLEVEL%"=="0" goto failed
"%TEST_OUTPUT%\windows_hardening.exe" %*
if not "%ERRORLEVEL%"=="0" goto failed
python tests\generate_recovery_test.py "%TEST_OUTPUT%\recovery-test.cpp"
if not "%ERRORLEVEL%"=="0" goto failed
cl /nologo /std:c++20 /EHsc /MD /utf-8 /I. /I"packages\Microsoft.Windows.ImplementationLibrary.1.0.200519.2\include" "%TEST_OUTPUT%\recovery-test.cpp" /Fo:"%TEST_OUTPUT%\recovery-test.obj" /Fe:"%TEST_OUTPUT%\recovery-test.exe" WindowsApp.lib
if not "%ERRORLEVEL%"=="0" goto failed
"%TEST_OUTPUT%\recovery-test.exe"
if not "%ERRORLEVEL%"=="0" goto failed
rmdir /s /q "%TEST_OUTPUT%"
popd
exit /b 0
:failed
rmdir /s /q "%TEST_OUTPUT%"
popd
exit /b 1
