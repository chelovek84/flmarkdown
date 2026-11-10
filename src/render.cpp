// render.cpp - Markdown -> HTML for Fl_Help_View, using md4c's parser API.
//
// Fl_Help_View understands only a small HTML 2.0 subset with no CSS, so
// instead of md4c-html we emit HTML shaped for it: shaded tables stand in
// for code-block and quote backgrounds, headings get GitHub-style anchors,
// strikethrough uses combining overlays, task items get checkbox glyphs.

#include "render.h"

#include <md4c.h>
#include <FL/Fl_Shared_Image.H>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string hex(unsigned c) {
  char b[8];
  snprintf(b, sizeof b, "#%06x", c & 0xffffff);
  return b;
}

void append_utf8(std::string &o, unsigned cp) {
  if (cp == 0 || cp > 0x10ffff) cp = 0xfffd;
  if (cp < 0x80) o += char(cp);
  else if (cp < 0x800) { o += char(0xc0 | (cp >> 6)); o += char(0x80 | (cp & 0x3f)); }
  else if (cp < 0x10000) {
    o += char(0xe0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 0x3f)); o += char(0x80 | (cp & 0x3f));
  } else {
    o += char(0xf0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 0x3f));
    o += char(0x80 | ((cp >> 6) & 0x3f)); o += char(0x80 | (cp & 0x3f));
  }
}

void escape(std::string &o, const char *s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    switch (s[i]) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '"': o += "&quot;"; break;
      default: o += s[i];
    }
  }
}

// Decode an entity like "&mdash;" or "&#8212;" into UTF-8.
void decode_entity(std::string &o, const char *s, size_t n) {
  std::string e(s, n);
  if (e.size() > 3 && e[1] == '#') {
    unsigned cp = (e[2] == 'x' || e[2] == 'X') ? strtoul(e.c_str() + 3, nullptr, 16)
                                               : strtoul(e.c_str() + 2, nullptr, 10);
    if (cp == '<' || cp == '>' || cp == '&' || cp == '"') { char c = char(cp); escape(o, &c, 1); }
    else append_utf8(o, cp);
    return;
  }
  static const struct { const char *n; unsigned cp; } tbl[] = {
    {"&nbsp;", 0xa0}, {"&copy;", 0xa9}, {"&reg;", 0xae}, {"&trade;", 0x2122},
    {"&mdash;", 0x2014}, {"&ndash;", 0x2013}, {"&hellip;", 0x2026}, {"&bull;", 0x2022},
    {"&middot;", 0xb7}, {"&laquo;", 0xab}, {"&raquo;", 0xbb}, {"&lsquo;", 0x2018},
    {"&rsquo;", 0x2019}, {"&ldquo;", 0x201c}, {"&rdquo;", 0x201d}, {"&larr;", 0x2190},
    {"&rarr;", 0x2192}, {"&uarr;", 0x2191}, {"&darr;", 0x2193}, {"&harr;", 0x2194},
    {"&times;", 0xd7}, {"&divide;", 0xf7}, {"&plusmn;", 0xb1}, {"&deg;", 0xb0},
    {"&euro;", 0x20ac}, {"&pound;", 0xa3}, {"&yen;", 0xa5}, {"&sect;", 0xa7},
    {"&para;", 0xb6}, {"&check;", 0x2713}, {"&le;", 0x2264}, {"&ge;", 0x2265},
    {"&ne;", 0x2260}, {"&infin;", 0x221e}, {"&hearts;", 0x2665}, {"&star;", 0x2606},
  };
  for (auto &x : tbl) if (e == x.n) { append_utf8(o, x.cp); return; }
  if (e == "&amp;" || e == "&lt;" || e == "&gt;" || e == "&quot;") { o += e; return; }
  escape(o, s, n);  // unknown: show literally
}

std::string attr(const MD_ATTRIBUTE &a) {
  std::string s;
  for (unsigned i = 0; a.substr_offsets[i] < a.size; i++) {
    const char *p = a.text + a.substr_offsets[i];
    size_t n = a.substr_offsets[i + 1] - a.substr_offsets[i];
    switch (a.substr_types[i]) {
      case MD_TEXT_NULLCHAR: append_utf8(s, 0xfffd); break;
      case MD_TEXT_ENTITY: { std::string d; decode_entity(d, p, n); s += d; break; }
      default: s.append(p, n);
    }
  }
  return s;
}

// GitHub-style heading slug: lowercase, spaces->'-', drop punctuation.
std::string slugify(const std::string &text) {
  std::string s;
  for (unsigned char c : text) {
    if (c >= 0x80 || isalnum(c) || c == '-' || c == '_') s += char(c >= 0x80 ? c : tolower(c));
    else if (c == ' ') s += '-';
  }
  return s;
}

struct R {
  std::string out;
  std::string base;
  const Theme *t;
  int max_img_w;
  RenderMetrics m;

  // Pixel width for a top-level box/table so that it has the same margin on
  // both sides. Fl_Help_View (format()) puts text 4px from the left, starts
  // table cells another textsize+3 px in, and adds 6px between columns; the
  // page is text_w + 12 px wide (see main.cpp). A percentage width ignores
  // the gaps, so multi-column tables would stick out on the right.
  std::string box_width(int columns) const {
    int w = m.text_w + 12 - 4 - 2 * (m.textsize + 3) - 6 * (columns - 1);
    return std::to_string(std::max(w, 40));
  }

  // How many monospace columns fit for a code block at the current nesting.
  size_t code_cols(bool boxed) const {
    double px = m.text_w;
    if (boxed) px -= 2 * m.textsize + 24;          // box inset + cell padding
    px -= li_depth * 4.0 * m.textsize;             // list indentation per level
    if (quote_boxed.size()) px -= m.textsize;      // quote cell padding
    int cols = (int)(px / m.mono_w) - (boxed ? 2 : 0) - 1;  // 2 = left pad
    return cols < 8 ? 8 : (size_t)cols;
  }

  // Fl_Help_View (FLTK 1.4.4) loops forever laying out a <table> inside an
  // <li>, and can't nest tables either. So shaded boxes and real tables are
  // only used at top level; elsewhere we fall back to table-free markup.
  int box_depth = 0;          // nesting of table-based boxes
  int li_depth = 0;           // nesting of list items
  bool can_box() const { return box_depth == 0 && li_depth == 0; }

  // Markdown table that can't be a real <table>: collected, then printed
  // as aligned monospace text.
  bool text_table = false;
  std::vector<std::vector<std::string>> trows;
  std::vector<int> talign;
  size_t thead_rows = 0;
  std::vector<bool> quote_boxed;
  int quote_paras = 0;
  bool code_boxed = false;
  bool code_flow = false;     // code inside a list: no <pre> (see emit_code)
  bool in_code_block = false;
  std::string code_buf;       // raw text of the current code block

  size_t heading_pos = 0;     // where the current heading started in `out`
  bool in_heading = false;
  std::string heading_text;

  int body_row = 0;           // for zebra striping
  bool in_thead = false;

  int img_depth = 0;          // inside ![alt](src): collect alt text
  std::string img_alt;

  int del_depth = 0;          // strikethrough via U+0336

  std::string resolve(const std::string &href) const {
    if (href.empty() || href[0] == '#' || href[0] == '/' || has_url_scheme(href)) return href;
    return base + "/" + href;
  }

  // href for a clickable link (see kExternalPrefix in render.h).
  std::string link_href(const std::string &href) const {
    if (href.empty() || href[0] == '#') return href;  // in-page anchor
    std::string h = href;
    if (h.compare(0, 7, "file://") == 0) h = h.substr(7);
    else if (h.compare(0, 5, "file:") == 0) h = h.substr(5);
    if (has_url_scheme(h)) return kExternalPrefix + h;
    // Fl_Help_View would split "#section" off before our handler sees it.
    std::string o;
    for (char c : resolve(h)) o += c == '#' ? std::string("%23") : std::string(1, c);
    return o;
  }

  // Same treatment for href="http..." inside raw HTML.
  static std::string fix_raw_html_links(const char *s, size_t n) {
    std::string in(s, n), low = in, out;
    for (char &c : low) c = (char)tolower((unsigned char)c);
    size_t i = 0;
    while (true) {
      size_t h = low.find("href", i);
      if (h == std::string::npos) { out += in.substr(i); break; }
      size_t j = h + 4;
      while (j < in.size() && isspace((unsigned char)in[j])) j++;
      if (j >= in.size() || in[j] != '=') { out += in.substr(i, j - i); i = j; continue; }
      j++;
      while (j < in.size() && isspace((unsigned char)in[j])) j++;
      if (j < in.size() && (in[j] == '"' || in[j] == '\'')) j++;
      out += in.substr(i, j - i);
      i = j;
      size_t k = j;
      while (k < in.size() && (isalnum((unsigned char)in[k]) || in[k] == '+' || in[k] == '-' || in[k] == '.')) k++;
      if (k < in.size() && in[k] == ':' && k > j + 1 && low.compare(j, 5, "file:") != 0)
        out += kExternalPrefix;
    }
    return out;
  }

  void text(MD_TEXTTYPE type, const char *s, size_t n) {
    if (text_table) {
      if (trows.empty() || trows.back().empty()) return;
      std::string &cell = trows.back().back();
      if (type == MD_TEXT_NULLCHAR) append_utf8(cell, 0xfffd);
      else if (type == MD_TEXT_ENTITY) decode_entity(cell, s, n);
      else if (type != MD_TEXT_HTML) cell.append(s, n);
      return;
    }
    if (img_depth) { img_alt.append(s, n); return; }
    if (in_heading) heading_text.append(s, n);
    switch (type) {
      case MD_TEXT_NULLCHAR: append_utf8(out, 0xfffd); return;
      case MD_TEXT_BR: out += "<br>"; return;
      case MD_TEXT_SOFTBR: out += "\n"; return;
      case MD_TEXT_ENTITY: decode_entity(out, s, n); return;
      case MD_TEXT_HTML:
        if (!can_box() && has_table_tag(s, n)) return;  // would hang the widget
        out += fix_raw_html_links(s, n);
        return;
      default: break;
    }
    if (in_code_block) { code_buf.append(s, n); return; }
    if (del_depth) {
      // Pango renders U+0336 (combining long stroke) as a clean strikethrough.
      for (size_t i = 0; i < n; i++) {
        escape(out, s + i, 1);
        bool cont = i + 1 < n && (s[i + 1] & 0xc0) == 0x80;
        if (!cont && s[i] != ' ') append_utf8(out, 0x336);
      }
      return;
    }
    put_text(s, n);
  }

  // Fl_Help_View only breaks lines at spaces, so one very long word (a URL,
  // a path in `code`) wider than the column widens the whole page. Like a
  // browser's overflow-wrap, force a break inside words that can't fit.
  size_t word_run = 0;   // glyphs since the last space
  bool in_code_span = false;
  void put_text(const char *s, size_t n) {
    double px = m.text_w - li_depth * 4.0 * m.textsize - (quote_boxed.empty() ? 0 : 2 * m.textsize);
    double cw = in_code_span ? m.mono_w : 0.7 * m.textsize;  // generous for proportional text
    size_t limit = std::max(8, (int)(px / cw) - 1);
    for (size_t i = 0; i < n; i++) {
      unsigned char c = s[i];
      if (c == ' ' || c == '\n') word_run = 0;
      else if ((c & 0xc0) != 0x80 && ++word_run > limit) { out += "<br>"; word_run = 1; }
      escape(out, s + i, 1);
    }
  }

  void enter_block(MD_BLOCKTYPE type, void *d) {
    word_run = 0;
    switch (type) {
      case MD_BLOCK_DOC: break;
      case MD_BLOCK_QUOTE:
        if (can_box()) {
          out += "<table width=\"" + box_width(1) + "\" border=\"0\"><tr><td bgcolor=\"" + hex(t->quote_bg) + "\">";
          box_depth++;
          quote_paras = 0;
          quote_boxed.push_back(true);
        } else {
          out += "<dl><dd>";
          quote_boxed.push_back(false);
        }
        break;
      case MD_BLOCK_UL: out += "<ul>"; break;
      case MD_BLOCK_OL: out += "<ol>"; break;
      case MD_BLOCK_LI: {
        auto *li = (MD_BLOCK_LI_DETAIL *)d;
        out += "<li>";
        li_depth++;
        if (li->is_task) {
          bool done = li->task_mark == 'x' || li->task_mark == 'X';
          out += "<font color=\"" + hex(done ? t->link : t->muted) + "\">";
          append_utf8(out, done ? 0x2611 : 0x2610);  // ballot box (with check)
          out += "</font>&nbsp;";
        }
        break;
      }
      case MD_BLOCK_HR:
        // A quiet book-style separator instead of a harsh full-contrast rule.
        out += "<p><center><font color=\"" + hex(t->bar) + "\">\xe2\x80\xa2&nbsp;&nbsp;&nbsp;"
               "\xe2\x80\xa2&nbsp;&nbsp;&nbsp;\xe2\x80\xa2</font></center></p>";
        break;
      case MD_BLOCK_H: {
        auto *h = (MD_BLOCK_H_DETAIL *)d;
        heading_pos = out.size();
        in_heading = true;
        heading_text.clear();
        out += "<h" + std::to_string(h->level) + "><font color=\"" + hex(t->heading) + "\">";
        break;
      }
      case MD_BLOCK_CODE:
        in_code_block = true;
        code_buf.clear();
        if (can_box()) {
          code_boxed = true;
          code_flow = false;
          box_depth++;
          out += "<table width=\"" + box_width(1) + "\" border=\"0\" bgcolor=\"" + hex(t->code_bg) + "\"><tr><td><pre>\n";
        } else {
          code_boxed = false;
          // Fl_Help_View draws <pre> inside <li> on top of the next items,
          // so inside lists code is emitted as monospace lines with <br>.
          code_flow = li_depth > 0;
          out += code_flow ? "<br><tt><font color=\"" + hex(t->code_fg) + "\">"
                           : "<pre><font color=\"" + hex(t->code_fg) + "\">";
        }
        break;
      case MD_BLOCK_HTML: break;
      case MD_BLOCK_P:
        if (!quote_boxed.empty() && quote_boxed.back()) {
          // Inside a quote box: <p> would add a blank line at the top of the cell.
          if (quote_paras++) out += "<br><br>";
        } else {
          out += "<p>";
        }
        if (!quote_boxed.empty()) out += "<font color=\"" + hex(t->muted) + "\"><i>";
        break;
      case MD_BLOCK_TABLE:
        body_row = 0;
        if (can_box()) {
          out += "<table width=\"" + box_width((int)((MD_BLOCK_TABLE_DETAIL *)d)->col_count) + "\" border=\"0\">";
          box_depth++;
        } else {
          text_table = true;
          trows.clear();
          talign.clear();
          thead_rows = 0;
        }
        break;
      case MD_BLOCK_THEAD: in_thead = true; break;
      case MD_BLOCK_TBODY: in_thead = false; break;
      case MD_BLOCK_TR:
        if (text_table) { trows.emplace_back(); if (in_thead) thead_rows++; }
        else out += "<tr>";
        break;
      case MD_BLOCK_TH:
      case MD_BLOCK_TD: {
        auto *td = (MD_BLOCK_TD_DETAIL *)d;
        if (text_table) {
          trows.back().emplace_back();
          size_t col = trows.back().size() - 1;
          if (col >= talign.size()) talign.push_back(td->align);
          break;
        }
        unsigned bg = in_thead ? t->code_bg : (body_row % 2 ? t->zebra : t->bg);
        out += type == MD_BLOCK_TH ? "<th" : "<td";
        out += " bgcolor=\"" + hex(bg) + "\"";
        if (td->align == MD_ALIGN_CENTER) out += " align=\"center\"";
        else if (td->align == MD_ALIGN_RIGHT) out += " align=\"right\"";
        else out += " align=\"left\"";
        out += ">";
        if (type == MD_BLOCK_TH) out += "<font color=\"" + hex(t->heading) + "\">";
        break;
      }
    }
  }

  void leave_block(MD_BLOCKTYPE type, void *d) {
    word_run = 0;
    switch (type) {
      case MD_BLOCK_DOC: break;
      case MD_BLOCK_QUOTE:
        if (quote_boxed.back()) { out += "</td></tr></table><p></p>"; box_depth--; }
        else out += "</dd></dl>";
        quote_boxed.pop_back();
        break;
      case MD_BLOCK_UL: out += "</ul>"; break;
      case MD_BLOCK_OL: out += "</ol>"; break;
      case MD_BLOCK_LI: out += "</li>"; li_depth--; break;
      case MD_BLOCK_HR: break;
      case MD_BLOCK_H: {
        auto *h = (MD_BLOCK_H_DETAIL *)d;
        out += "</font></h" + std::to_string(h->level) + ">";
        std::string slug = slugify(heading_text);
        if (!slug.empty()) out.insert(heading_pos, "<a name=\"" + slug + "\"></a>");
        in_heading = false;
        break;
      }
      case MD_BLOCK_CODE:
        emit_code(code_buf, code_cols(code_boxed));
        if (code_boxed) { out += "\n</pre></td></tr></table><p></p>"; box_depth--; }
        else if (code_flow) out += "</font></tt><br>";
        else out += "</font></pre>";
        in_code_block = false;
        break;
      case MD_BLOCK_HTML: break;
      case MD_BLOCK_P:
        if (!quote_boxed.empty()) out += "</i></font>";
        if (quote_boxed.empty() || !quote_boxed.back()) out += "</p>";
        break;
      case MD_BLOCK_TABLE:
        if (text_table) { emit_text_table(); text_table = false; }
        else { out += "</table><p></p>"; box_depth--; }
        break;
      case MD_BLOCK_THEAD: in_thead = false; break;
      case MD_BLOCK_TBODY: break;
      case MD_BLOCK_TR: if (!text_table) out += "</tr>\n"; if (!in_thead) body_row++; break;
      case MD_BLOCK_TH: if (!text_table) out += "</font></th>"; break;
      case MD_BLOCK_TD:
        if (text_table) break;
        if (out.size() >= 1 && out.back() == '>') out += "&nbsp;";
        out += "</td>";
        break;
    }
  }

  static bool has_table_tag(const char *s, size_t n) {
    std::string l(s, n);
    for (char &c : l) c = (char)tolower((unsigned char)c);
    for (const char *tag : {"<table", "<tr", "<td", "<th", "</table", "</tr", "</td", "</th"}) {
      size_t p = l.find(tag);
      if (p != std::string::npos) {
        char next = p + strlen(tag) < l.size() ? l[p + strlen(tag)] : '>';
        if (next == '>' || next == ' ' || next == '/' || next == '\n' || next == '\t') return true;
      }
    }
    return false;
  }

  // Emit code lines, soft-wrapping the ones that don't fit in `cols`.
  // Wrapping matters beyond looks: a line wider than the view makes
  // Fl_Help_View widen the whole page, and very long <pre> lines are
  // quadratic to lay out (an 800-char line costs ~0.2 s).
  // Continuation lines get a hanging indent: under the "comment column"
  // (text after a run of 2+ spaces) when there is one, else a bit deeper
  // than the line's own indentation.
  void emit_code(const std::string &code, size_t cols) {
    std::string text = code;
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    bool first = true;
    auto emit_line = [&](const std::vector<std::string> &g, size_t from, size_t to, size_t pad) {
      if (!first) out += code_flow ? "<br>" : "\n";
      first = false;
      std::string l(pad + (code_flow ? 0 : 2), ' ');
      for (size_t i = from; i < to; i++) l += g[i];
      while (!l.empty() && l.back() == ' ') l.pop_back();
      put_code(l);
    };
    size_t pos = 0;
    while (true) {
      size_t nl = text.find('\n', pos);
      if (nl == std::string::npos) nl = text.size();
      // Split into glyphs (UTF-8 code points), expanding tabs to 4 columns.
      std::vector<std::string> g;
      for (size_t i = pos; i < nl;) {
        unsigned char c = text[i];
        if (c == '\t') { do g.push_back(" "); while (g.size() % 4); i++; continue; }
        size_t n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        g.push_back(text.substr(i, n));
        i += n;
      }
      if (g.size() <= cols) {
        emit_line(g, 0, g.size(), 0);
      } else {
        size_t lead = 0;
        while (lead < g.size() && g[lead] == " ") lead++;
        size_t hang = 0;
        for (size_t i = 2; i < g.size() && i <= cols * 6 / 10; i++)
          if (g[i] != " " && g[i - 1] == " " && g[i - 2] == " " && i > lead) hang = i;
        if (!hang) hang = std::min(lead + 4, cols / 2);
        size_t start = 0, pad = 0;
        while (start < g.size()) {
          size_t lim = cols - pad;
          size_t end = start + lim;
          if (end >= g.size()) { emit_line(g, start, g.size(), pad); break; }
          // Prefer breaking after a space, if that keeps the line half full.
          size_t brk = end;
          while (brk > start + lim / 2 && g[brk - 1] != " ") brk--;
          if (brk <= start + lim / 2) brk = end;
          emit_line(g, start, brk, pad);
          start = brk;
          while (start < g.size() && g[start] == " ") start++;
          pad = hang;
        }
      }
      if (nl >= text.size()) break;
      pos = nl + 1;
    }
  }

  // Escaped code text; outside <pre> spaces must be &nbsp; to keep indentation.
  void put_code(const std::string &s) {
    if (!code_flow) { escape(out, s.data(), s.size()); return; }
    for (char c : s) {
      if (c == ' ') out += "&nbsp;";
      else escape(out, &c, 1);
    }
  }

  static size_t width_of(const std::string &s) {  // display columns (approx.)
    size_t w = 0;
    for (unsigned char c : s) if ((c & 0xc0) != 0x80) w++;
    return w;
  }

  // A Markdown table that can't be a real <table> (inside a list or quote).
  // If it fits: aligned monospace columns. If not: one row per line, cells
  // joined by " · " and wrapped, so nothing is wider than the view.
  void emit_text_table() {
    size_t cols = code_cols(false);
    std::vector<size_t> wid(talign.size(), 0);
    for (auto &r : trows)
      for (size_t c = 0; c < r.size() && c < wid.size(); c++)
        wid[c] = std::max(wid[c], width_of(r[c]));
    size_t total = 0;
    for (size_t c = 0; c < wid.size(); c++) total += wid[c] + (c ? 3 : 0);

    auto aligned = [&](const std::vector<std::string> &r) {
      std::string l;
      for (size_t c = 0; c < wid.size(); c++) {
        std::string v = c < r.size() ? r[c] : "";
        size_t pad = wid[c] - width_of(v), left = 0;
        if (talign[c] == MD_ALIGN_RIGHT) left = pad;
        else if (talign[c] == MD_ALIGN_CENTER) left = pad / 2;
        if (c) l += "   ";
        l += std::string(left, ' ') + v + std::string(pad - left, ' ');
      }
      while (!l.empty() && l.back() == ' ') l.pop_back();
      return l;
    };
    auto joined = [&](const std::vector<std::string> &r) {
      std::string l;
      for (auto &v : r) {
        if (v.empty()) continue;
        if (!l.empty()) l += "  \xc2\xb7  ";  // middle dot
        l += v;
      }
      return l;
    };

    bool saved = code_flow;
    code_flow = li_depth > 0;  // <pre> inside <li> overlaps the next items
    out += code_flow ? "<br><tt>" : "<pre>";
    auto sep = [&] { out += code_flow ? "<br>" : "\n"; };
    auto head_on = [&] { out += "<font color=\"" + hex(t->heading) + "\">"; };
    for (size_t i = 0; i < trows.size(); i++) {
      bool head = i < thead_rows;
      if (i) sep();
      if (head) head_on();
      if (total <= cols) put_code(aligned(trows[i]));
      else emit_code(joined(trows[i]), cols);
      if (head) out += "</font>";
      if (head && i + 1 == thead_rows) {
        size_t n = std::min(total, cols);
        std::string rule;
        for (size_t k = 0; k < n; k++) rule += "\xe2\x94\x80";  // box-drawing line
        sep();
        out += "<font color=\"" + hex(t->bar) + "\">" + rule + "</font>";
      }
    }
    out += code_flow ? "</tt><br>" : "</pre>";
    code_flow = saved;
  }

  void enter_span(MD_SPANTYPE type, void *d) {
    if (text_table) return;
    if (img_depth && type != MD_SPAN_IMG) return;
    switch (type) {
      case MD_SPAN_EM: out += "<i>"; break;
      case MD_SPAN_STRONG: out += "<b>"; break;
      case MD_SPAN_U: out += "<u>"; break;
      case MD_SPAN_DEL: del_depth++; out += "<font color=\"" + hex(t->muted) + "\">"; break;
      case MD_SPAN_A: {
        auto *a = (MD_SPAN_A_DETAIL *)d;
        out += "<a href=\"";
        std::string h = link_href(attr(a->href));
        escape(out, h.data(), h.size());
        out += "\">";
        break;
      }
      case MD_SPAN_IMG:
        if (img_depth++ == 0) img_alt.clear();
        break;
      case MD_SPAN_CODE:
        in_code_span = true;
        // Fl_Help_View draws the space before a font change in the NEW font,
        // so a plain space before monospace looks too wide and throws off its
        // line-width math. Bind it to the previous word as a normal-width nbsp.
        if (!out.empty() && out.back() == ' ') { out.pop_back(); out += "&nbsp;"; }
        out += "<font color=\"" + hex(t->code_fg) + "\"><code>";
        break;
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY:
        out += "<font color=\"" + hex(t->code_fg) + "\"><i>";
        break;
      case MD_SPAN_WIKILINK: {
        auto *w = (MD_SPAN_WIKILINK_DETAIL *)d;
        std::string h = link_href(attr(w->target) + ".md");
        out += "<a href=\"";
        escape(out, h.data(), h.size());
        out += "\">";
        break;
      }
    }
  }

  void leave_span(MD_SPANTYPE type, void *d) {
    if (text_table) return;
    if (img_depth && type != MD_SPAN_IMG) return;
    switch (type) {
      case MD_SPAN_EM: out += "</i>"; break;
      case MD_SPAN_STRONG: out += "</b>"; break;
      case MD_SPAN_U: out += "</u>"; break;
      case MD_SPAN_DEL: del_depth--; out += "</font>"; break;
      case MD_SPAN_A: case MD_SPAN_WIKILINK: out += "</a>"; break;
      case MD_SPAN_IMG: {
        if (--img_depth) break;
        auto *im = (MD_SPAN_IMG_DETAIL *)d;
        std::string src = attr(im->src);
        if (has_url_scheme(src) && src.compare(0, 5, "file:") != 0) {
          // Fl_Help_View can't fetch remote images: show a link instead.
          std::string href = link_href(src);
          out += "<a href=\"";
          escape(out, href.data(), href.size());
          out += "\"><font color=\"" + hex(t->muted) + "\">[image: ";
          escape(out, img_alt.data(), img_alt.size());
          out += "]</font></a>";
          break;
        }
        if (src.compare(0, 7, "file://") == 0) src = src.substr(7);
        src = resolve(src);
        std::string size;
        if (Fl_Shared_Image *img = Fl_Shared_Image::get(src.c_str())) {
          int w = img->w(), h = img->h();
          if (w > max_img_w && w > 0) { h = h * max_img_w / w; w = max_img_w; }
          size = " width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"";
          img->release();
        }
        out += "<img src=\"";
        escape(out, src.data(), src.size());
        out += "\" alt=\"";
        escape(out, img_alt.data(), img_alt.size());
        out += "\"" + size + ">";
        break;
      }
      case MD_SPAN_CODE: in_code_span = false; out += "</code></font>"; break;
      case MD_SPAN_LATEXMATH:
      case MD_SPAN_LATEXMATH_DISPLAY: out += "</i></font>"; break;
    }
  }
};

int cb_enter_block(MD_BLOCKTYPE t, void *d, void *u) { ((R *)u)->enter_block(t, d); return 0; }
int cb_leave_block(MD_BLOCKTYPE t, void *d, void *u) { ((R *)u)->leave_block(t, d); return 0; }
int cb_enter_span(MD_SPANTYPE t, void *d, void *u) { ((R *)u)->enter_span(t, d); return 0; }
int cb_leave_span(MD_SPANTYPE t, void *d, void *u) { ((R *)u)->leave_span(t, d); return 0; }
int cb_text(MD_TEXTTYPE t, const MD_CHAR *s, MD_SIZE n, void *u) { ((R *)u)->text(t, s, n); return 0; }

}  // namespace

bool has_url_scheme(const std::string &s) {
  size_t i = 0;
  while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
  return i > 1 && i < s.size() && s[i] == ':';
}

// Wrap every case-insensitive occurrence of `needle` in the text (not inside
// tags) with a colored, underlined, bold mark. Fl_Help_View can't draw a
// background behind inline text, so this is how all matches stand out; the
// current match additionally gets the selection background (main.cpp).
static void mark_matches(std::string &html, const std::string &needle, unsigned color) {
  std::string n;
  escape(n, needle.data(), needle.size());  // text in the HTML is escaped the same way
  if (n.empty()) return;
  for (char &c : n) c = (char)tolower((unsigned char)c);
  std::string open = "<font color=\"" + hex(color) + "\"><u><b>", close = "</b></u></font>";
  std::string out;
  out.reserve(html.size() + 64);
  size_t i = 0;
  bool in_body = false;
  while (i < html.size()) {
    if (html[i] == '<') {
      size_t e = html.find('>', i);
      if (e == std::string::npos) e = html.size() - 1;
      if (html.compare(i, 5, "<body") == 0) in_body = true;
      out.append(html, i, e - i + 1);
      i = e + 1;
      continue;
    }
    size_t e = html.find('<', i);
    if (e == std::string::npos) e = html.size();
    size_t j = i;
    while (j < e) {
      bool hit = in_body && j + n.size() <= e;
      for (size_t k = 0; hit && k < n.size(); k++)
        hit = tolower((unsigned char)html[j + k]) == (unsigned char)n[k];
      if (hit) {
        out += open;
        out.append(html, j, n.size());
        out += close;
        j += n.size();
      } else if (html[j] == '&') {
        // Copy an entity like &amp; whole, so a match can't split it.
        size_t semi = html.find(';', j);
        size_t len = (semi != std::string::npos && semi < e && semi - j <= 10) ? semi - j + 1 : 1;
        out.append(html, j, len);
        j += len;
      } else {
        out += html[j++];
      }
    }
    i = e;
  }
  html.swap(out);
}

std::string markdown_to_html(const std::string &md, const std::string &base_dir,
                             const Theme &t, const RenderMetrics &m,
                             const std::string &highlight) {
  R r;
  r.m = m;
  int max_img_w = m.text_w - 8;
  r.base = base_dir;
  r.t = &t;
  r.max_img_w = max_img_w;
  r.out.reserve(md.size() * 2 + 512);
  r.out += "<html><body bgcolor=\"" + hex(t.bg) + "\" text=\"" + hex(t.fg) +
           "\" link=\"" + hex(t.link) + "\">\n";

  MD_PARSER p = {};
  p.abi_version = 0;
  p.flags = MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS | MD_FLAG_WIKILINKS;
  p.enter_block = cb_enter_block;
  p.leave_block = cb_leave_block;
  p.enter_span = cb_enter_span;
  p.leave_span = cb_leave_span;
  p.text = cb_text;
  md_parse(md.data(), (MD_SIZE)md.size(), &p, &r);

  r.out += "\n<br><br></body></html>\n";
  if (!highlight.empty()) mark_matches(r.out, highlight, t.mark);
  return r.out;
}
