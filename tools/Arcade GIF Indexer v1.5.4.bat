@echo off
setlocal enabledelayedexpansion
title Retro Pixel LED - Universal Indexer v1.5.4

:: --- LOCAL PATH CONFIGURATION ---
set "cache_name=batocera_cache.txt"
set "name_list=%~dp0nombres_roms_batocera.txt"
set "tmp_file=%~dp0cache_temp.tmp"

:menu
cls
echo ======================================================
echo       RETRO PIXEL LED - INDEX MANAGER v1.5.4
echo ======================================================
echo  1. Update ONE system (e.g.: snes)
echo  2. Update ALL systems (Full scan)
echo  3. Delete index and start over
echo  4. Exit
echo ======================================================
set /p opt="Select an option [1-4]: "

if "%opt%"=="3" (
    if exist "%name_list%" del /f /q "%name_list%"
    echo Local files deleted.
    pause
    goto :menu
)
if "%opt%"=="4" exit

:configure_paths
set /p sd_drive="SD drive letter (e.g.: E): "
set /p roms_base="Base ROM path (e.g.: \\192.168.1.112\share\roms): "

if not exist "%roms_base%" (
    echo [ERROR] Cannot access the network path.
    pause
    goto :menu
)

set "sd_path=%sd_drive%:\batocera"
set "final_cache=%sd_drive%:\%cache_name%"

if not exist "%sd_path%" mkdir "%sd_path%"

:: REFERENCE FILE CREATION/RESET
echo GIF REFERENCE LIST > "%name_list%"
echo Generated on: %date% %time% >> "%name_list%"
echo ========================================== >> "%name_list%"

if "%opt%"=="1" (
    set /p systemName="Enter the system name (e.g.: snes): "
    call :process_system
    goto :finish
)

if "%opt%"=="2" (
    for /d %%d in ("%roms_base%\*") do (
        set "systemName=%%~nxd"
        call :process_system
    )
    goto :finish
)
goto :menu

:process_system
echo.
echo --- Processing system: [!systemName!] ---
echo [!systemName!] >> "%name_list%"

if not exist "%sd_path%\!systemName!" mkdir "%sd_path%\!systemName!"

if exist "%final_cache%" (
    findstr /v /c:"|!systemName!|" "%final_cache%" > "%tmp_file%"
    move /y "%tmp_file%" "%final_cache%" >nul
)

:: 1. Fallback logo
if exist "%sd_path%\!systemName!\_logo.gif" (
    (echo 01^|!systemName!^|default^|/batocera/!systemName!/_logo.gif)>>"%final_cache%"
)

:: 2. Game search
set "found=0"
for %%f in ("%roms_base%\!systemName!\*.*") do (
    set "rom_name=%%~nf"
    
    :: ALWAYS WRITE TO THE NAME LIST (so you know which GIFs to create)
    echo   !rom_name! >> "%name_list%"
    
    if exist "%sd_path%\!systemName!\!rom_name!.gif" (
        (echo 00^|!systemName!^|!rom_name!^|/batocera/!systemName!/!rom_name!.gif)>>"%final_cache%"
        echo [OK] Game: !rom_name!
        set "found=1"
    )
)

if "!found!"=="0" (
    echo [i] There are no GIFs for !systemName!. Check: %name_list%
)
echo. >> "%name_list%"
goto :eof

:finish
echo.
echo ======================================================
echo  PROCESS COMPLETE
echo  List: %name_list%
echo ======================================================
pause
goto :menu
