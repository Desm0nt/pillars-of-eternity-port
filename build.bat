@echo off
rem Pillars of Eternity Mobile (org.pillars.mobile): build the native libraries and the signed APK
rem (tools\build_native.py, then tools\build_apk.py; tool locations and the signing key: docs\BUILD.md).
rem   build.bat [Release|Development]      -> dist\PillarsOfEternity-Mobile-v<version>-arm64.apk
setlocal
chcp 65001 >nul
set "PYTHONUTF8=1"
set "HERE=%~dp0"
set "PY=%HERE%tools\.runtime\venv\Scripts\python.exe"
if not exist "%PY%" set "PY=%HERE%tools\.runtime\python\python.exe"
if not exist "%PY%" set "PY=python"
"%PY%" "%HERE%tools\build_native.py" all
if errorlevel 1 exit /b 1
"%PY%" "%HERE%tools\build_apk.py" %*
exit /b %ERRORLEVEL%
