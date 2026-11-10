// flmarkdown - a calm, fast Markdown viewer (FLTK 1.4 + md4c).

#include "icon_data.h"
#include "render.h"
#include "theme.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Help_View.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Preferences.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FLMD_VERSION
#define FLMD_VERSION "dev"
#endif

extern char **environ;

// ---------------------------------------------------------------- helpers

static std::string dirname_of(const std::string &p) {
  size_t s = p.rfind('/');
  if (s == std::string::npos) return ".";
  if (s == 0) return "/";
  return p.substr(0, s);
}

static std::string basename_of(const std::string &p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

static std::string absolute(const std::string &p) {
  char buf[PATH_MAX];
  if (realpath(p.c_str(), buf)) return buf;
  if (!p.empty() && p[0] == '/') return p;
  if (getcwd(buf, sizeof buf)) return std::string(buf) + "/" + p;
  return p;
}

// "/home/me/x" -> "~/x"
static std::string tilde(const std::string &p) {
  const char *home = getenv("HOME");
  if (home && *home) {
    std::string h = home;
    if (p == h) return "~";
    if (p.compare(0, h.size() + 1, h + "/") == 0) return "~" + p.substr(h.size());
  }
  return p;
}

static std::string lower_ext(const std::string &p) {
  std::string b = basename_of(p);
  size_t d = b.rfind('.');
  if (d == std::string::npos) return "";
  std::string e = b.substr(d + 1);
  for (char &c : e) c = (char)tolower((unsigned char)c);
  return e;
}

static bool is_markdown(const std::string &p) {
  std::string e = lower_ext(p);
  return e == "md" || e == "markdown" || e == "mdown" || e == "mkd" || e == "mkdn" ||
         e == "mdwn" || e == "txt" || e.empty();
}

static bool is_image(const std::string &p) {
  std::string e = lower_ext(p);
  return e == "png" || e == "jpg" || e == "jpeg" || e == "gif" || e == "bmp" || e == "svg" ||
         e == "xpm";
}

static bool is_file(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static void xdg_open(const std::string &target) {
  pid_t pid;
  const char *argv[] = {"xdg-open", target.c_str(), nullptr};
  posix_spawnp(&pid, "xdg-open", nullptr, nullptr, (char *const *)argv, environ);
}

static std::string url_decode(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
        isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else o += s[i];
  }
  return o;
}

// Put text on both the clipboard (Ctrl+V) and the X selection (middle click).
static void copy_text(const std::string &s) {
  Fl::copy(s.c_str(), (int)s.size(), 1);
  Fl::copy(s.c_str(), (int)s.size(), 0);
}

static Fl_Color rgb(unsigned c) { return fl_rgb_color((c >> 16) & 255, (c >> 8) & 255, c & 255); }

// Blend two colors: f=0 -> a, f=1 -> b.
static unsigned mix(unsigned a, unsigned b, float f) {
  auto ch = [&](int s) {
    int x = (a >> s) & 255, y = (b >> s) & 255;
    return (unsigned)(x + (y - x) * f + 0.5f) << s;
  };
  return ch(16) | ch(8) | ch(0);
}

// ---------------------------------------------------------------- app state

static const int kMinSize = 11, kMaxSize = 32;
static const int kFindH = 40;
static const int kMenuH = 30;
static const int kDefaultSize = 19;  // page text size, pixels (Ctrl+0 returns here)
static const int kUiSize = 15;     // menu font size (doesn't zoom with the page)
static const int kMaxRecent = 10;

// Built-in pages live at pseudo paths with this prefix.
static const char *kInternal = "flmd:";
static bool is_internal(const std::string &p) { return p.compare(0, 5, kInternal) == 0; }

struct HistEntry { std::string path; int top; };

class View;
class FindBar;
class MainWin;

static struct {
  MainWin *win = nullptr;
  View *view = nullptr;
  FindBar *find = nullptr;
  Fl_Menu_Bar *menubar = nullptr;
  Fl_Menu_Button *popup_style = nullptr;  // never shown; styles the context menu
  Fl_Preferences *prefs = nullptr;
  int theme = 0;
  int size = kDefaultSize;
  bool show_menubar = true;
  std::string path;           // current file, or an internal page ("flmd:...")
  time_t mtime = 0;
  off_t fsize = -1;
  std::vector<HistEntry> back;
  std::vector<std::string> recent;
  std::string search;         // active search text (matches are marked on the page)
  std::vector<int> matches;   // start of each match in the view's HTML
  std::vector<int> match_y;   // scroll position that shows each match
  int match = -1;             // current match index
  bool clicking = false;      // a real left click's release is being handled
  bool probing = false;       // a right click is asking "which link is here?"
  std::string probe_uri;
  int rendered_w = 0;         // view width the current page was laid out for
  bool remember = true;       // record opened files in "Open Recent" (off for --check)
} app;

static void render(bool keep_scroll);
static void find_all_matches();
static void open_file(const std::string &path, const std::string &anchor = "", bool push = true);
static void layout();
static void build_menubar();
static void show_context_menu(const std::string &link);

// ---------------------------------------------------------------- selection

// Fl_Help_View has no public API to select a range of text (needed to
// highlight the current search match). Its selection state is a set of
// private static ints. Standard C++ allows naming private members in an
// explicit template instantiation, which is used here to reach exactly
// those fields - no #define tricks, no layout assumptions.
namespace hv {
template <typename Tag, typename Tag::type M> struct Grant {
  friend typename Tag::type get(Tag) { return M; }
};
#define FLMD_HV_FIELD(Name, Type, Member)                  \
  struct Name { typedef Type type; friend type get(Name); }; \
  template struct Grant<Name, &Fl_Help_View::Member>;
FLMD_HV_FIELD(SelFirst, int *, selection_first_)
FLMD_HV_FIELD(SelLast, int *, selection_last_)
FLMD_HV_FIELD(PushFirst, int *, selection_push_first_)
FLMD_HV_FIELD(PushLast, int *, selection_push_last_)
FLMD_HV_FIELD(DragFirst, int *, selection_drag_first_)
FLMD_HV_FIELD(DragLast, int *, selection_drag_last_)
FLMD_HV_FIELD(Selected, int *, selected_)
FLMD_HV_FIELD(CurrentView, Fl_Help_View **, current_view_)
#undef FLMD_HV_FIELD
}  // namespace hv

// Select bytes [first, last) of the view's HTML (Fl_Help_View highlights
// whole words that start in that range).
static void select_range(Fl_Help_View *v, int first, int last) {
  v->clear_selection();
  *get(hv::CurrentView{}) = v;
  *get(hv::SelFirst{}) = *get(hv::PushFirst{}) = *get(hv::DragFirst{}) = first;
  *get(hv::SelLast{}) = *get(hv::PushLast{}) = *get(hv::DragLast{}) = last;
  *get(hv::Selected{}) = 1;
  v->redraw();
}

// Where a match of `needle` that starts at `start` ends in the HTML: walks
// the same way Fl_Help_View::find() compares (tags skipped, an entity
// counts as one character).
static int match_end(const char *html, int start, const std::string &needle) {
  int p = start;
  size_t i = 0;
  while (html[p] && i < needle.size()) {
    if (html[p] == '<') {
      while (html[p] && html[p] != '>') p++;
      if (html[p]) p++;
      continue;
    }
    if (html[p] == '&') {
      const char *semi = strchr(html + p, ';');
      if (semi && semi - (html + p) <= 10) {
        p = (int)(semi - html) + 1;
        unsigned char c = needle[i];
        i += c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;  // one character
        continue;
      }
    }
    p++;
    i++;
  }
  return p;
}

// ---------------------------------------------------------------- widgets

class View : public Fl_Help_View {
 public:
  View(int x, int y, int w, int h) : Fl_Help_View(x, y, w, h) {}

  // Fl_Help_View keeps its links private. To learn which link (if any) is
  // under the mouse, replay the click as a left click in "probe" mode:
  // link_cb records the target instead of following it.
  std::string probe_link() {
    int top = topline();
    int saved_key = Fl::e_keysym;
    app.probe_uri.clear();
    int saved_click = Fl::e_is_click;
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_is_click = 1;  // Fl_Help_View only follows a link on a "click"
    Fl_Help_View::handle(FL_PUSH);
    app.probing = true;  // only the release follows links (push may redraw)
    Fl_Help_View::handle(FL_RELEASE);
    app.probing = false;
    Fl::e_keysym = saved_key;
    Fl::e_is_click = saved_click;
    if (topline() != top) topline(top);  // an in-page #anchor link scrolled
    return app.probe_uri;
  }

  int handle(int e) override {
    switch (e) {
      case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_LEAVE: case FL_DND_RELEASE:
        return 1;
      case FL_PUSH:
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          // Keep an existing selection (so "Copy" works); otherwise look for a link.
          std::string link = text_selected() ? "" : probe_link();
          show_context_menu(link);
          return 1;
        }
        return Fl_Help_View::handle(e);
      case FL_RELEASE: {
        if (Fl::event_button() == FL_RIGHT_MOUSE) return 1;
        // Fl_Help_View follows links only on mouse release, and its release
        // handling never redraws. Mark that window so link_cb knows a real
        // click is asking. (On push it may redraw, which also calls link_cb
        // for every image on the page - those must not count as clicks.)
        app.clicking = true;
        int r = Fl_Help_View::handle(e);
        app.clicking = false;
        return r;
      }
      case FL_PASTE: {
        // Dropped file(s): take the first one.
        std::string t = Fl::event_text();
        std::string first = t.substr(0, t.find_first_of("\r\n"));
        if (first.compare(0, 7, "file://") == 0) first = url_decode(first.substr(7));
        if (!first.empty()) open_file(first);
        return 1;
      }
    }
    return Fl_Help_View::handle(e);
  }
};

class FindBar : public Fl_Group {
 public:
  Fl_Box *label;
  Fl_Input *input;
  Fl_Box *status;
  FindBar(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    box(FL_FLAT_BOX);
    label = new Fl_Box(x + 12, y, 50, h, "Find");
    label->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    input = new Fl_Input(x + 64, y + 7, 320, h - 14);
    input->box(FL_FLAT_BOX);
    input->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    input->callback(input_cb);
    status = new Fl_Box(x + 396, y, 300, h);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    end();
    resizable(nullptr);
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    int iw = W - 64 - 12 - 160;
    if (iw > 420) iw = 420;
    if (iw < 80) iw = 80;
    label->resize(X + 12, Y, 50, H);
    input->resize(X + 64, Y + 7, iw, H - 14);
    status->resize(X + 64 + iw + 12, Y, W - (64 + iw + 24), H);
  }
  void search(bool next);
  static void input_cb(Fl_Widget *, void *) {
    bool enter = Fl::event() == FL_KEYBOARD &&
                 (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter);
    app.find->search(enter);
  }
};

class MainWin : public Fl_Double_Window {
 public:
  MainWin(int w, int h) : Fl_Double_Window(w, h, "flmarkdown") {}
  void resize(int X, int Y, int W, int H) override {
    Fl_Double_Window::resize(X, Y, W, H);
    layout();
  }
  int handle(int e) override;
};

// ---------------------------------------------------------------- layout & theme

static int column_width() { return app.size * 46; }  // ~80-90 characters per line

static void rewrap_cb(void *) { render(true); }

static void layout() {
  MainWin *w = app.win;
  if (!w || !app.view) return;
  int W = w->w(), H = w->h();
  int mh = app.show_menubar ? kMenuH : 0;
  int fh = app.find->visible() ? kFindH : 0;
  int margin = W < 700 ? 16 : 32;
  int sb = app.view->scrollbar_size() ? app.view->scrollbar_size() : Fl::scrollbar_size();
  int cw = column_width() + sb + 16;
  if (cw > W - 2 * margin) cw = W - 2 * margin;
  if (cw < 100) cw = W;
  int top = mh + 16;
  app.menubar->resize(0, 0, W, kMenuH);
  if (app.show_menubar) app.menubar->show(); else app.menubar->hide();
  app.view->resize((W - cw) / 2 + sb / 2, top, cw, H - fh - top);
  app.find->resize(0, H - fh, W, kFindH);
  // Column width changed (window narrower than the reading column):
  // re-wrap code to the new width once resizing pauses.
  if (app.rendered_w && app.view->w() != app.rendered_w) {
    Fl::remove_timeout(rewrap_cb);
    Fl::add_timeout(0.15, rewrap_cb);
  }
}

// Flat box with a thin border in the theme's color, for popup menus.
static const Fl_Boxtype kMenuBox = FL_FREE_BOXTYPE;
static Fl_Color menu_border = FL_GRAY;
static void draw_menu_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x, y, w, h);
  fl_color(menu_border);
  fl_rect(x, y, w, h);
}

// Menu bar (and its drop-downs): flat, with one soft line along the bottom.
static const Fl_Boxtype kBarBox = (Fl_Boxtype)(FL_FREE_BOXTYPE + 1);
static void draw_bar_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x, y, w, h);
  fl_color(menu_border);
  fl_xyline(x, y + h - 1, x + w - 1);
}

static void style_menu(Fl_Menu_ *m, const Theme &t) {
  m->color(rgb(t.code_bg));
  m->textcolor(rgb(t.fg));
  m->selection_color(rgb(t.sel));
  m->textfont(FL_HELVETICA);
  m->textsize(kUiSize);
  m->down_box(FL_FLAT_BOX);
}

static void apply_theme() {
  const Theme &t = kThemes[app.theme];
  Fl_Color bg = rgb(t.bg), fg = rgb(t.fg);
  app.win->color(bg);
  app.view->color(bg);
  app.view->textcolor(fg);
  app.view->selection_color(rgb(t.sel));
  // Fl_Help_View draws its selection in the global FL_SELECTION_COLOR.
  Fl::set_color(FL_SELECTION_COLOR, rgb(t.sel));
  for (Fl_Scrollbar *s : {app.view->scrollbar(), app.view->hscrollbar()}) {
    s->box(FL_FLAT_BOX);
    s->slider(FL_FLAT_BOX);
    s->color(bg);                                 // trough blends into the page
    s->selection_color(rgb(mix(t.bg, t.fg, 0.16f)));
    s->labelcolor(rgb(mix(t.bg, t.fg, 0.35f)));   // arrows
  }
  // Global colors also affect menus' borders, the find bar and dialogs.
  Fl::background((t.code_bg >> 16) & 255, (t.code_bg >> 8) & 255, t.code_bg & 255);
  Fl::foreground((t.fg >> 16) & 255, (t.fg >> 8) & 255, t.fg & 255);
  Fl::background2((t.bg >> 16) & 255, (t.bg >> 8) & 255, t.bg & 255);
  style_menu(app.menubar, t);
  app.menubar->box(kBarBox);
  style_menu(app.popup_style, t);
  app.popup_style->box(kMenuBox);
  menu_border = rgb(t.bar);
  // Menu dividers are drawn with FL_DARK3 + FL_LIGHT3 (a 3D groove that is
  // far too bright on dark themes). Make them one soft line.
  Fl::set_color(FL_DARK3, rgb(mix(t.code_bg, t.fg, 0.18f)));
  Fl::set_color(FL_LIGHT3, rgb(t.code_bg));
  FindBar *f = app.find;
  f->color(rgb(t.code_bg));
  f->label->labelcolor(rgb(t.muted));
  f->status->labelcolor(rgb(t.muted));
  f->input->color(bg);
  f->input->textcolor(fg);
  f->input->cursor_color(rgb(t.link));
  f->input->selection_color(rgb(t.sel));
  f->label->labelsize(kUiSize);
  f->status->labelsize(kUiSize);
  f->input->textsize(kUiSize + 1);
  app.win->redraw();
}

// ---------------------------------------------------------------- built-in pages

static const char *kWelcome =
    "# flmarkdown\n\n"
    "A calm Markdown viewer.\n\n"
    "- **Open a file** with *File \xe2\x86\x92 Open* (`Ctrl+O`), "
    "by dropping it onto this window, or from your file manager's *Open With* menu.\n"
    "- **Right-click** anywhere for the most useful actions.\n"
    "- *Help \xe2\x86\x92 Keyboard Shortcuts* (`F1`) lists every key.\n";

static const char *kKeys =
    "# Keyboard Shortcuts\n\n"
    "Everything here is also in the menu bar and the right-click menu.\n\n"
    "## Files\n\n"
    "| Key | Action |\n|:--|:--|\n"
    "| `Ctrl+O` | Open a file |\n"
    "| `F5` or `Ctrl+R` | Reload (also automatic when the file changes) |\n"
    "| `Alt+Left` or `Backspace` | Back after following a link |\n"
    "| `Ctrl+Q` or `q` | Quit |\n\n"
    "## Reading\n\n"
    "| Key | Action |\n|:--|:--|\n"
    "| `Space` or `PgDn` | Next page |\n"
    "| `Shift+Space` or `PgUp` | Previous page |\n"
    "| `\xe2\x86\x93` or `j` | Scroll down |\n"
    "| `\xe2\x86\x91` or `k` | Scroll up |\n"
    "| `Home` or `g` | Top |\n"
    "| `End` or `G` | Bottom |\n"
    "| `Ctrl+F` or `/` | Find (all matches are marked) |\n"
    "| `Enter` or `F3` | Next match |\n"
    "| `Shift+Enter` or `Shift+F3` | Previous match |\n"
    "| `Esc` | Close the find bar |\n"
    "| `Ctrl+C` | Copy the selected text |\n"
    "| `Ctrl+A` | Select all |\n\n"
    "## Appearance\n\n"
    "| Key | Action |\n|:--|:--|\n"
    "| `Ctrl+=` | Larger text |\n"
    "| `Ctrl+-` | Smaller text |\n"
    "| `Ctrl+0` | Default text size |\n"
    "| `Ctrl+T` | Next theme: Night, Paper, Sepia |\n"
    "| `Ctrl+M` | Show or hide the menu bar |\n"
    "| `F1` | This page |\n";

static std::string about_page() {
  return std::string("# flmarkdown ") + FLMD_VERSION +
         "\n\n"
         "A calm, lightweight Markdown viewer for Linux.\n\n"
         "- Source code: <https://github.com/chelovek84/flmarkdown>\n"
         "- License: MIT\n"
         "- Built with [FLTK](https://www.fltk.org/) " +
         std::to_string(FL_MAJOR_VERSION) + "." + std::to_string(FL_MINOR_VERSION) + "." +
         std::to_string(FL_PATCH_VERSION) +
         " and [md4c](https://github.com/mity/md4c)\n\n"
         "Settings are stored in `~/.config/flmarkdown/flmarkdown.prefs`.\n";
}

// ---------------------------------------------------------------- content

static bool read_file(const std::string &p, std::string &out, struct stat &st) {
  if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

static void set_title() {
  static std::string title;
  if (app.path == "flmd:keys") title = "Keyboard Shortcuts \xe2\x80\x94 flmarkdown";
  else if (app.path == "flmd:about") title = "About \xe2\x80\x94 flmarkdown";
  else if (is_internal(app.path)) title = "flmarkdown";
  else title = basename_of(app.path) + "  \xe2\x80\x94  " + tilde(dirname_of(app.path));
  app.win->label(title.c_str());
}

// Locate every match of app.search in the current page (Fl_Help_View::find
// scrolls as a side effect, so the scroll position is restored).
static void find_all_matches() {
  app.matches.clear();
  app.match_y.clear();
  if (app.search.empty()) return;
  int top = app.view->topline();
  for (int pos = 0; app.matches.size() < 10000;) {
    int r = app.view->find(app.search.c_str(), pos);
    if (r < 0) break;
    app.matches.push_back(r);
    // find() scrolls the match's line to the very top; leave some context.
    app.match_y.push_back(std::max(0, app.view->topline() - app.view->h() / 4));
    pos = r + 1;
  }
  app.view->topline(top);
}

static void render(bool keep_scroll) {
  std::string md, dir = ".";
  if (app.path == "flmd:keys") md = kKeys;
  else if (app.path == "flmd:about") md = about_page();
  else if (is_internal(app.path)) md = kWelcome;
  else if (is_image(app.path)) {
    // Images open inside the viewer, scaled to fit, with Back to return.
    dir = dirname_of(app.path);
    md = "![" + basename_of(app.path) + "](<" + app.path + ">)\n\n`" + tilde(app.path) + "`\n";
    struct stat st;
    if (stat(app.path.c_str(), &st) == 0) { app.mtime = st.st_mtime; app.fsize = st.st_size; }
  } else {
    struct stat st;
    dir = dirname_of(app.path);
    if (read_file(app.path, md, st)) {
      app.mtime = st.st_mtime;
      app.fsize = st.st_size;
    } else {
      md = "# Cannot open file\n\n`" + app.path + "`\n\n" + strerror(errno) + "\n";
      app.mtime = 0;
      app.fsize = -1;
    }
  }
  int top = app.view->topline();
  int total = app.view->size();
  float frac = total > 0 ? (float)top / total : 0;

  // Measure what actually fits, so code wraps exactly at the view's edge.
  int sb = app.view->scrollbar_size() ? app.view->scrollbar_size() : Fl::scrollbar_size();
  fl_font(FL_COURIER, app.size);
  RenderMetrics m;
  m.text_w = app.view->w() - sb - 12;  // minus Fl_Help_View's own margins
  m.mono_w = fl_width("0123456789", 10) / 10.0;
  m.textsize = app.size;
  app.rendered_w = app.view->w();
  std::string html = markdown_to_html(md, dir, kThemes[app.theme], m, app.search);
  app.view->textsize(app.size);
  app.view->value(html.c_str());

  if (keep_scroll) {
    // Preserve position; if the document size changed (zoom), keep the same fraction.
    int nt = app.view->size() == total ? top : (int)(frac * app.view->size());
    app.view->topline(nt);
  }
  set_title();
  // The HTML changed, so match positions did too.
  if (!app.search.empty()) {
    find_all_matches();
    if (app.match >= 0 && app.match < (int)app.matches.size()) {
      int s = app.matches[app.match];
      select_range(app.view, s, match_end(app.view->value(), s, app.search));
    } else {
      app.match = -1;
    }
  }
}

static void save_recent() {
  Fl_Preferences r(app.prefs, "recent");
  r.clear();
  for (size_t i = 0; i < app.recent.size(); i++)
    r.set(std::to_string(i).c_str(), app.recent[i].c_str());
  app.prefs->flush();
}

static void add_recent(const std::string &p) {
  if (!app.remember || is_internal(p) || !is_file(p) || is_image(p)) return;
  app.recent.erase(std::remove(app.recent.begin(), app.recent.end(), p), app.recent.end());
  app.recent.insert(app.recent.begin(), p);
  if (app.recent.size() > (size_t)kMaxRecent) app.recent.resize(kMaxRecent);
  save_recent();
}

static void open_file(const std::string &path, const std::string &anchor, bool push) {
  std::string p = is_internal(path) ? path : absolute(path);
  if (push && !app.path.empty() && p != app.path) app.back.push_back({app.path, app.view->topline()});
  bool same = p == app.path;
  app.path = p;
  if (!same) app.match = -1;  // a search stays active, but starts over
  render(same);
  if (!anchor.empty()) app.view->topline(anchor.c_str());
  add_recent(p);
  build_menubar();
}

static void go_back() {
  if (app.back.empty()) return;
  HistEntry h = app.back.back();
  app.back.pop_back();
  app.path = h.path;
  render(false);
  app.view->topline(h.top);
  build_menubar();
}

// Open a link target: Markdown and images inside the viewer, web links in
// the browser, other local files with their default application.
static void deferred_open(void *d) {
  auto *s = (std::pair<std::string, std::string> *)d;
  open_file(s->first, s->second);
  delete s;
}

struct LinkTarget {
  enum Kind { NONE, WEB, MARKDOWN, IMAGE, OTHER } kind = NONE;
  std::string target;  // URL or absolute path
  std::string anchor;
};

static LinkTarget classify_link(const std::string &uri) {
  LinkTarget l;
  std::string u = uri;
  size_t pl = strlen(kExternalPrefix);
  if (u.compare(0, pl, kExternalPrefix) == 0) u = u.substr(pl);
  if (u.compare(0, 7, "file://") == 0) u = u.substr(7);
  else if (u.compare(0, 5, "file:") == 0) u = u.substr(5);
  if (u.empty()) return l;
  if (has_url_scheme(u)) { l.kind = LinkTarget::WEB; l.target = u; return l; }
  u = url_decode(u);  // the renderer encodes '#' as %23 (see link_href)
  size_t h = u.rfind('#');
  if (h != std::string::npos) { l.anchor = u.substr(h + 1); u = u.substr(0, h); }
  l.target = u;
  if (!is_file(u)) l.kind = LinkTarget::OTHER;
  else if (is_image(u)) l.kind = LinkTarget::IMAGE;
  else if (is_markdown(u)) l.kind = LinkTarget::MARKDOWN;
  else l.kind = LinkTarget::OTHER;
  return l;
}

static void follow(const LinkTarget &l) {
  switch (l.kind) {
    case LinkTarget::WEB: xdg_open(l.target); break;
    case LinkTarget::MARKDOWN:
    case LinkTarget::IMAGE:
      // Don't replace the document from inside the widget's own event handler.
      Fl::add_timeout(0, deferred_open, new std::pair<std::string, std::string>(l.target, l.anchor));
      break;
    case LinkTarget::OTHER: if (is_file(l.target)) xdg_open(l.target); break;
    case LinkTarget::NONE: break;
  }
}

// Schemes Fl_Help_View::load() hands to fl_open_uri() by itself. The
// renderer prefixes such links (kExternalPrefix) so this shouldn't happen,
// but if one slips through, it must not be opened a second time.
static bool fltk_opens_itself(const char *u) {
  if (!u) return false;
  for (const char *s : {"ftp:", "http:", "https:", "ipp:", "mailto:", "news:"})
    if (!strncmp(u, s, strlen(s))) return true;
  return false;
}

// Fl_Help_View calls this when a link is clicked, when we probe for a link
// (right click), AND while laying out the page (to resolve <img> sources).
// Only a real click may open anything.
static const char *link_cb(Fl_Widget *, const char *uri) {
  if (app.probing) { app.probe_uri = uri ? uri : ""; return nullptr; }
  if (app.clicking && fltk_opens_itself(uri)) return nullptr;  // don't open it twice
  std::string u = uri ? uri : "";
  if (u.compare(0, 7, "file://") == 0) u = u.substr(7);
  else if (u.compare(0, 5, "file:") == 0) u = u.substr(5);
  if (!app.clicking) {
    // Layout/image request: never launch anything. Never return NULL here
    // (that crashes Fl_Help_View::format()). Relative paths (from raw HTML
    // <img>) are resolved against the document's folder; remote URLs are
    // returned as-is and simply not shown, since the widget can't fetch them.
    static std::string resolved;
    if (!has_url_scheme(u) && !u.empty() && u[0] != '/' && !is_internal(app.path)) {
      resolved = dirname_of(app.path) + "/" + url_decode(u);
      return resolved.c_str();
    }
    return uri;
  }
  follow(classify_link(uri ? uri : ""));
  return nullptr;
}

// Auto-reload when the file changes on disk.
static void watch_cb(void *) {
  if (!is_internal(app.path)) {
    struct stat st;
    if (stat(app.path.c_str(), &st) == 0 && (st.st_mtime != app.mtime || st.st_size != app.fsize))
      render(true);
  }
  Fl::repeat_timeout(0.7, watch_cb);
}

// ---------------------------------------------------------------- actions

static void show_find() {
  if (!app.find->visible()) { app.find->show(); layout(); }
  app.find->input->take_focus();
  app.find->input->insert_position(0, app.find->input->size());
  app.win->redraw();
}

static void set_find_status(const std::string &s) {
  app.find->status->copy_label(s.c_str());
  app.find->status->redraw();
}

// Make match `i` the current one: scroll to it and select it.
static void show_match(int i, const char *note = "") {
  int n = (int)app.matches.size();
  if (n == 0) return;
  app.match = (i % n + n) % n;
  app.view->topline(app.match_y[app.match]);
  int s = app.matches[app.match];
  select_range(app.view, s, match_end(app.view->value(), s, app.search));
  set_find_status(std::to_string(app.match + 1) + " of " + std::to_string(n) + note);
}

// The search text changed: mark all matches and go to the first one at or
// below the current position (like a browser).
static void run_search() {
  std::string q = app.find->input->value();
  if (q == app.search && !app.matches.empty()) return;
  app.search = q;
  app.match = -1;
  app.view->clear_selection();
  int top = app.view->topline();
  render(true);  // re-render with matches marked; also fills app.matches
  if (q.empty()) { set_find_status(""); return; }
  if (app.matches.empty()) { set_find_status("not found"); return; }
  int first = 0;
  while (first < (int)app.match_y.size() && app.match_y[first] < top - 1) first++;
  show_match(first < (int)app.matches.size() ? first : 0);
}

static void run_search_cb(void *) { run_search(); }

// Next (+1) or previous (-1) match.
static void step_match(int dir) {
  if (std::string(app.find->input->value()) != app.search || app.matches.empty()) {
    Fl::remove_timeout(run_search_cb);
    run_search();
    return;
  }
  int n = (int)app.matches.size();
  int next = app.match + dir;
  const char *note = next >= n ? "  (from the top)" : next < 0 ? "  (from the bottom)" : "";
  show_match(next, note);
}

static void hide_find() {
  Fl::remove_timeout(run_search_cb);
  app.find->hide();
  app.search.clear();
  app.matches.clear();
  app.match_y.clear();
  app.match = -1;
  set_find_status("");
  app.view->clear_selection();
  layout();
  render(true);  // remove the marks
  app.view->take_focus();
  app.win->redraw();
}

static void find_next(int dir) {
  if (!app.find->visible() || !*app.find->input->value()) show_find();
  else step_match(dir);
}

void FindBar::search(bool enter) {
  if (enter) { step_match(Fl::event_state(FL_SHIFT) ? -1 : +1); return; }
  // Typing: wait for a short pause before re-rendering with the marks.
  Fl::remove_timeout(run_search_cb);
  Fl::add_timeout(0.15, run_search_cb);
}

static void zoom(int delta) {
  int s = delta == 0 ? kDefaultSize : app.size + delta;
  if (s < kMinSize || s > kMaxSize) return;
  app.size = s;
  apply_theme();
  layout();
  render(true);
}

static void set_theme(int t) {
  app.theme = (t + kThemeCount) % kThemeCount;
  apply_theme();
  render(true);
  build_menubar();
}

static void toggle_menubar() {
  app.show_menubar = !app.show_menubar;
  layout();
  build_menubar();
  app.win->redraw();
}

static void open_dialog() {
  Fl_Native_File_Chooser fc;
  fc.title("Open Markdown file");
  fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
  fc.filter("Markdown\t*.{md,markdown,mdown,mkd,mkdn,txt}\nAll files\t*");
  if (!is_internal(app.path)) fc.directory(dirname_of(app.path).c_str());
  if (fc.show() == 0 && fc.filename()) open_file(fc.filename());
}

static void scroll_by(int dy) {
  int t = app.view->topline() + dy;
  int max = app.view->size() - app.view->h();
  if (t > max) t = max;
  if (t < 0) t = 0;
  app.view->topline(t);
}

static void copy_selection() { if (app.view->text_selected()) app.view->copy(1); }
static void open_folder() { if (!is_internal(app.path)) xdg_open(dirname_of(app.path)); }
static void copy_path() { if (!is_internal(app.path)) copy_text(app.path); }

// ---------------------------------------------------------------- menus

// One callback for every menu item; the item's user data says what to do.
enum Action {
  A_OPEN = 1, A_RELOAD, A_FOLDER, A_COPY_PATH, A_QUIT,
  A_COPY, A_SELECT_ALL, A_FIND, A_FIND_NEXT, A_FIND_PREV,
  A_ZOOM_IN, A_ZOOM_OUT, A_ZOOM_RESET, A_THEME0, A_THEME1, A_THEME2, A_NEXT_THEME, A_MENUBAR,
  A_BACK, A_TOP, A_BOTTOM,
  A_KEYS, A_ABOUT,
  A_LINK_OPEN, A_LINK_EXTERNAL, A_LINK_COPY,
  A_RECENT0 = 100,  // A_RECENT0 + i opens recent[i]
};

static LinkTarget ctx_link;  // link under the mouse when the context menu opened

static void do_action(long a) {
  switch (a) {
    case A_OPEN: open_dialog(); break;
    case A_RELOAD: render(true); break;
    case A_FOLDER: open_folder(); break;
    case A_COPY_PATH: copy_path(); break;
    case A_QUIT: app.win->hide(); break;
    case A_COPY: copy_selection(); break;
    case A_SELECT_ALL:
      app.view->select_all();
      app.view->redraw();  // select_all() itself doesn't repaint
      break;
    case A_FIND: show_find(); break;
    case A_FIND_NEXT: find_next(+1); break;
    case A_FIND_PREV: find_next(-1); break;
    case A_ZOOM_IN: zoom(+1); break;
    case A_ZOOM_OUT: zoom(-1); break;
    case A_ZOOM_RESET: zoom(0); break;
    case A_THEME0: case A_THEME1: case A_THEME2: set_theme((int)(a - A_THEME0)); break;
    case A_NEXT_THEME: set_theme(app.theme + 1); break;
    case A_MENUBAR: toggle_menubar(); break;
    case A_BACK: go_back(); break;
    case A_TOP: scroll_by(-INT_MAX / 2); break;
    case A_BOTTOM: scroll_by(INT_MAX / 2); break;
    case A_KEYS: open_file("flmd:keys"); break;
    case A_ABOUT: open_file("flmd:about"); break;
    case A_LINK_OPEN: follow(ctx_link); break;
    case A_LINK_EXTERNAL: xdg_open(ctx_link.target); break;
    case A_LINK_COPY: copy_text(ctx_link.target); break;
    default:
      if (a >= A_RECENT0 && a < A_RECENT0 + (long)app.recent.size()) {
        std::string p = app.recent[a - A_RECENT0];
        open_file(p);
      }
  }
}

static void menu_cb(Fl_Widget *, void *d) { do_action((long)(fl_intptr_t)d); }

// Fl_Menu_::add() treats '/', '\\', '&' and a leading '_' specially.
static std::string menu_escape(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '/' || c == '\\') o += '\\';
    if (c == '&') o += '&';
    o += c;
  }
  return o;
}

static void build_menubar() {
  Fl_Menu_Bar *m = app.menubar;
  if (!m) return;
  bool file = !is_internal(app.path);
  int inact = FL_MENU_INACTIVE;
  auto add = [&](const char *label, int key, long action, int flags = 0) {
    m->add(label, key, menu_cb, (void *)(fl_intptr_t)action, flags);
  };
  m->clear();
  add("&File/&Open...", FL_CTRL + 'o', A_OPEN);
  if (app.recent.empty()) {
    add("&File/Open &Recent/(empty)", 0, 0, inact);
  } else {
    for (size_t i = 0; i < app.recent.size(); i++) {
      std::string lbl = "&File/Open &Recent/" + std::to_string(i + 1) + "   " +
                        menu_escape(basename_of(app.recent[i]) + "   " + tilde(dirname_of(app.recent[i])));
      add(lbl.c_str(), 0, A_RECENT0 + (long)i);
    }
  }
  add("&File/Re&load", FL_F + 5, A_RELOAD, file ? FL_MENU_DIVIDER : FL_MENU_DIVIDER | inact);
  add("&File/Open Containing &Folder", 0, A_FOLDER, file ? 0 : inact);
  add("&File/&Copy File Path", 0, A_COPY_PATH, (file ? 0 : inact) | FL_MENU_DIVIDER);
  add("&File/&Quit", FL_CTRL + 'q', A_QUIT);

  add("&Edit/&Copy", FL_CTRL + 'c', A_COPY);
  add("&Edit/Select &All", FL_CTRL + 'a', A_SELECT_ALL, FL_MENU_DIVIDER);
  add("&Edit/&Find...", FL_CTRL + 'f', A_FIND);
  add("&Edit/Find &Next", FL_F + 3, A_FIND_NEXT);
  add("&Edit/Find &Previous", FL_SHIFT + FL_F + 3, A_FIND_PREV);

  add("&View/Zoom &In", FL_CTRL + '=', A_ZOOM_IN);
  add("&View/Zoom &Out", FL_CTRL + '-', A_ZOOM_OUT);
  add("&View/&Actual Size", FL_CTRL + '0', A_ZOOM_RESET, FL_MENU_DIVIDER);
  for (int t = 0; t < kThemeCount; t++) {
    std::string lbl = std::string("&View/") + kThemes[t].name + " Theme";
    add(lbl.c_str(), 0, A_THEME0 + t,
        FL_MENU_RADIO | (t == app.theme ? FL_MENU_VALUE : 0) | (t == kThemeCount - 1 ? 0 : 0));
  }
  add("&View/Next &Theme", FL_CTRL + 't', A_NEXT_THEME, FL_MENU_DIVIDER);
  add("&View/Show &Menu Bar   ", FL_CTRL + 'm', A_MENUBAR,
      FL_MENU_TOGGLE | (app.show_menubar ? FL_MENU_VALUE : 0));

  add("&Go/&Back", FL_ALT + FL_Left, A_BACK, app.back.empty() ? inact : 0);
  add("&Go/&Top", FL_Home, A_TOP);
  add("&Go/B&ottom", FL_End, A_BOTTOM);

  add("&Help/&Keyboard Shortcuts", FL_F + 1, A_KEYS);
  add("&Help/&About flmarkdown", 0, A_ABOUT);
  m->redraw();
}

// Right-click menu items, built fresh each time from what's under the mouse.
static std::vector<Fl_Menu_Item> context_items(const std::string &link) {
  ctx_link = classify_link(link);
  bool file = !is_internal(app.path);
  bool sel = app.view->text_selected() != 0;
  std::vector<Fl_Menu_Item> items;
  auto item = [&](const char *label, int key, long action, int flags = 0) {
    Fl_Menu_Item it = {};
    it.text = label;
    it.shortcut_ = key;
    it.callback_ = menu_cb;
    it.user_data_ = (void *)(fl_intptr_t)action;
    it.flags = flags;
    items.push_back(it);
  };
  auto end_menu = [&] { items.push_back(Fl_Menu_Item{}); };
  int inact = FL_MENU_INACTIVE, div = FL_MENU_DIVIDER;

  switch (ctx_link.kind) {
    case LinkTarget::WEB:
      item("Open Link in Browser", 0, A_LINK_OPEN);
      item("Copy Link Address", 0, A_LINK_COPY, div);
      break;
    case LinkTarget::MARKDOWN:
      item("Open Link", 0, A_LINK_OPEN);
      item("Copy Link Address", 0, A_LINK_COPY, div);
      break;
    case LinkTarget::IMAGE:
      item("View Image", 0, A_LINK_OPEN);
      item("Open Image in Image Viewer", 0, A_LINK_EXTERNAL);
      item("Copy Image Path", 0, A_LINK_COPY, div);
      break;
    case LinkTarget::OTHER:
      item("Open with Default App", 0, A_LINK_OPEN, is_file(ctx_link.target) ? 0 : inact);
      item("Copy Link Address", 0, A_LINK_COPY, div);
      break;
    case LinkTarget::NONE: break;
  }
  item("Copy", FL_CTRL + 'c', A_COPY, sel ? 0 : inact);
  item("Select All", FL_CTRL + 'a', A_SELECT_ALL);
  item("Find...", FL_CTRL + 'f', A_FIND, div);
  item("Back", FL_ALT + FL_Left, A_BACK, app.back.empty() ? inact : 0);
  item("Reload", FL_F + 5, A_RELOAD, file ? div : div | inact);
  item("Zoom In", FL_CTRL + '=', A_ZOOM_IN);
  item("Zoom Out", FL_CTRL + '-', A_ZOOM_OUT);
  item("Theme", 0, 0, FL_SUBMENU | div);
  for (int t = 0; t < kThemeCount; t++)
    item(kThemes[t].name, 0, A_THEME0 + t, FL_MENU_RADIO | (t == app.theme ? FL_MENU_VALUE : 0));
  end_menu();
  item("Open File...", FL_CTRL + 'o', A_OPEN);
  item("Open Containing Folder", 0, A_FOLDER, file ? 0 : inact);
  item("Copy File Path", 0, A_COPY_PATH, (file ? 0 : inact) | div);
  item("Show Menu Bar   ", FL_CTRL + 'm', A_MENUBAR, FL_MENU_TOGGLE | (app.show_menubar ? FL_MENU_VALUE : 0));
  end_menu();

  for (Fl_Menu_Item &it : items) {
    if (!it.text) continue;
    it.labelfont_ = FL_HELVETICA;
    it.labelsize_ = kUiSize;
    it.labelcolor_ = rgb(kThemes[app.theme].fg);
  }
  return items;
}

static void show_context_menu(const std::string &link) {
  std::vector<Fl_Menu_Item> items = context_items(link);
  const Fl_Menu_Item *r =
      items[0].popup(Fl::event_x(), Fl::event_y(), nullptr, nullptr, app.popup_style);
  if (r && r->callback()) r->do_callback(app.view);
}

int MainWin::handle(int e) {
  if (e == FL_MOUSEWHEEL) {
    // Scroll even when the pointer is in the side margins.
    if (Fl_Double_Window::handle(e)) return 1;
    scroll_by(Fl::event_dy() * app.size * 3);
    return 1;
  }
  if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && !Fl_Double_Window::handle(e)) {
    show_context_menu("");  // right click in the margins
    return 1;
  }
  if (e != FL_SHORTCUT) return Fl_Double_Window::handle(e);

  // All keyboard shortcuts are handled here (the menus only display them),
  // so they keep working when the menu bar is hidden.
  int k = Fl::event_key();
  int st = Fl::event_state();
  bool ctrl = st & FL_CTRL, alt = st & FL_ALT;
  bool typing = Fl::focus() == app.find->input;
  int line = app.size * 2;
  int page = app.view->h() - line * 2;
  const char *txt = Fl::event_text();

  if (ctrl) {
    switch (k) {
      case 'o': do_action(A_OPEN); return 1;
      case 'f': do_action(A_FIND); return 1;
      case 'r': do_action(A_RELOAD); return 1;
      case 't': do_action(A_NEXT_THEME); return 1;
      case 'm': do_action(A_MENUBAR); return 1;
      case 'q': case 'w': do_action(A_QUIT); return 1;
      case '=': case '+': case FL_KP + '+': do_action(A_ZOOM_IN); return 1;
      case '-': case FL_KP + '-': do_action(A_ZOOM_OUT); return 1;
      case '0': case FL_KP + '0': do_action(A_ZOOM_RESET); return 1;
      case 'c': if (!typing) { do_action(A_COPY); return 1; } break;
      case 'a': if (!typing) { do_action(A_SELECT_ALL); return 1; } break;
    }
  }
  if (alt && k == FL_Left) { do_action(A_BACK); return 1; }
  switch (k) {
    case FL_Escape:
      if (app.find->visible()) hide_find();
      return 1;  // never close the window on Escape
    case FL_F + 1: do_action(A_KEYS); return 1;
    case FL_F + 3: do_action(st & FL_SHIFT ? A_FIND_PREV : A_FIND_NEXT); return 1;
    case FL_F + 5: do_action(A_RELOAD); return 1;
  }
  if (typing) return 0;
  switch (k) {
    case FL_BackSpace: do_action(A_BACK); return 1;
    case FL_Down: scroll_by(line); return 1;
    case FL_Up: scroll_by(-line); return 1;
    case FL_Page_Down: scroll_by(page); return 1;
    case FL_Page_Up: scroll_by(-page); return 1;
    case FL_Home: do_action(A_TOP); return 1;
    case FL_End: do_action(A_BOTTOM); return 1;
    case ' ': scroll_by(st & FL_SHIFT ? -page : page); return 1;
  }
  if (!ctrl && !alt && txt && txt[0] && !txt[1]) {
    switch (txt[0]) {
      case 'j': scroll_by(line); return 1;
      case 'k': scroll_by(-line); return 1;
      case 'g': do_action(A_TOP); return 1;
      case 'G': do_action(A_BOTTOM); return 1;
      case 'q': do_action(A_QUIT); return 1;
      case '/': do_action(A_FIND); return 1;
      case '+': do_action(A_ZOOM_IN); return 1;
      case '-': do_action(A_ZOOM_OUT); return 1;
    }
  }
  // Ignore Alt+letter etc. here; the menu bar uses them to open its menus.
  return Fl_Double_Window::handle(e);
}

// ---------------------------------------------------------------- fonts

static void setup_fonts() {
  // Comfortable UI/reading font and a matching monospace. Pango falls back
  // gracefully if a family is missing. Override with FLMD_FONT / FLMD_MONO.
  const char *sans = getenv("FLMD_FONT");
  const char *mono = getenv("FLMD_MONO");
  std::string s = sans && *sans ? sans : "Noto Sans";
  std::string m = mono && *mono ? mono : "DejaVu Sans Mono";
  static std::string names[8];
  names[0] = s; names[1] = s + " Bold"; names[2] = s + " Italic"; names[3] = s + " Bold Italic";
  names[4] = m; names[5] = m + " Bold"; names[6] = m + " Italic"; names[7] = m + " Bold Italic";
  Fl::set_font(FL_HELVETICA, names[0].c_str());
  Fl::set_font(FL_HELVETICA_BOLD, names[1].c_str());
  Fl::set_font(FL_HELVETICA_ITALIC, names[2].c_str());
  Fl::set_font(FL_HELVETICA_BOLD_ITALIC, names[3].c_str());
  Fl::set_font(FL_COURIER, names[4].c_str());
  Fl::set_font(FL_COURIER_BOLD, names[5].c_str());
  Fl::set_font(FL_COURIER_ITALIC, names[6].c_str());
  Fl::set_font(FL_COURIER_BOLD_ITALIC, names[7].c_str());
}

// ---------------------------------------------------------------- main

static void close_cb(Fl_Widget *w, void *) {
  // Escape is handled in MainWin::handle; only real close requests get here.
  if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
  w->hide();
}

#ifdef FLMD_SELFTEST
#include "../tests/selftest.inc"  // built only by "make test"
#endif

int main(int argc, char **argv) {
  signal(SIGCHLD, SIG_IGN);  // reap xdg-open children automatically
  fl_register_images();
  Fl::scheme("none");
  Fl::visible_focus(0);
  Fl::scrollbar_size(10);
  Fl::menu_linespacing(8);
  Fl::set_boxtype(kMenuBox, draw_menu_box, 1, 1, 2, 2);
  Fl::set_boxtype(kBarBox, draw_bar_box, 0, 0, 0, 1);
  fl_open_display();  // needed to measure fonts before the first render
  setup_fonts();

  static Fl_Preferences prefs(Fl_Preferences::USER_L, "flmarkdown", "flmarkdown");
  app.prefs = &prefs;
  int W, H, mb;
  prefs.get("theme", app.theme, 0);
  prefs.get("textsize", app.size, kDefaultSize);
  prefs.get("width", W, 960);
  prefs.get("height", H, 900);
  prefs.get("menubar", mb, 1);
  app.show_menubar = mb != 0;
  if (app.theme < 0 || app.theme >= kThemeCount) app.theme = 0;
  if (app.size < kMinSize || app.size > kMaxSize) app.size = kDefaultSize;
  {
    Fl_Preferences r(prefs, "recent");
    for (int i = 0; i < kMaxRecent; i++) {
      char *v = nullptr;
      if (r.get(std::to_string(i).c_str(), v, "") && v && *v) app.recent.push_back(v);
      free(v);
    }
  }

  const char *file = nullptr;
  bool check_only = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("usage: %s [--theme night|paper|sepia] [file.md]\n", argv[0]);
      return 0;
    }
    if (!strcmp(argv[i], "--version")) { printf("flmarkdown %s\n", FLMD_VERSION); return 0; }
    if (!strcmp(argv[i], "--check")) { check_only = true; app.remember = false; continue; }  // layout test, no window
    if (!strcmp(argv[i], "--theme") && i + 1 < argc) {
      ++i;
      for (int t = 0; t < kThemeCount; t++)
        if (!strcasecmp(argv[i], kThemes[t].name)) app.theme = t;
      continue;
    }
    file = argv[i];
  }
#ifdef FLMD_SELFTEST
  app.remember = false;  // the self-test must not touch Open Recent
#endif
  app.win = new MainWin(W, H);
  app.win->size_range(360, 240);
  app.menubar = new Fl_Menu_Bar(0, 0, W, kMenuH);
  app.view = new View(0, 0, W, H);
  app.view->box(FL_FLAT_BOX);
  app.view->textfont(FL_HELVETICA);
  app.view->link(link_cb);
  app.find = new FindBar(0, H - kFindH, W, kFindH);
  app.find->hide();
  app.win->end();
  app.popup_style = new Fl_Menu_Button(0, 0, 0, 0);  // not in the window; style only
  app.win->resizable(app.win);
  app.win->callback(close_cb);

  apply_theme();
  layout();

  if (file) open_file(file);
  else open_file("flmd:welcome");
  if (check_only) {
    // Rendering finished without hanging. Exit 3 if anything overflowed sideways.
    if (app.view->hscrollbar()->visible()) { fprintf(stderr, "overflow: %s\n", file ? file : ""); return 3; }
    return 0;
  }

  // Window/taskbar icon (embedded, so it works even when not installed).
  static Fl_PNG_Image icon(nullptr, icon_png, icon_png_len);
  app.win->icon(&icon);
  app.win->xclass("flmarkdown");  // matches StartupWMClass in the .desktop file
  app.win->show();
  app.view->take_focus();
  Fl::add_timeout(0.7, watch_cb);
#ifdef FLMD_SELFTEST
  selftest_start();
#endif
  int r = Fl::run();

  prefs.set("theme", app.theme);
  prefs.set("textsize", app.size);
  prefs.set("width", app.win->w());
  prefs.set("height", app.win->h());
  prefs.set("menubar", app.show_menubar ? 1 : 0);
  return r;
}
