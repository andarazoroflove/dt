CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -I/usr/dt/include
LDFLAGS = -L/usr/dt/lib -Wl,-rpath,/usr/dt/lib -lDtWidget -lDtSvc -ltt -lDtXinerama -lXpm -lXm -lXt -lX11 -lm -ldl
BINDIR  = $(HOME)/.local/bin

all: cde-calendar

cde-calendar: cde_calendar.c
	$(CC) $(CFLAGS) -o $@ cde_calendar.c $(LDFLAGS)

install: cde-calendar
	mkdir -p $(BINDIR) $(HOME)/.dt/types
	install -m 755 cde-calendar $(BINDIR)/cde-calendar
	install -m 755 cde-calendar $(BINDIR)/dtcm
	install -m 755 cde-calendar-backend.py $(BINDIR)/cde-calendar-backend
	sed "s|__HOME__|$(HOME)|g" cde/dtcalendar.dt.in > $(HOME)/.dt/types/dtcalendar.dt
	if [ -x /usr/dt/bin/dtaction ]; then /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true; fi

clean:
	rm -f cde-calendar

.PHONY: all install clean
