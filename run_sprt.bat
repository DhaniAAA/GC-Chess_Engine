@echo off
setlocal enabledelayedexpansion
REM ============================================================================
REM SPRT Testing Script for GC-Chess Engine (Stockfish-style)
REM Uses cutechess-cli for tournament management
REM ============================================================================

title GC-Chess Engine SPRT Testing

echo.
echo ================================================
echo   GC-Chess Engine SPRT Testing Script v2.0
echo   (Stockfish-style Testing Framework)
echo ================================================
echo.

REM Create output directories
if not exist "tests" mkdir tests
if not exist "tests\results" mkdir tests\results

REM ============================================================================
REM CONFIGURATION - Edit these values as needed
REM ============================================================================
set CUTECHESS_CLI=cutechess\cutechess-cli.exe
set ENGINE_DEV=output\main.exe
set ENGINE_BASE=output\main-old.exe
set HASH=128
set THREADS=1
set CONCURRENCY=4

REM ============================================================================
REM MAIN MENU
REM ============================================================================
:menu
echo.
echo Select Test Type:
echo ===============================================
echo   [1] STC - Short Time Control (10+0.1)
echo       Elo bounds: [0, 10] - Quick test
echo.
echo   [2] LTC - Long Time Control (60+0.6)
echo       Elo bounds: [0, 5] - Standard test
echo.
echo   [3] VLTC - Very Long Time Control (180+1.8)
echo       Elo bounds: [0, 3] - Precise test
echo.
echo   [4] Regression Test (Non-regression)
echo       Elo bounds: [-3, 1] - Ensure no regression
echo.
echo   [5] Quick Strength Test (100 games)
echo       No SPRT, just raw Elo estimation
echo.
echo   [6] Custom Settings
echo.
echo   [0] Exit
echo ===============================================
echo.

set /p CHOICE="Enter your choice (0-6): "

if "%CHOICE%"=="1" goto stc_test
if "%CHOICE%"=="2" goto ltc_test
if "%CHOICE%"=="3" goto vltc_test
if "%CHOICE%"=="4" goto regression_test
if "%CHOICE%"=="5" goto quick_test
if "%CHOICE%"=="6" goto custom_test
if "%CHOICE%"=="0" goto end
echo Invalid choice. Please try again.
goto menu

REM ============================================================================
REM TEST CONFIGURATIONS
REM ============================================================================
:stc_test
echo.
echo Setting up STC Test...
set TC=10+0.1
set ELO0=0
set ELO1=10
set ALPHA=0.05
set BETA=0.05
set GAMES=1000
set TEST_NAME=STC
set USE_SPRT=1
goto pre_check

:ltc_test
echo.
echo Setting up LTC Test...
set TC=60+0.6
set ELO0=0
set ELO1=5
set ALPHA=0.05
set BETA=0.05
set GAMES=10000
set TEST_NAME=LTC
set USE_SPRT=1
goto pre_check

:vltc_test
echo.
echo Setting up VLTC Test...
set TC=180+1.8
set ELO0=0
set ELO1=3
set ALPHA=0.05
set BETA=0.05
set GAMES=20000
set TEST_NAME=VLTC
set USE_SPRT=1
goto pre_check

:regression_test
echo.
echo Setting up Regression Test...
set TC=10+0.1
set ELO0=-3
set ELO1=1
set ALPHA=0.05
set BETA=0.05
set GAMES=10000
set TEST_NAME=REGRESSION
set USE_SPRT=1
goto pre_check

:quick_test
echo.
echo Setting up Quick Strength Test...
set TC=5+0.05
set GAMES=100
set TEST_NAME=QUICK
set USE_SPRT=0
goto pre_check

:custom_test
echo.
echo Custom Test Configuration
echo ========================
set /p TC="Time Control (e.g., 10+0.1): "
set /p ELO0="Elo0 H0 bound (default 0): "
set /p ELO1="Elo1 H1 bound (default 10): "
set /p GAMES="Max games (default 5000): "
if "%ELO0%"=="" set ELO0=0
if "%ELO1%"=="" set ELO1=10
if "%GAMES%"=="" set GAMES=5000
set ALPHA=0.05
set BETA=0.05
set TEST_NAME=CUSTOM
set USE_SPRT=1
goto pre_check

REM ============================================================================
REM PRE-CHECK
REM ============================================================================
:pre_check
echo.
echo ================================================
echo   Pre-Test Checks
echo ================================================

REM Check cutechess-cli
if not exist "%CUTECHESS_CLI%" (
    echo [ERROR] cutechess-cli not found at: %CUTECHESS_CLI%
    echo Please download from: https://github.com/cutechess/cutechess/releases
    goto error
)
echo [OK] cutechess-cli found

REM Check DEV engine
if not exist "%ENGINE_DEV%" (
    echo [ERROR] DEV engine not found: %ENGINE_DEV%
    echo Please build the engine first.
    goto error
)
echo [OK] DEV engine found: %ENGINE_DEV%

REM Check BASE engine
if not exist "%ENGINE_BASE%" (
    echo [WARNING] BASE engine not found: %ENGINE_BASE%
    echo.
    set /p CREATE_BASE="Create baseline from DEV engine? (Y/n): "
    if /i "!CREATE_BASE!"=="n" goto error
    copy /Y "%ENGINE_DEV%" "%ENGINE_BASE%" >nul
    if errorlevel 1 (
        echo [ERROR] Failed to create baseline copy
        goto error
    )
    echo [OK] Baseline created at %ENGINE_BASE%
    echo.
    echo IMPORTANT: Make your code changes and rebuild DEV engine
    echo before running this test again!
    pause
    goto end
)
echo [OK] BASE engine found: %ENGINE_BASE%

REM ============================================================================
REM DISPLAY CONFIGURATION
REM ============================================================================
echo.
echo ================================================
echo   Test Configuration: %TEST_NAME%
echo ================================================
echo   DEV Engine:    %ENGINE_DEV%
echo   BASE Engine:   %ENGINE_BASE%
echo   Hash:          %HASH% MB
echo   Threads:       %THREADS%
echo   Concurrency:   %CONCURRENCY%
echo   Time Control:  %TC%
echo   Max Games:     %GAMES%
if "%USE_SPRT%"=="1" (
    echo   SPRT Bounds:   [%ELO0%, %ELO1%] Elo
    echo   Alpha:         %ALPHA%
    echo   Beta:          %BETA%
)
echo ================================================
echo.

set /p START_TEST="Start test? (Y/n): "
if /i "%START_TEST%"=="n" goto menu

REM ============================================================================
REM RUN TEST
REM ============================================================================
:run_test
echo.
echo Starting %TEST_NAME% test at %date% %time%
echo.

REM Set output file
set OUTPUT_PGN=tests\results\sprt_%TEST_NAME%.pgn

if "%USE_SPRT%"=="0" (
    "%CUTECHESS_CLI%" ^
        -engine name="DEV" cmd="%ENGINE_DEV%" option.Hash=%HASH% option.Threads=%THREADS% ^
        -engine name="BASE" cmd="%ENGINE_BASE%" option.Hash=%HASH% option.Threads=%THREADS% ^
        -each proto=uci tc=%TC% ^
        -games %GAMES% ^
        -repeat ^
        -recover ^
        -draw movenumber=40 movecount=8 score=10 ^
        -resign movecount=5 score=1000 ^
        -pgnout "%OUTPUT_PGN%" ^
        -concurrency %CONCURRENCY%
) else (
    "%CUTECHESS_CLI%" ^
        -engine name="DEV" cmd="%ENGINE_DEV%" option.Hash=%HASH% option.Threads=%THREADS% ^
        -engine name="BASE" cmd="%ENGINE_BASE%" option.Hash=%HASH% option.Threads=%THREADS% ^
        -each proto=uci tc=%TC% ^
        -sprt elo0=%ELO0% elo1=%ELO1% alpha=%ALPHA% beta=%BETA% ^
        -games %GAMES% ^
        -repeat ^
        -recover ^
        -draw movenumber=40 movecount=8 score=10 ^
        -resign movecount=3 score=800 ^
        -pgnout "%OUTPUT_PGN%" ^
        -concurrency %CONCURRENCY%
)

echo.
echo ================================================
echo   Test Completed!
echo   Results saved to: %OUTPUT_PGN%
echo ================================================

REM Run Elo Calculator
python tests\elo_calculator.py "%OUTPUT_PGN%" 2>nul
if errorlevel 1 (
    echo.
    echo [Note] Python not found or calculator error.
    echo To use Elo calculator, ensure Python is installed.
    echo You can run manually: python tests\elo_calculator.py "%OUTPUT_PGN%"
)

set /p ANOTHER="Run another test? (y/N): "
if /i "%ANOTHER%"=="y" goto menu
goto end

REM ============================================================================
REM ERROR HANDLING
REM ============================================================================
:error
echo.
echo [ERROR] Script terminated with errors.
echo.
pause
exit /b 1

REM ============================================================================
REM END
REM ============================================================================
:end
echo.
echo Thank you for using GC-Chess Engine SPRT Testing!
echo.
pause
endlocal
exit /b 0
