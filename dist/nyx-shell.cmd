@echo off
rem Explorer right-click helper for Nyx.
rem
rem Copyright 2026 ValisSowilo.  GPL-3.0-or-later; see LICENSE.md.
rem
rem Registry commands cannot do path arithmetic -- the shell substitutes %1
rem with a literal path before cmd ever sees it, so tricks like %~dp1 in a
rem registry value silently do nothing.  Inside a batch file %1 is a real
rem argument again, so this is where "extract next to the archive, in a folder
rem named after it" can actually be worked out.
rem
rem It also keeps the console open.  Nyx runs for minutes to hours; a window
rem that closes the instant it finishes would hide both the result and any
rem error.
rem
rem NAME COLLISIONS.  Both actions invent an output name from an input name, so
rem both can collide with something already there:
rem
rem   compress  C:\data      -> C:\data.nyx    ...which may already exist
rem   extract   C:\data.nyx  -> C:\data\       ...and C:\data may exist, and
rem                                               may even be a FILE.  That is
rem                                               exactly what happens when you
rem                                               extract report.txt.nyx while
rem                                               report.txt is still sitting
rem                                               next to it -- Nyx cannot make
rem                                               a directory where a file of
rem                                               that name already is.
rem
rem Overwriting is the wrong answer for either: replacing an archive destroys
rem data, and merging into an existing folder makes it impossible to tell what
rem actually came out of the archive.  So both count up to a free name --
rem "data (1).nyx", "data (2)", and so on.
rem
rem   nyx-shell.cmd compress <path>
rem   nyx-shell.cmd extract  <archive>
rem   nyx-shell.cmd verify   <archive>
rem   nyx-shell.cmd list     <archive>

setlocal
set "NYX=%~dp0nyx.exe"

if not exist "%NYX%" (
    echo nyx.exe not found next to this script ^(%~dp0^)
    goto :wait
)

if /i "%~1"=="compress" goto :compress
if /i "%~1"=="extract"  goto :extract
if /i "%~1"=="verify"   goto :verify
if /i "%~1"=="list"     goto :list
echo unknown action "%~1"
goto :wait

rem --------------------------------------------------------------- compress
:compress
call :freename "%~2" ".nyx" TARGET
if not defined TARGET goto :toomany
echo Compressing "%~2"
echo   into "%TARGET%"
echo.
echo This is slow by design -- roughly 0.6 MB/s at the default preset.
echo Press Ctrl+C to stop; nothing you are compressing will be modified.
echo.
"%NYX%" c "%TARGET%" "%~2"
goto :wait

rem ---------------------------------------------------------------- extract
:extract
rem  C:\foo\data.nyx  ->  C:\foo\data\   (or "data (1)" if that name is taken)
call :freename "%~dp2%~n2" "" TARGET
if not defined TARGET goto :toomany
echo Extracting "%~2"
echo   into "%TARGET%\"
echo.
"%NYX%" x "%~2" "%TARGET%"
goto :wait

:verify
echo Checking "%~2"
echo.
"%NYX%" t "%~2"
goto :wait

:list
"%NYX%" l "%~2"
goto :wait

rem ------------------------------------------------------------------------
rem freename <base> <suffix> <outvar>
rem
rem Sets <outvar> to the first of
rem     base+suffix, "base (1)"+suffix, "base (2)"+suffix, ...
rem that does not already exist.
rem
rem `if exist` is true for files AND directories, which is what is wanted here:
rem a directory cannot be created where a file of that name sits, and that is
rem the failure this was written for.
rem
rem Gives up after 999 rather than looping forever, leaving <outvar> undefined.
:freename
set "_base=%~1"
set "_sfx=%~2"
set "%~3="
if not exist "%_base%%_sfx%" (
    set "%~3=%_base%%_sfx%"
    goto :eof
)
set /a _n=0
:freeloop
set /a _n+=1
if %_n% gtr 999 goto :eof
if exist "%_base% (%_n%)%_sfx%" goto :freeloop
set "%~3=%_base% (%_n%)%_sfx%"
goto :eof

:toomany
echo.
echo Could not find a free name near "%~2" -- 999 variants already exist.
echo Move or delete some of them, or run Nyx from a terminal and name the
echo output yourself.
goto :wait

:wait
echo.
if errorlevel 2 (
    echo *** FAILED: the archive is corrupt or did not verify ***
) else if errorlevel 1 (
    echo *** FAILED: usage or I/O error ***
)
pause
endlocal
