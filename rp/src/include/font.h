/**
 * File: font.h
 * Description: Bitmap font descriptor, kept field-for-field identical to
 *              md-framebuffer-template's `struct FB_FONT` so font assets
 *              (font8x8.h) drop in between the two projects unmodified.
 *
 * md-oric has no chunked framebuffer, so the template's renderer (fb_font.c)
 * does not port across -- the overlay writes Oric-format pixels into
 * `line_buff` and converts to ST planar. Only the descriptor and the glyph
 * data are shared.
 *
 * Glyph contract (from the template): each glyph is `h` bytes, one byte per
 * row top-to-bottom; within a row **bit 0 is the LEFTMOST pixel** (LSB-left).
 */

#ifndef ORIC_FONT_H
#define ORIC_FONT_H

struct FB_FONT {
  int w;                     /* glyph cell width in pixels (advance) */
  int h;                     /* glyph height in pixels */
  int first_char;            /* first ASCII codepoint represented */
  int num_chars;             /* count of sequential characters */
  const unsigned char *data; /* bitmap rows: (h rows) * num_chars */
};

#endif /* ORIC_FONT_H */
