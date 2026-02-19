@echo off
rem tst.bat

call bld.bat
if errorlevel 1 exit /b 1

del /q tst.x tst.tmp tst.log 2>nul

minconpty.exe vim tst.tmp <tst.cr >tst.log

if not exist tst.tmp (
  echo ERROR: tst.tmp was not created.
  exit /b 1
)

echo Test passed (tst.tmp created, not checked for correctness).
