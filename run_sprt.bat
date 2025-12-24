@echo off
REM ============================================================================
REM SPRT Testing Script for Chess Engine
REM Uses cutechess-cli for tournament management
REM ============================================================================

echo.
echo ============================================
echo  Chess Engine SPRT Testing Script
echo ============================================
echo.

REM Create output directory if it doesn't exist
if not exist "tests" mkdir tests

REM Configuration
set ENGINE_NEW=output\main.exe
set ENGINE_BASE=output\main_baseline.exe
set HASH=64
set THREADS=1
set CONCURRENCY=4
set GAMES=2000
set TC=10+0.1
set BOOK=tests\book.pgn
set OUTPUT_PGN=tests\sprt_results.pgn

REM SPRT Parameters
REM H0: Elo = 0 (no improvement)
REM H1: Elo = 5 (5 ELO improvement)
set ELO0=0
set ELO1=5
set ALPHA=0.05
set BETA=0.05

echo Configuration:
echo   New Engine: %ENGINE_NEW%
echo   Baseline: %ENGINE_BASE%
echo   Hash: %HASH% MB
echo   Threads: %THREADS%
echo   Concurrency: %CONCURRENCY%
echo   Games: %GAMES%
echo   Time Control: %TC%
echo   SPRT Bounds: [%ELO0%, %ELO1%]
echo.

REM Check if engines exist
if not exist "%ENGINE_NEW%" (
    echo ERROR: New engine not found: %ENGINE_NEW%
    echo Please build the engine first.
    goto :error
)

if not exist "%ENGINE_BASE%" (
    echo WARNING: Baseline engine not found: %ENGINE_BASE%
    echo Creating a copy of current engine as baseline...
    copy /Y "%ENGINE_NEW%" "%ENGINE_BASE%" >nul
    if errorlevel 1 (
        echo ERROR: Failed to copy engine. Check permissions or if file is in use.
        goto :error
    )
    echo Copy created.
)

REM Check for Book
set BOOK_ARGS=
if exist "%BOOK%" (
    echo Using opening book: %BOOK%
    set BOOK_ARGS=-openings file="%BOOK%" format=pgn order=random
) else (
    echo WARNING: Opening book not found at %BOOK%
    echo Running without opening book - startpos.
)

REM Check for cutechess-cli
where cutechess-cli >nul 2>&1
if errorlevel 1 (
    echo ERROR: cutechess-cli not found in PATH.
    echo Please install it or add it to PATH.
    goto :error
)

echo Starting SPRT test...
echo.

REM Run cutechess-cli
REM Note: Using ^ for line continuation. Do not add spaces after ^
cutechess-cli ^
 -engine name="New" cmd="%ENGINE_NEW%" option.Hash=%HASH% option.Threads=%THREADS% ^
 -engine name="Baseline" cmd="%ENGINE_BASE%" option.Hash=%HASH% option.Threads=%THREADS% ^
 -each proto=uci tc=%TC% ^
 %BOOK_ARGS% ^
 -sprt elo0=%ELO0% elo1=%ELO1% alpha=%ALPHA% beta=%BETA% ^
 -games %GAMES% ^
 -repeat ^
 -recover ^
 -draw movenumber=50 movecount=8 score=10 ^
 -resign movecount=3 score=1000 ^
 -pgnout "%OUTPUT_PGN%" ^
 -concurrency %CONCURRENCY%

echo.
echo Test completed. Results saved to %OUTPUT_PGN%
goto :end

:error
echo.
echo Script terminated with errors.
pause
exit /b 1

:end
echo.
pause
