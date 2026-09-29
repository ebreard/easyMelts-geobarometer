@echo off
rem Runs a CSV batch with the command-line tool, several compositions at once, and joins the results
rem into <name>_summary.csv and <name>_detail.csv. Put this file next to geobarometer_cli.exe and the
rem CSV file, set CSV and SETTINGS below (same keys as in the user guide, section 10), then double-click it.

cd /d "%~dp0"
set CSV=glasses_template.csv
set SETTINGS=p_start=500 p_end=25 p_step=25 t_start=1100 t_end=700 t_step=1 buffer=nno offsets=0 phases=quartz,feldspar1,feldspar2 rule=any h2o=13

rem one part per processor core, keeping one core free for the rest of the computer
set /a JOBS=%NUMBER_OF_PROCESSORS%-1
if %JOBS% LSS 1 set JOBS=1

"%~dp0geobarometer_cli.exe" "%CSV%" %SETTINGS% jobs=%JOBS% out=%CSV:.csv=%
pause
