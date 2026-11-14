@echo off
rem Builds everything with MinGW-w64 g++ (install MSYS2 or WinLibs, make sure g++ is on PATH).
rem Usage: build_windows.bat            -> builds the engine, the engine tests, the console simulator and its tests
setlocal
cd /d "%~dp0"
where g++ >nul 2>nul || (echo g++ was not found on PATH. See README, "Windows setup". & exit /b 1)
if not exist build mkdir build
set FLAGS=-std=c++17 -Wall -Wextra -pedantic -O2 -Iinclude -pthread
set LIB=src\banker.cpp src\cpu_scheduler.cpp src\engine_api.cpp src\job.cpp src\json.cpp src\memory_manager.cpp src\ranking.cpp src\report.cpp src\resource_manager.cpp src\scheduler.cpp src\server.cpp src\simulation.cpp src\sync_demo.cpp src\workload.cpp

echo [1/4] build\mcrm_engine.exe
g++ %FLAGS% %LIB% src\engine_main.cpp -o build\mcrm_engine.exe || exit /b 1
echo [2/4] build\engine_tests.exe
g++ %FLAGS% -Itests\engine %LIB% tests\engine\test_cpu_scheduler.cpp tests\engine\test_modules.cpp tests\engine\test_runner.cpp -o build\engine_tests.exe || exit /b 1
echo [3/4] build\mcrm.exe (console simulator)
g++ %FLAGS% %LIB% src\main.cpp -o build\mcrm.exe || exit /b 1
echo [4/4] build\tests.exe
g++ %FLAGS% %LIB% tests\test_main.cpp -o build\tests.exe || exit /b 1
echo.
echo Done. Run the C++ tests with:  build\engine_tests.exe   and   build\tests.exe
