@echo off
REM SPDX-License-Identifier: GPL-2.0
REM install.bat - Windows entry point of the global installer (Linux: sudo ./install.sh).
REM Double-click it or run it from a prompt; it asks for Administrator rights, then walks
REM through: card present? -> install driver -> card already flashed? -> (serial + flash) ->
REM boot task. Options are passed through: install.bat -Yes answers yes to every question.
call "%~dp0winmod\install_windows.bat" %*

