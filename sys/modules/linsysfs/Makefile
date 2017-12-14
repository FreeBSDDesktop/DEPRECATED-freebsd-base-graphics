# $FreeBSD: head/sys/modules/linsysfs/Makefile 314651 2017-03-04 10:10:17Z ngie $

.PATH: ${SRCTOP}/sys/compat/linsysfs

KMOD=	linsysfs
SRCS=	vnode_if.h \
	device_if.h bus_if.h  pci_if.h \
	linsysfs.c

.include <bsd.kmod.mk>
