# FreeBSD kernel module if_octnic (Cavium CN6640-SNIC10E).
# Build on FreeBSD 14+ with the matching /usr/src:  make   -> if_octnic.ko
# Install:                                          make install
# This Makefile needs bsd.kmod.mk, so it only works on FreeBSD (not GNU make).

KMOD=	if_octnic
SRCS=	if_octnic.c

CFLAGS+=	-Wall -Werror

.include <bsd.kmod.mk>
