// tests/selftest.inc - runs every menu action inside the real app and
// checks what it did. Built and run by "make test"; never part of the
// installed program. It needs an X11/Wayland display (a window opens
// briefly) and exits before preferences are saved, so it doesn't touch
// your settings or recent files.
//
// xdg-open is replaced by tests/fakebin/xdg-open (via PATH), which only
// appends its argument to $FLMD_XDG_LOG, so nothing is launched.

#include <functional>

namespace selftest {

std::vector<std::function<void()>> steps;
size_t next_step = 0;
double delay = 0.05;  // pause before the next step (some actions are deferred)
int passed = 0, failed = 0;
std::string doc;      // absolute path of the test document

void check(bool ok, const std::string &what) {
  printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
  ok ? passed++ : failed++;
}

// Receives the clipboard via Fl::paste().
struct Clipboard : Fl_Widget {
  std::string text;
  Clipboard() : Fl_Widget(0, 0, 0, 0) {}
  void draw() override {}
  int handle(int e) override {
    if (e != FL_PASTE) return 0;
    text = Fl::event_text() ? Fl::event_text() : "";
    return 1;
  }
};
Clipboard *clip;

std::string clipboard() {
  clip->text.clear();
  Fl::paste(*clip, 1);  // we own the clipboard, so this is delivered at once
  return clip->text;
}

std::string xdg_log() {
  const char *f = getenv("FLMD_XDG_LOG");
  std::ifstream in(f ? f : "/dev/null");
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool html_has(const std::string &s) { return strstr(app.view->value(), s.c_str()) != nullptr; }

std::string hexcolor(unsigned c) {
  char b[8];
  snprintf(b, sizeof b, "#%06x", c);
  return b;
}

// Every active item in a menu must lead to an action.
void check_menu(const Fl_Menu_Item *m, const std::string &name) {
  int items = 0, bad = 0;
  for (; m && (m->text || m->next() != m); m++) {
    if (!m->text) continue;                       // end of a submenu
    if (m->submenu() || !m->active()) continue;   // submenu title / greyed out
    items++;
    if (m->callback() != menu_cb || !(fl_intptr_t)m->user_data()) {
      bad++;
      printf("      no action: %s\n", m->text);
    }
  }
  check(items > 0 && bad == 0, name + ": all " + std::to_string(items) + " active items have an action");
}

void build() {
  auto step = [](std::function<void()> f) { steps.push_back(f); };
  auto wait = [](double s) { steps.push_back([s] { delay = s; }); };

  // ---- menus are wired up
  step([] {
    zoom(0);
    const Fl_Menu_Item *m = app.menubar->menu();
    int n = 0;
    for (const Fl_Menu_Item *p = m; p && p->text; p = p->next()) n++;
    check(n == 5, "menu bar has File, Edit, View, Go, Help");
    // walk the whole menu bar array (submenus are inline)
    int items = 0, bad = 0;
    for (int i = 0; i < app.menubar->size(); i++) {
      const Fl_Menu_Item &it = m[i];
      if (!it.text || it.submenu() || !it.active()) continue;
      items++;
      if (it.callback() != menu_cb || !(fl_intptr_t)it.user_data()) { bad++; printf("      no action: %s\n", it.text); }
    }
    check(items >= 20 && bad == 0, "menu bar: all " + std::to_string(items) + " active items have an action");
    for (const char *l : {"", "x-flmd-ext:https://example.com/", "/no/such/file.pdf"}) {
      std::vector<Fl_Menu_Item> c = context_items(l);
      check_menu(c.data(), std::string("right-click menu (") + (*l ? l : "no link") + ")");
    }
  });

  // ---- Edit
  step([] {
    app.view->clear_selection();
    check(!app.view->text_selected(), "nothing selected at start");
    do_action(A_SELECT_ALL);
    check(app.view->text_selected(), "Edit > Select All selects the text");
    do_action(A_COPY);
    std::string c = clipboard();
    check(c.find("calm, lightweight Markdown viewer") != std::string::npos &&
              c.find("License") != std::string::npos,
          "Edit > Copy after Select All copies the whole document (" + std::to_string(c.size()) + " bytes)");
    app.view->clear_selection();
  });

  // ---- Double-click selects a word
  step([] {
    std::ifstream in(doc);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string src = ss.str();
    int words = 0, bad = 0, code = 0, code_bad = 0;
    std::string bad_list;
    for (int top = 0; top < app.view->size(); top += app.view->h() - 40) {
      app.view->topline(top);
      app.view->redraw();
      Fl::flush();
      for (int y = app.view->y() + 20; y < app.view->y() + app.view->h() - 20; y += 9) {
        for (int x = app.view->x() + 20; x < app.view->x() + app.view->w() - 30; x += 37) {
          // A double-click as FLTK delivers it. The release is not a "click",
          // so a link under the pointer is not followed.
          Fl::e_x = x, Fl::e_y = y, Fl::e_x_root = x, Fl::e_y_root = y;
          Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
          Fl::e_clicks = 1, Fl::e_is_click = 1;
          app.view->handle(FL_PUSH);
          Fl::e_is_click = 0;
          app.view->handle(FL_RELEASE);
          Fl::e_clicks = 0;
          if (!app.view->text_selected()) continue;
          do_action(A_COPY);
          std::string w = clipboard();
          bool pre = inside_pre(app.view->value(), app.word.first);
          bool ok = !w.empty() && w == app.word.text && w == trim(w) &&
                    w.find("&amp;") == std::string::npos && w.find("&nbsp;") == std::string::npos &&
                    (pre || w.find(' ') == std::string::npos) && src.find(w) != std::string::npos;
          words++;
          if (pre) { code++; if (!ok) code_bad++; }
          if (!ok) { bad++; if (bad_list.size() < 200) bad_list += " [" + w + "]"; }
          app.view->clear_selection();
        }
      }
    }
    app.view->topline(0);
    check(words >= 30 && bad == 0,
          "double-click selects and copies a single word (" + std::to_string(words) + " tried)" + bad_list);
    check(code > 0 && code_bad == 0,
          "double-click in a code block copies without a leading newline (" + std::to_string(code) + " tried)");
    const char *h = "<p>R&amp;D&nbsp;lab, x</p><pre>\n  a &lt; b\n</pre>";
    int w1 = 3, w2 = (int)(strstr(h, "\n  a") - h);
    check(plain_text(h + w1, h + piece_end(h, w1, inside_pre(h, w1))) == "R&D lab," &&
              trim(plain_text(h + w2, h + piece_end(h, w2, inside_pre(h, w2)))) == "a < b",
          "a double-clicked word keeps its entities whole (\"R&D lab,\", \"a < b\")");
  });

  // ---- Find
  step([] {
    do_action(A_FIND);
    check(app.find->visible() && Fl::focus() == app.find->input, "Edit > Find opens the find bar with focus");
    app.find->input->value("theme");
    run_search();
    int n = (int)app.matches.size();
    check(n >= 3, "search finds all matches (" + std::to_string(n) + ")");
    check(html_has(hexcolor(kThemes[app.theme].mark)), "all matches are marked on the page");
    check(app.match == 0 && app.view->text_selected(), "first match is current and selected");
    check(std::string(app.find->status->label()) == "1 of " + std::to_string(n), "status shows \"1 of N\"");
    int top0 = app.view->topline();
    do_action(A_FIND_NEXT);
    check(app.match == 1 && std::string(app.find->status->label()).rfind("2 of ", 0) == 0,
          "Edit > Find Next goes to match 2");
    check(app.view->topline() >= top0, "Find Next scrolls forward");
    do_action(A_FIND_PREV);
    check(app.match == 0, "Edit > Find Previous goes back to match 1");
    do_action(A_FIND_PREV);
    check(app.match == n - 1 && strstr(app.find->status->label(), "from the bottom"),
          "Find Previous wraps to the last match");
    do_action(A_ZOOM_IN);
    check(app.match == n - 1 && app.view->text_selected() && (int)app.matches.size() == n,
          "search survives a re-render (zoom)");
    do_action(A_ZOOM_RESET);
    app.find->input->value("zzqxjv");
    run_search();
    check(app.matches.empty() && std::string(app.find->status->label()) == "not found", "no match shows \"not found\"");
    app.find->input->value("theme");
    run_search();
    hide_find();
    check(!app.find->visible() && app.search.empty() && !html_has(hexcolor(kThemes[app.theme].mark)),
          "Esc closes the find bar and removes the marks");
  });

  // ---- View
  step([] {
    do_action(A_ZOOM_IN);
    check(app.size == kDefaultSize + 1, "View > Zoom In");
    do_action(A_ZOOM_OUT);
    do_action(A_ZOOM_OUT);
    check(app.size == kDefaultSize - 1, "View > Zoom Out");
    do_action(A_ZOOM_IN); do_action(A_ZOOM_IN); do_action(A_ZOOM_IN);
    do_action(A_ZOOM_RESET);
    check(app.size == kDefaultSize, "View > Actual Size");
    int t0 = app.theme;
    for (int t = 0; t < kThemeCount; t++) {
      do_action(A_THEME0 + t);
      check(app.theme == t && html_has(hexcolor(kThemes[t].bg)), std::string("View > ") + kThemes[t].name + " Theme");
    }
    do_action(A_NEXT_THEME);
    check(app.theme == 0, "View > Next Theme cycles");
    do_action(A_THEME0 + t0);
    do_action(A_MENUBAR);
    check(!app.menubar->visible() && app.view->y() < kMenuH, "View > Show Menu Bar hides it");
    do_action(A_MENUBAR);
    check(app.menubar->visible() && app.view->y() > kMenuH, "...and shows it again");
  });

  // ---- Go
  step([] {
    do_action(A_BOTTOM);
    int max = app.view->size() - app.view->h();
    check(max > 0 && app.view->topline() == max, "Go > Bottom");
    do_action(A_TOP);
    check(app.view->topline() == 0, "Go > Top");
    do_action(A_KEYS);
    check(app.path == "flmd:keys", "Help > Keyboard Shortcuts");
    do_action(A_ABOUT);
    check(app.path == "flmd:about" && html_has(FLMD_VERSION), "Help > About shows the version");
    do_action(A_BACK);
    check(app.path == "flmd:keys", "Go > Back");
    do_action(A_BACK);
    check(app.path == doc, "Go > Back again returns to the document");
  });

  // ---- File
  step([] {
    do_action(A_RELOAD);
    check(app.path == doc && app.view->size() > 0, "File > Reload");
    do_action(A_COPY_PATH);
    check(clipboard() == doc, "File > Copy File Path");
    do_action(A_FOLDER);
    app.recent = {doc};
    build_menubar();
    do_action(A_KEYS);
    do_action(A_RECENT0);
    check(app.path == doc, "File > Open Recent");
  });
  wait(0.5);  // xdg-open runs asynchronously
  step([] {
    check(xdg_log().find(dirname_of(doc) + "\n") != std::string::npos, "File > Open Containing Folder (xdg-open folder)");
  });

  // ---- right-click link actions
  step([] {
    context_items("x-flmd-ext:https://example.com/page");
    do_action(A_LINK_COPY);
    check(clipboard() == "https://example.com/page", "Copy Link Address");
    do_action(A_LINK_OPEN);
    std::string img = dirname_of(doc) + "/docs/screenshots/night.png";
    context_items(img);
    check(ctx_link.kind == LinkTarget::IMAGE, "an image link is recognized");
    do_action(A_LINK_COPY);
    check(clipboard() == img, "Copy Image Path");
    do_action(A_LINK_EXTERNAL);
    do_action(A_LINK_OPEN);  // View Image (deferred)
  });
  wait(0.5);
  step([] {
    std::string log = xdg_log();
    check(log.find("https://example.com/page\n") != std::string::npos, "Open Link in Browser (xdg-open, once)");
    check(log.find("night.png\n") != std::string::npos, "Open Image in Image Viewer (xdg-open)");
    check(is_image(app.path) && app.path.find("night.png") != std::string::npos, "View Image opens it in the viewer");
    do_action(A_BACK);
    context_items(dirname_of(doc) + "/docs/sample.md%23code");
    do_action(A_LINK_OPEN);  // Open Link (deferred)
  });
  step([] {
    check(app.path == dirname_of(doc) + "/docs/sample.md" && app.view->topline() > 0,
          "Open Link opens a Markdown file at its #section");
    int opens = 0;
    std::string log = xdg_log();
    for (size_t p = 0; (p = log.find("https://example.com/page", p)) != std::string::npos; p++) opens++;
    check(opens == 1, "web links are opened exactly once");
  });

  // ---- File > Quit (last)
  step([] {
    printf("skip  File > Open... (native file dialog, needs a person)\n");
    do_action(A_QUIT);
    check(!app.win->shown(), "File > Quit closes the window");
    printf("\n%d passed, %d failed\n", passed, failed);
    exit(failed ? 1 : 0);  // before main() would save preferences
  });
}

void run(void *) {
  if (next_step >= steps.size()) return;
  delay = 0.05;
  steps[next_step++]();
  Fl::add_timeout(delay, run);
}

}  // namespace selftest

static void selftest_start() {
  app.remember = false;  // keep test files out of Open Recent
  selftest::doc = app.path;
  Fl_Group::current(nullptr);
  selftest::clip = new selftest::Clipboard;
  selftest::build();
  Fl::add_timeout(0.5, selftest::run);
}
