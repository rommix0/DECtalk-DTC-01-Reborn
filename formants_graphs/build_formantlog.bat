@echo off
rem Builds build\formantlog\formantlog.exe (formants_graphs\formantlog.c on the speech engine in src\) with MSVC.
rem Used by make_formant_graphs.py; see FORMANT_SYSTEM.md s11.
setlocal
set HERE=%~dp0
set SRC=%HERE%..\src
set OUT=%HERE%..\build\formantlog
if not defined VCINSTALLDIR (
  if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
  ) else (
    call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
  )
)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /O2 /W4 /D_CRT_SECURE_NO_WARNINGS /I"%SRC%\speech" /I"%SRC%\kernel" /Fo"%OUT%\obj\\" ^
   /Fe"%OUT%\formantlog.exe" "%HERE%formantlog.c" ^
   "%SRC%\speech\engine.c" "%SRC%\kernel\kernel.c" "%SRC%\kernel\console.c" ^
   "%SRC%\speech\tx_scan.c" "%SRC%\speech\tx_num.c" "%SRC%\speech\tx_word.c" "%SRC%\speech\tx_dict.c" "%SRC%\speech\tx_lts.c" ^
   "%SRC%\speech\tx_phon.c" "%SRC%\speech\tx_clause.c" "%SRC%\speech\tx_rom.c" "%SRC%\speech\tx_rom_lts.c" "%SRC%\speech\tx_rom_dict.c" ^
   "%SRC%\speech\ph_frame.c" "%SRC%\speech\ph_timing.c" "%SRC%\speech\ph_alloph.c" "%SRC%\speech\ph_clause.c" ^
   "%SRC%\speech\ph_command.c" "%SRC%\speech\ph_task.c" "%SRC%\speech\ph_rom.c" ^
   "%SRC%\speech\dsp_synth.c" "%SRC%\speech\dsp_rom.c" "%SRC%\speech\dsp_link.c"
endlocal
