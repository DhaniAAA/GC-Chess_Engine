@echo off
setlocal enabledelayedexpansion
REM ============================================================================
REM Gauntlet Testing Script for GC-Engine
REM Tests against multiple opponent engines to estimate ELO
REM ============================================================================

echo ============================================
echo   GC-Engine Gauntlet Testing Script
echo ============================================
echo.

REM Configuration
set ENGINE_TEST=output\main.exe
set HASH=64
set THREADS=1
set GAMES_PER_OPPONENT=100
set TC=5+0.05
set CUTECHESS=cutechess\cutechess-cli.exe

REM Opponent engines directory
set OPPONENT_DIR=engines

echo Configuration:
echo   Tested Engine: %ENGINE_TEST%
echo   Games per opponent: %GAMES_PER_OPPONENT%
echo   Time Control: %TC% (5 sec + 0.05 sec increment)
echo   Hash: %HASH% MB
echo.

REM Check if main engine exists
if not exist "%ENGINE_TEST%" (
    echo ERROR: Engine not found: %ENGINE_TEST%
    echo Please build the engine first with 'mingw32-make'
    pause
    exit /b 1
)

REM Check if cutechess-cli exists
if not exist "%CUTECHESS%" (
    echo.
    echo ERROR: cutechess-cli not found at: %CUTECHESS%
    echo.
    echo Please either:
    echo   1. Download cutechess-cli from: https://github.com/cutechess/cutechess/releases
    echo   2. Extract to 'cutechess' subfolder
    echo.
    goto :show_example
)

echo Found cutechess-cli: %CUTECHESS%

REM Check for opponent engines
if not exist "%OPPONENT_DIR%" (
    mkdir "%OPPONENT_DIR%"
    echo Created 'engines' folder. Please add opponent engines there.
    goto :show_example
)

REM Count opponent engines
set OPPONENT_COUNT=0
for %%f in (%OPPONENT_DIR%\*.exe) do set /a OPPONENT_COUNT+=1

if %OPPONENT_COUNT% EQU 0 (
    echo No opponent engines found in '%OPPONENT_DIR%' folder.
    echo Please add some UCI engines to test against.
    goto :show_example
)

echo Found %OPPONENT_COUNT% opponent engine(s) in '%OPPONENT_DIR%':
for %%f in (%OPPONENT_DIR%\*.exe) do echo   - %%~nxf
echo.

REM Ask to run
set /p RUN_TEST="Run gauntlet test now? (y/n): "
if /i not "%RUN_TEST%"=="y" goto :show_example

echo.
echo Starting Gauntlet Test...
echo ============================================
echo.

REM Run gauntlet against each opponent
set TOTAL_WINS=0
set TOTAL_LOSSES=0
set TOTAL_DRAWS=0

for %%O in (%OPPONENT_DIR%\*.exe) do (
    echo.
    echo ============================================
    echo Testing against: %%~nxO
    echo Games: %GAMES_PER_OPPONENT%  TC: %TC%
    echo ============================================
    echo.

    REM Run cutechess and capture output to temp file
    "%CUTECHESS%" -engine name="GC-Engine" cmd="%ENGINE_TEST%" option.Hash=%HASH% -engine name="%%~nO" cmd="%%O" option.Hash=%HASH% -each proto=uci tc=%TC% -games %GAMES_PER_OPPONENT% -repeat -recover -draw movenumber=50 movecount=8 score=10 -resign movecount=3 score=1000 -pgnout "results_vs_%%~nO.pgn" -ratinginterval 10 > temp_results.txt 2>&1
    type temp_results.txt

    REM Parse results from temp file
    for /f "tokens=7,9,11 delims= " %%a in ('findstr /C:"Score of GC-Engine" temp_results.txt ^| findstr /V "playing"') do (
        set WINS=%%a
        set LOSSES=%%b
        set DRAWS=%%c
    )

    echo.
    echo ----------------------------------------
    echo Finished testing against %%~nxO
    echo Results saved to: results_vs_%%~nO.pgn
    echo.
    echo Match Result: W-L-D = !WINS!-!LOSSES!-!DRAWS!
    echo.
)

echo.
echo ============================================
echo         GAUNTLET COMPLETE!
echo ============================================
echo.
echo Results saved to results_vs_*.pgn files
echo.

REM Clean up temp file
if exist temp_results.txt del temp_results.txt

REM Run ELO calculator if Python is available
echo Calculating ELO...
echo.
python calculate_elo.py 2>nul
if errorlevel 1 (
    echo.
    echo Python not found or script error. To calculate ELO manually:
    echo   Performance ELO = OpponentELO + 400 * log10(Score / (1 - Score))
    echo.
    echo Or install Python and run: python calculate_elo.py
)
echo.
goto :end

:show_example
echo.
echo ============================================
echo Example cutechess-cli command:
echo ============================================
echo.
echo %CUTECHESS% -engine name="GC-Engine" cmd="%ENGINE_TEST%" option.Hash=%HASH% -engine name="Opponent" cmd="engines\opponent.exe" option.Hash=%HASH% -each proto=uci tc=%TC% -games %GAMES_PER_OPPONENT% -repeat -recover -draw movenumber=50 movecount=8 score=10 -resign movecount=3 score=1000 -pgnout results.pgn
echo.
echo ============================================
echo Recommended opponent engines for ELO testing:
echo ============================================
echo.
echo   ~1000 ELO: Fruit 2.1, Faile 1.4, TSCP
echo   ~1200 ELO: Mediocre 0.34, Sungorus 1.4
echo   ~1500 ELO: Rodent III (easy), Zurichess
echo   ~1800 ELO: Demolito, Counter
echo   ~2000 ELO: Laser, Xiphos
echo   ~2200 ELO: Ethereal, Rubichess
echo   ~2500 ELO: Stockfish (limited), Komodo
echo.
echo Download engines from:
echo   https://computerchess.org.uk/ccrl/
echo   https://www.chess.com/computer-chess-championship
echo.

:end
pause
