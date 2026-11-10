// render.h - Markdown -> HTML tailored for FLTK's Fl_Help_View.
#pragma once
#include <string>
#include "theme.h"

// Measurements of the view, so nothing is laid out wider than it.
// (If anything is wider, Fl_Help_View widens the WHOLE page and text
// disappears under the scrollbar.)
struct RenderMetrics {
  int text_w;      // pixels available for body text
  double mono_w;   // width of one monospace character, pixels
  int textsize;    // body font size, pixels
};

// Convert Markdown text to an HTML document that Fl_Help_View renders well.
//  base_dir : directory of the .md file, used to resolve relative links/images
//  highlight: if not empty, every case-insensitive occurrence of it in the
//             text is marked (search results)
std::string markdown_to_html(const std::string &md, const std::string &base_dir,
                             const Theme &t, const RenderMetrics &m,
                             const std::string &highlight = "");

// External links are emitted as "x-flmd-ext:<url>". Fl_Help_View opens
// http:/https:/mailto: links by itself (and then calls the link handler
// too); with this prefix only our handler sees them and decides.
constexpr const char *kExternalPrefix = "x-flmd-ext:";

// Is this a URL with a scheme (http:, mailto:, ...)?
bool has_url_scheme(const std::string &s);
