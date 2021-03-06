SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

CFLAGS=-Wall -Wextra -Wpedantic -Wno-unused-parameter
LIBS=-lwayland-client
OBJ=lsft.o wlr-foreign-toplevel-management-unstable-v1.o
GEN=wlr-foreign-toplevel-management-unstable-v1.c wlr-foreign-toplevel-management-unstable-v1.h

lsft: $(OBJ)
	$(CC)$ $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install:
	install -D -t $(DESTDIR)$(BINDIR) lsft
	install -D -t $(DESTDOR)$(MANDIR)/man1/ lsft.1

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/lsft
	$(RM) $(DESTDIR)$(MANDIR)/man1/lsft.1

clean:
	$(RM) lsft $(GEN) $(OBJ)

.PHONY: clean install

