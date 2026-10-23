/* loading.c -- boot "LOADING..." screen, drawn by the wrapper itself.
 *
 * Shown as soon as the EGL context exists (before the engine .so files are
 * even loaded) and kept on screen through the engine's first steps, which
 * render nothing while Godot initializes and runs the game's autoloads.
 *
 * No shaders, textures or font files: every lit pixel of a built-in 5x7
 * bitmap font is a scissored glClear, so it works on any GLES context.
 *
 * MIT license; see LICENSE. */

#include <string.h>
#include <GLES3/gl3.h>

#include "loading.h"

#define GLYPH_W 5
#define GLYPH_H 7

typedef struct { char c; const char *rows[GLYPH_H]; } Glyph;

static const Glyph s_font[] = {
  { 'L', { "10000", "10000", "10000", "10000", "10000", "10000", "11111" } },
  { 'O', { "01110", "10001", "10001", "10001", "10001", "10001", "01110" } },
  { 'A', { "01110", "10001", "10001", "11111", "10001", "10001", "10001" } },
  { 'D', { "11110", "10001", "10001", "10001", "10001", "10001", "11110" } },
  { 'I', { "11111", "00100", "00100", "00100", "00100", "00100", "11111" } },
  { 'N', { "10001", "11001", "10101", "10011", "10001", "10001", "10001" } },
  { 'G', { "01110", "10001", "10000", "10111", "10001", "10001", "01111" } },
  { '.', { "00000", "00000", "00000", "00000", "00000", "01100", "01100" } },
  { ' ', { "00000", "00000", "00000", "00000", "00000", "00000", "00000" } },
};

static const Glyph *find_glyph(char c) {
  for (unsigned i = 0; i < sizeof(s_font) / sizeof(*s_font); i++)
    if (s_font[i].c == c) return &s_font[i];
  return NULL;
}

void loading_screen_draw(int w, int h) {
  static const char text[] = "LOADING...";
  const int len = (int)strlen(text);

  // one font pixel = 1/90 of the screen height (8 px at 720p, 12 at 1080p),
  // like a chunky NES-style title
  int px = h / 90;
  if (px < 2) px = 2;
  const int adv = (GLYPH_W + 1) * px; // glyph + 1 column of spacing
  const int text_w = len * adv - px;
  const int text_h = GLYPH_H * px;
  const int x0 = (w - text_w) / 2;
  const int y0 = (h - text_h) / 2; // GL origin is bottom-left

  glViewport(0, 0, w, h);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glEnable(GL_SCISSOR_TEST);
  glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
  for (int i = 0; i < len; i++) {
    const Glyph *g = find_glyph(text[i]);
    if (!g) continue;
    for (int row = 0; row < GLYPH_H; row++) {
      for (int col = 0; col < GLYPH_W; col++) {
        if (g->rows[row][col] != '1') continue;
        const int x = x0 + i * adv + col * px;
        const int y = y0 + (GLYPH_H - 1 - row) * px; // row 0 is the top
        glScissor(x, y, px, px);
        glClear(GL_COLOR_BUFFER_BIT);
      }
    }
  }
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}
