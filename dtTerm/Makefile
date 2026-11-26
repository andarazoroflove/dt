CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -I/usr/dt/include
LDFLAGS = -L/usr/dt/lib -Wl,-rpath,/usr/dt/lib -lDtTerm -lDtWidget -lDtSvc -ltt -lDtXinerama -lXpm -lXm -lXt -lX11
BINDIR  = $(HOME)/.local/bin

all: cde-term

cde-term: cde_term.c cde_term_icon.h
	$(CC) $(CFLAGS) -o $@ cde_term.c $(LDFLAGS)

install: cde-term
	mkdir -p $(BINDIR) $(HOME)/.dt/icons $(HOME)/.dt/types/fp_dynamic
	install -m 755 cde-term $(BINDIR)/cde-term
	ln -sf $(BINDIR)/cde-term $(BINDIR)/dtTerm
	ln -sf $(BINDIR)/cde-term $(BINDIR)/dtterm
	cp -f icons/*.pm $(HOME)/.dt/icons/
	rm -f $(HOME)/.dt/types/fp_dynamic/TermUtil1.fp
	cp -f cde/dtterm1.fp $(HOME)/.dt/types/fp_dynamic/dtterm1.fp
	sed "s|__HOME__|$(HOME)|g" cde/dtterm.dt.in > $(HOME)/.dt/types/dtterm.dt
	if [ -x /usr/dt/bin/dtaction ]; then /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true; fi

clean:
	rm -f cde-term

.PHONY: all install clean
