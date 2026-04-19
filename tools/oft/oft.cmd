@echo off
setlocal
set "OFT_VERSION=4.2.2"
set "SCRIPT_DIR=%~dp0"
set "JAR_PATH=%SCRIPT_DIR%openfasttrace-%OFT_VERSION%.jar"

if not exist "%JAR_PATH%" (
  echo OpenFastTrace JAR not found at %JAR_PATH% 1>&2
  echo Run %SCRIPT_DIR%install.sh first. 1>&2
  exit /b 1
)

where java >nul 2>nul
if %ERRORLEVEL% equ 0 (
  java -jar "%JAR_PATH%" %*
  exit /b %ERRORLEVEL%
)

for /d %%D in ("C:\Program Files\Microsoft\jdk-*-hotspot") do (
  if exist "%%D\bin\java.exe" (
    "%%D\bin\java.exe" -jar "%JAR_PATH%" %*
    exit /b %ERRORLEVEL%
  )
)

echo Error: java not found on PATH or at known JDK install location. 1>&2
exit /b 1
