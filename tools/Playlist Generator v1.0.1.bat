@echo off
title Retro Pixel LED - Playlist Generator (STANDARD)
color 0B
setlocal enabledelayedexpansion

:: Configuration
set "TARGET_DIR=gifs"
set "PLAYLIST_DIR=playlists"
set "ROOT_DIR=%~dp0"

echo ========================================================
echo   RETRO PIXEL LED - INTERACTIVE PLAYLIST GENERATOR
echo ========================================================
echo.

if not exist "%TARGET_DIR%" (
    color 0C
    echo [ERROR] Folder not found '%TARGET_DIR%'.
    pause
    exit /b
)

if not exist "%PLAYLIST_DIR%" mkdir "%PLAYLIST_DIR%"

echo [1] Scanning folders...
echo.

set /a folderCount=0
for /f "tokens=*" %%D in ('dir /b /ad "%TARGET_DIR%"') do (
    set /a folderCount+=1
    set "folder[!folderCount!]=%%D"
    echo  [!folderCount!] %%D
)

echo.
echo [2] Folder Selection
echo --------------------------------------------------------
echo Type the numbers separated by commas (example: 1,3,5)
echo Or type "ALL" to include every folder.
echo --------------------------------------------------------
set /p selection="Selection: "

echo.
set /p playlistName="[3] List name: "
set "OUTPUT_FILE=%ROOT_DIR%%PLAYLIST_DIR%\%playlistName%.txt"

if exist "%OUTPUT_FILE%" del "%OUTPUT_FILE%"

set /a totalGifs=0

if /i "%selection%"=="ALL" (
    set "selection="
    for /L %%i in (1,1,%folderCount%) do (
        if %%i equ 1 (set "selection=%%i") else (set "selection=!selection!,%%i")
    )
)

:: Indexing loop
for %%s in (%selection%) do (
    set "currentFolder=!folder[%%s]!"
    echo  - Indexing: !currentFolder!
    
    :: Enter the selected folder under gifs
    pushd "%ROOT_DIR%%TARGET_DIR%\!currentFolder!"
    
    :: Search for .gif files in that folder and subfolders
    for /r %%F in (*.gif) do (
        set "FILE_ABS=%%F"
        :: Get the path relative to the folder 'gifs'
        set "FILE_REL=!FILE_ABS:%ROOT_DIR%=!"
        :: Change slashes \ to /
        set "FILE_LINE=/!FILE_REL:\=/!"
        
        echo !FILE_LINE!>>"%OUTPUT_FILE%"
        set /a totalGifs+=1
    )
    popd
)

echo.
color 0A
echo ========================================================
echo [SUCCESS] Playlist '%playlistName%.txt' created.
echo Indexed !totalGifs! GIFs successfully.
echo ========================================================
echo.
pause
