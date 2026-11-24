# dmw version
VERSION = 0.1

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

# libraries
PKG_CONFIG = pkg-config
SWC_PREFIX != ${PKG_CONFIG} --variable=prefix swc
SWC_XML = ${SWC_PREFIX}/share/swc/swc.xml
PKGS = swc wld wayland-server wayland-client xkbcommon

# includes and libs
INCS   != ${PKG_CONFIG} --cflags ${PKGS}
LIBS   != ${PKG_CONFIG} --libs ${PKGS}

# flags
CPPFLAGS ?= -Os -Wall
CFLAGS   ?= -Os -Wall

# compiler and linker
CC = cc
