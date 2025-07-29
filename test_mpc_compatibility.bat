@echo off
REM Test script to debug MPC-HC compatibility issues
REM This script helps test different MPC-HC command line options

echo Testing MPC-HC compatibility for stdin streaming...
echo.

REM Test 1: Basic stdin with /play
echo Test 1: Basic stdin with /play
"C:\Program Files\MPC-HC\mpc-hc64.exe" - /play
echo.

REM Test 2: With dubdelay (our current approach)
echo Test 2: With dubdelay (current implementation)
"C:\Program Files\MPC-HC\mpc-hc64.exe" /play /dubdelay 0 -
echo.

REM Test 3: With additional buffering options
echo Test 3: With additional buffering options
"C:\Program Files\MPC-HC\mpc-hc64.exe" /play /new /minimized -
echo.

REM Test 4: Different parameter order
echo Test 4: Different parameter order
"C:\Program Files\MPC-HC\mpc-hc64.exe" /play - /dubdelay 0
echo.

echo All tests completed. Check which configuration works best.
echo If none work with stdin, consider using named pipes or temporary files.
pause