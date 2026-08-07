@echo off
setlocal EnableExtensions

cd /D "%~dp0"

set "BUILD_DIR=out\build\x64-Release"
set "BOOST_DIR=%~dp0third_party\boost_1_84_0"

rem --- Locate Visual Studio (any edition / year) via vswhere ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo [ERROR] No se encontro vswhere.exe. Instala Visual Studio con C++.
	exit /b 1
)

set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
	echo [ERROR] No se encontro una instalacion de Visual Studio con herramientas C++ x64.
	exit /b 1
)

set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
	echo [ERROR] No se encontro VsDevCmd.bat en:
	echo         %VSDEVCMD%
	exit /b 1
)

if not exist "%BOOST_DIR%\boost" (
	echo [ERROR] No se encontro Boost en:
	echo         %BOOST_DIR%
	exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

echo [1/3] Entorno VS: %VSINSTALL%
call "%VSDEVCMD%" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

where cl >nul 2>&1
if errorlevel 1 (
	echo [ERROR] cl.exe no esta en PATH tras VsDevCmd. Revisa la instalacion de "Desktop development with C++".
	exit /b 1
)

rem Prefer the CMake that ships with VS ^(avoids a random/broken cmake earlier in PATH^).
set "CMAKE_EXE="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find **/cmake.exe`) do (
	if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
)
if not defined CMAKE_EXE (
	where cmake >nul 2>&1 && for /f "delims=" %%i in ('where cmake') do (
		if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
	)
)
if not defined CMAKE_EXE (
	echo [ERROR] No se encontro cmake.exe ^(VS ni PATH^).
	exit /b 1
)

where ninja >nul 2>&1
if errorlevel 1 (
	echo [ERROR] No se encontro ninja.exe en PATH. Instala el componente "C++ CMake tools" de VS
	echo         o anade Ninja al PATH.
	exit /b 1
)

rem A failed earlier configure can leave CMAKE_C_COMPILER=C in the cache
rem ^(path with spaces split at C:^\^). Drop the cache so we reconfigure clean.
if exist "%BUILD_DIR%\CMakeCache.txt" del /f /q "%BUILD_DIR%\CMakeCache.txt" >nul 2>&1
if exist "%BUILD_DIR%\CMakeFiles" rmdir /s /q "%BUILD_DIR%\CMakeFiles" >nul 2>&1

echo [2/3] Configurando CMake...
echo       cmake: %CMAKE_EXE%
"%CMAKE_EXE%" -S . -B "%BUILD_DIR%" -G Ninja ^
	-DCMAKE_BUILD_TYPE=Release ^
	-DCMAKE_POLICY_VERSION_MINIMUM=3.5.0 ^
	-DCMAKE_C_COMPILER=cl ^
	-DCMAKE_CXX_COMPILER=cl ^
	-DARCHITECTURE=x86_64 ^
	-DBOOST_ROOT="%BOOST_DIR%" ^
	-DBoost_INCLUDE_DIR="%BOOST_DIR%" ^
	-DBoost_NO_SYSTEM_PATHS=ON
if errorlevel 1 exit /b 1

set "RES_OBJ=%BUILD_DIR%\pvz2native\CMakeFiles\pvz2native.dir\windows_resources.rc.res"
if exist "%RES_OBJ%" (
	echo       Forzando recompilacion de resources...
	del /f /q "%RES_OBJ%" >nul 2>&1
)

echo [3/3] Compilando...
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

echo.
echo Compilacion completada. Ejecutable:
echo   %~dp0%BUILD_DIR%\pvz2native\pvz2.exe

endlocal
