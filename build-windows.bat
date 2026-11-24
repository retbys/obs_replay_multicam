@echo off
rem Сборка плагина под Windows одним кликом.
rem Нужны: Visual Studio 2022 (с компонентом "Разработка классических приложений на C++"),
rem CMake 3.28+ и доступ в интернет (первый раз скачиваются заголовки и библиотеки OBS).
setlocal
cd /d "%~dp0"

cmake --preset windows-x64
if errorlevel 1 goto :fail

cmake --build --preset windows-x64 --config RelWithDebInfo
if errorlevel 1 goto :fail

cmake --install build_x64 --config RelWithDebInfo --prefix "%~dp0release"
if errorlevel 1 goto :fail

echo.
echo ===== ГОТОВО =====
echo Папку "release\obs-multicam-replay" скопируйте целиком в
echo %APPDATA%\obs-studio\plugins\  (OBS должен быть закрыт).
pause
exit /b 0

:fail
echo.
echo ===== ОШИБКА СБОРКИ. Скопируйте текст выше и пришлите его. =====
pause
exit /b 1
