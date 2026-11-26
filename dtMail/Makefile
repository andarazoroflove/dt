CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -I/usr/dt/include
LDFLAGS = -L/usr/dt/lib -Wl,-rpath,/usr/dt/lib -lDtWidget -lDtSvc -ltt -lDtXinerama -lXpm -lXm -lXt -lX11 -lm -ldl
BINDIR  = $(HOME)/.local/bin

all: cde-mail

cde-mail: cde_mail.c cde_oe_icons.h
	$(CC) $(CFLAGS) -o $@ cde_mail.c $(LDFLAGS)

install: cde-mail
	mkdir -p $(BINDIR) $(HOME)/.dt/icons $(HOME)/.dt/types/fp_dynamic
	install -m 755 cde-mail $(BINDIR)/cde-mail
	ln -sf $(BINDIR)/cde-mail $(BINDIR)/dtmail
	install -m 755 cde-mail-backend.py $(BINDIR)/cde-mail-backend
	install -m 755 cde-mail-htmlview.py $(BINDIR)/cde-mail-htmlview
	install -m 755 cde-mail-webauth.py $(BINDIR)/cde-mail-webauth
	cp -f icons/*.pm $(HOME)/.dt/icons/
	sed "s|__HOME__|$(HOME)|g" cde/dtmail.dt.in > $(HOME)/.dt/types/dtmail-o365.dt
	cp -f cde/dtmail1.fp $(HOME)/.dt/types/fp_dynamic/dtmail1.fp
	if [ -x /usr/dt/bin/dtaction ]; then /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true; fi

clean:
	rm -f cde-mail

.PHONY: all install clean
