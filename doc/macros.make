PACKAGE		= ldm
MAJOR_LEVEL	= 6
MINOR_LEVEL	= 8
BUG_LEVEL	= 1
REV_LEVEL	= A
MINOR_VERSION	= $(MAJOR_LEVEL).$(MINOR_LEVEL)
BUG_VERSION	= $(MINOR_VERSION).$(BUG_LEVEL)
REV_VERSION	= $(MINOR_VERSION).$(REV_LEVEL)
FTPDIR		= /home/ftp/pub/ldm
RSYNC_FLAGS	= --rsh=ssh --rsync-path=/opt/bin/rsync
