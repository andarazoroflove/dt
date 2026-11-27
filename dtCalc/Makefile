CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -I/usr/dt/include
LDFLAGS = -L/usr/dt/lib -Wl,-rpath,/usr/dt/lib -lXpm -lXm -lXt -lX11 -lm
BINDIR  = $(HOME)/.local/bin

all: cde-calc

cde-calc: cde_calc.c cde_calc_icon.h
	$(CC) $(CFLAGS) -o $@ cde_calc.c $(LDFLAGS)

install: cde-calc
	mkdir -p $(BINDIR) $(HOME)/.dt/icons $(HOME)/.dt/types/fp_dynamic
	install -m 755 cde-calc $(BINDIR)/cde-calc
	ln -sf $(BINDIR)/cde-calc $(BINDIR)/dtCalc
	ln -sf $(BINDIR)/cde-calc $(BINDIR)/dtcalc
	cp -f icons/*.pm $(HOME)/.dt/icons/
	cp -f cde/dtcalc1.fp $(HOME)/.dt/types/fp_dynamic/dtcalc1.fp
	sed "s|__HOME__|$(HOME)|g" cde/dtcalc.dt.in > $(HOME)/.dt/types/dtcalc.dt
	if [ -x /usr/dt/bin/dtaction ]; then /usr/dt/bin/dtaction ReloadActions 2>/dev/null || true; fi

clean:
	rm -f cde-calc

.PHONY: all install clean
