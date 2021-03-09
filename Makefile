SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

CFLAGS=-Wall -Werror -Wextra -Wpedantic -Wno-unused-parameter -Wconversion
LIBS=-lwayland-client
OBJ=lswt.o wlr-foreign-toplevel-management-unstable-v1.o xdg-output-unstable-v1.o
GEN=wlr-foreign-toplevel-management-unstable-v1.c wlr-foreign-toplevel-management-unstable-v1.h xdg-output-unstable-v1.c xdg-output-unstable-v1.h

lswt: $(OBJ)
	$(CC)$ $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install:
	install -D -t $(DESTDIR)$(BINDIR) lswt
	install -D -t $(DESTDOR)$(MANDIR)/man1/ lswt.1

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/lswt
	$(RM) $(DESTDIR)$(MANDIR)/man1/lswt.1

clean:
	$(RM) lswt $(GEN) $(OBJ)

.PHONY: clean install

