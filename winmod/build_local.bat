@echo off
REM SPDX-License-Identifier: GPL-2.0
REM build_local.bat ??? build octnic.sys with plain cl/link (VS Build Tools + WDK 10.0.26100),
REM no MSBuild WDK integration needed. Output: out\octnic.sys
setlocal
set "SDK=10.0.26100.0"
set "KITS=C:\Program Files (x86)\Windows Kits\10"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out

set INC=/I"%KITS%\Include\%SDK%\km" /I"%KITS%\Include\%SDK%\shared" /I"%KITS%\Include\%SDK%\km\crt" /I"%KITS%\Include\%SDK%\um"
set DEFS=/D_AMD64_ /DAMD64 /D_WIN64 /DNDEBUG /DNDIS_MINIPORT_DRIVER=1 /DNDIS620_MINIPORT=1 /DPOOL_NX_OPTIN=1 /DNTDDI_VERSION=0x0A00000B /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DDEPRECATE_DDK_FUNCTIONS=1 /DMSC_NOOPT /DWINNT=1
set CFL=/nologo /c /kernel /O2 /Oi /W3 /GS- /Gz /Zl /Zi /wd4100 /wd4201 /wd4214 /wd4324 /Fo"out\\" /Fd"out\octnic.pdb"

cl %CFL% %DEFS% %INC% octnic_main.c octnic_init.c octnic_tx.c octnic_rx.c octnic_oid.c octnic_watchdog.c octnic_utils.c octnic_ctl.c || exit /b 1
rc /nologo /I"%KITS%\Include\%SDK%\um" /I"%KITS%\Include\%SDK%\shared" /fo out\octnic.res octnic.rc || exit /b 1

set LIBP=/LIBPATH:"%KITS%\Lib\%SDK%\km\x64"
link /nologo /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry /NODEFAULTLIB /RELEASE /MERGE:_TEXT=.text /MERGE:_PAGE=PAGE /OPT:REF /OPT:ICF /INTEGRITYCHECK /DEBUG /PDB:out\octnic.pdb /OUT:out\octnic.sys %LIBP% out\octnic_main.obj out\octnic_init.obj out\octnic_tx.obj out\octnic_rx.obj out\octnic_oid.obj out\octnic_watchdog.obj out\octnic_utils.obj out\octnic_ctl.obj out\octnic.res ntoskrnl.lib hal.lib wmilib.lib ndis.lib BufferOverflowFastFailK.lib || exit /b 1
echo BUILD OK: %CD%\out\octnic.sys
endlocal
