SCANNER := wayland-scanner

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin

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

uninstall:
	$(RM) $(DESTDIR)$(BINDIR)/lsft

clean:
	rm -f lsft $(GEN) $(OBJ)

.PHONY: clean install

