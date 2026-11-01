# SPDX-License-Identifier: GPL-2.0
# Makefile for octnic Windows NDIS miniport driver
# Build with WDK (Windows Driver Kit)
#
# Prerequisites:
#   - Windows 10/11 SDK + WDK installed
#   - Setenv.cmd from WDK sourced before build
#
# Usage:
#   msbuild octnic.vcxproj
#   or
#   build_release.bat

TARGET_NAME = octnic

DRIVER_TYPE = WDM

!IF DEFINED(WDK_WIN11)
TARGETLIBS = $(SDK_LIB_PATH)\ntdll.lib $(SDK_LIB_PATH)\hal.lib $(SDK_LIB_PATH)\wdmsec.lib
!ELSE
TARGETLIBS = $(SDK_LIB_PATH)\ntdll.lib $(SDK_LIB_PATH)\hal.lib
!ENDIF

INCLUDES = $(INCLUDES) /I$(SRC_DIR)

SOURCELIBS = \
    $(DDK_LIB_PATH)\ndis.lib \
    $(DDK_LIB_PATH)\wdmsec.lib

SOURCES = \
    octnic_main.c \
    octnic_init.c \
    octnic_tx.c \
    octnic_rx.c \
    octnic_oid.c \
    octnic_watchdog.c \
    octnic_utils.c \
    octnic.rc

HEADERS = \
    octnic.h

OBJECTS = \
    $(OBJ_PATH)\octnic_main.obj \
    $(OBJ_PATH)\octnic_init.obj \
    $(OBJ_PATH)\octnic_tx.obj \
    $(OBJ_PATH)\octnic_rx.obj \
    $(OBJ_PATH)\octnic_oid.obj \
    $(OBJ_PATH)\octnic_watchdog.obj \
    $(OBJ_PATH)\octnic_utils.obj

TARGETLIBS += $(DDK_LIB_PATH)\ndis.lib

TARGETTYPE = DRIVER

!INCLUDE $(NTMAKEENV)\makefile.def
