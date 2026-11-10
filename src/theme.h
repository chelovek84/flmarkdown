// theme.h - color themes, tuned for low-contrast-fatigue reading.
#pragma once

struct Theme {
  const char *name;
  // all colors are 0xRRGGBB
  unsigned bg;        // page background
  unsigned fg;        // body text (never pure black/white)
  unsigned muted;     // quotes, secondary text
  unsigned heading;   // headings
  unsigned link;      // hyperlinks
  unsigned code_fg;   // inline code text
  unsigned code_bg;   // code block / table header background
  unsigned quote_bg;  // block quote background
  unsigned bar;       // block quote left bar, rules
  unsigned zebra;     // alternate table row
  unsigned sel;       // selection, current search match (background)
  unsigned mark;      // other search matches (text color)
};

static const Theme kThemes[] = {
  // name      bg        fg        muted     heading   link      code_fg   code_bg   quote_bg  bar       zebra     sel       mark
  {"Night",  0x1f2228, 0xc8ccd3, 0x8a909b, 0xe4e6ea, 0x82b1e0, 0xe0b97a, 0x282c34, 0x252931, 0x4b5262, 0x23272e, 0x3e5a80, 0x7ddcaa},
  {"Paper",  0xf7f6f2, 0x2e3236, 0x6a6f75, 0x1b1e21, 0x2d68b0, 0x9b4a2f, 0xeceae3, 0xf0eee8, 0xc9c5ba, 0xf1efe9, 0xb9d3f0, 0x0f7a4a},
  {"Sepia",  0xf3ead7, 0x3d3226, 0x75664f, 0x2b2218, 0x7a4a1c, 0x8c3b1e, 0xe8dcc1, 0xede2ca, 0xc6b38d, 0xeee4ce, 0xdcc79b, 0x2c6e3a},
};
static const int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);
