# dtSubpanel (`libdtwm_subpanel_cols.so`)

**Automatic Multi-Column CDE Front Panel Subpanel Engine for `dtwm`**

Stock CDE `dtwm` lays out Front Panel `SUBPANEL` controls in a single vertical `XmForm` chain, which overflows `768p`/`800p` displays and hangs down over the main Front Panel when many applications are installed.

`libdtwm_subpanel_cols.so` (preloaded into `dtwm` via `~/.local/bin/dtwm-wrapper` and `Dtsession*wmStartupCommand`) dynamically inspects every CDE subpanel when opened:
- Keeps short subpanels in a classic single column.
- Automatically splits tall subpanels (such as **Utilities**) into **2 (or more) balanced side-by-side columns** with an etched Motif vertical divider whenever a single column would exceed the space above the Front Panel.
- Enforces strict vertical bounds so a subpanel **never covers the main Front Panel**.
