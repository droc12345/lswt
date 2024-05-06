SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man
BASHCOMPDIR=$(PREFIX)/share/bash-completion/completions

CFLAGS=-Wall -Werror -Wextra -Wpedantic -Wno-error=unused-function -Wno-unused-parameter -Wconversion -Wformat-security -Wformat -Wsign-conversion -Wfloat-conversion -Wunused-result
LIBS=-lwayland-client
OBJ=lswt.o wlr-foreign-toplevel-management-unstable-v1.o ext-foreign-toplevel-list-v1.o
GEN=wlr-foreign-toplevel-management-unstable-v1.c wlr-foreign-toplevel-management-unstable-v1.h ext-foreign-toplevel-list-v1.c ext-foreign-toplevel-list-v1.h

lswt: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

$(OBJ): $(GEN)

%.c: %.xml
	$(SCANNER) private-code < $< > $@

%.h: %.xml
	$(SCANNER) client-header < $< > $@

install: lswt
	install        -D lswt            $(DESTDIR)$(BINDIR)/lswt
	install -m 644 -D lswt.1          $(DESTDIR)$(MANDIR)/man1/lswt.1
	install        -D bash-completion $(DESTDIR)$(BASHCOMPDIR)/lswt

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/lswt
	$(RM) $(DESTDIR)$(MANDIR)/man1/lswt.1
	$(RM) $(DESTDIR)$(BASHCOMPDIR)/lswt

clean:
	$(RM) lswt $(GEN) $(OBJ)

.PHONY: clean install

