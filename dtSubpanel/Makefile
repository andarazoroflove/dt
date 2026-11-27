CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -fPIC -I/usr/dt/include
LDFLAGS = -shared -L/usr/dt/lib -Wl,-rpath,/usr/dt/lib -lXm -lXt -lX11 -ldl
BINDIR  = $(HOME)/.local/bin

all: libdtwm_subpanel_cols.so

libdtwm_subpanel_cols.so: dtwm_subpanel_cols.c
	$(CC) $(CFLAGS) -o $@ dtwm_subpanel_cols.c $(LDFLAGS)

install: libdtwm_subpanel_cols.so
	mkdir -p $(BINDIR)
	install -m 755 libdtwm_subpanel_cols.so $(BINDIR)/libdtwm_subpanel_cols.so.new
	mv -f $(BINDIR)/libdtwm_subpanel_cols.so.new $(BINDIR)/libdtwm_subpanel_cols.so
	install -m 755 dtwm-wrapper $(BINDIR)/dtwm-wrapper

clean:
	rm -f libdtwm_subpanel_cols.so

.PHONY: all install clean
