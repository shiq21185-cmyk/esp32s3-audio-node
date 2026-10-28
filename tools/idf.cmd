@echo off
REM ---------------------------------------------------------------
REM ESP-IDF command-line wrapper (for the agent and for humans).
REM
REM Why not export.bat / export.sh / export.ps1:
REM   1) export.sh in git-bash is refused by the MSYS guard in
REM      tools/idf_tools.py (checks env var MSYSTEM);
REM   2) dot-sourcing export.ps1 in PowerShell is blocked by policy;
REM   3) export.bat only works inside a real cmd.exe.
REM So we set PATH / IDF_* ourselves and call idf.py with the venv python.
REM
REM Usage:  tools\idf.cmd -C firmware\d1-bringup build
REM         tools\idf.cmd -C firmware\d1-bringup -p COM5 flash monitor
REM ---------------------------------------------------------------

set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.3.5"
set "IDF_TOOLS_PATH=C:\Espressif"
set "IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.3_py3.12_env"

set "ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20240305\"

set "PATH=%IDF_PATH%\tools;%IDF_PYTHON_ENV_PATH%\Scripts;%PATH%"
set "PATH=C:\Espressif\tools\cmake\3.30.2\bin;%PATH%"
set "PATH=C:\Espressif\tools\ninja\1.12.1;%PATH%"
set "PATH=C:\Espressif\tools\xtensa-esp-elf\esp-13.2.0_20250707\xtensa-esp-elf\bin;%PATH%"
set "PATH=C:\Espressif\tools\riscv32-esp-elf\esp-13.2.0_20250707\riscv32-esp-elf\bin;%PATH%"
set "PATH=C:\Espressif\tools\idf-exe\1.0.3;%PATH%"

"%IDF_PYTHON_ENV_PATH%\Scripts\python.exe" "%IDF_PATH%\tools\idf.py" %*
exit /b %ERRORLEVEL%
