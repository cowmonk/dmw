include config.mk

SRC = dmw.c bar.c util.c
OBJ = ${SRC:.c=.o} swc-protocol.o

all: dmw

.c.o:
	${CC} ${CPPFLAGS} ${CFLAGS} -std=c99 -D_POSIX_C_SOURCE=200809L -DVERSION=\"${VERSION}\" ${INCS} -c $<

dmw: ${OBJ}
	${CC} ${LDFLAGS} -o $@ ${OBJ} ${LIBS}

dmw.o: config.h bar.h util.h
bar.o: bar.h util.h swc-client-protocol.h
${OBJ}: config.mk
util.o: util.h

config.h:
	cp config.def.h $@

swc-client-protocol.h: ${SWC_XML}
	wayland-scanner client-header ${SWC_XML} $@

swc-protocol.c: ${SWC_XML}
	wayland-scanner private-code ${SWC_XML} $@

clean:
	rm -f dmw ${OBJ} swc-client-protocol.h swc-protocol.c dmw-${VERSION}.tar.gz

dist:
	mkdir -p dmw-${VERSION}
	cp LICENSE Makefile README config.def.h config.mk bar.h util.h ${SRC} dmw-${VERSION}/
	tar -cf dmw-${VERSION}.tar dmw-${VERSION}
	gzip -f dmw-${VERSION}.tar
	rm -rf dmw-${VERSION}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin ${DESTDIR}${MANPREFIX}/man1
	cp dmw ${DESTDIR}${PREFIX}/bin/dmw
	chmod 755 ${DESTDIR}${PREFIX}/bin/dmw
	# TODO make manpage (reuse dwm?)
	# sed 's/VERSION/${VERSION}/g' dwm.1 > ${DESTDIR}${MANPREFIX}/man1/dmw.1
	# chmod 644 ${DESTDIR}${MANPREFIX}/man1/dwm.1

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dmw ${DESTDIR}${MANPREFIX}/man1/dmw.1

.PHONY: all clean dist install uninstall
