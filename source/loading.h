/* loading.h -- boot "LOADING..." screen. MIT license; see LICENSE. */

#ifndef __LOADING_H__
#define __LOADING_H__

// draw the loading screen into the current GL context's back buffer
// (the caller swaps). w/h: surface size in pixels.
void loading_screen_draw(int w, int h);

#endif
