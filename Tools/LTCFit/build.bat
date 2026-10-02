@echo off
rem Build LTCFit (run from this folder). Output: LTCFit.exe
rem Usage after build: LTCFit.exe ..\..\LTC.cpp [--verify]
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /D_CRT_SECURE_NO_WARNINGS LTCFit.cpp /Fe:LTCFit.exe
