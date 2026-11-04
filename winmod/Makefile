# SPDX-License-Identifier: GPL-2.0
# octnic Windows driver — WDK nmake Makefile
# Usage (on Windows with WDK):
#   Setenv.cmd from WDK, then: nmake clean && nmake

TARGET_NAME = octnic
TARGETLIBS = $(DDK_LIB_PATH)\ndis.lib

SOURCES = \
    octnic_main.c \
    octnic_init.c \
    octnic_tx.c \
    octnic_rx.c \
    octnic_oid.c \
    octnic_watchdog.c \
    octnic_utils.c

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

TARGETTYPE = DRIVER
DRIVER_TYPE = WDM

!IF DEFINED(WDK_WIN11)
TARGETLIBS += $(SDK_LIB_PATH)\ntdll.lib $(SDK_LIB_PATH)\hal.lib
!ELSE
TARGETLIBS += $(DDK_LIB_PATH)\wdmsec.lib
!ENDIF

INCLUDES = $(INCLUDES) /I$(SRC_DIR)

!INCLUDE $(NTMAKEENV)\makefile.def
