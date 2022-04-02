/*
 *
 * Conky, a system monitor, based on torsmo
 *
 * Any original torsmo code is licensed under the BSD license
 *
 * All code written since the fork of torsmo is licensed under the GPL
 *
 * Please see COPYING for details
 *
 * Copyright (c) 2004, Hannu Saransaari and Lauri Hakkarainen
 * Copyright (c) 2005-2021 Brenden Matthews, Philip Kovacs, et. al.
 *	(see AUTHORS)
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
#include "conky.h"
#include "logging.h"
#include "gui.h"
#ifdef BUILD_X11
#include "x11.h"
#endif /*BUILD_X11*/
#ifdef BUILD_WAYLAND
#include "x11-color.h"
#endif /*BUILD_WAYLAND*/

/* precalculated: 31/255, and 63/255 */
#define CONST_8_TO_5_BITS 0.12156862745098
#define CONST_8_TO_6_BITS 0.247058823529412

short colour_depth = 0;
long redmask, greenmask, bluemask;

void set_up_gradient() {
#ifdef BUILD_X11
  if (out_to_x.get(*state)) {
    colour_depth = DisplayPlanes(display, screen);
  } else
#endif /* BUILD_X11 */
  {
    colour_depth = 16;
  }
  if (colour_depth != 24 && colour_depth != 16) {
    NORM_ERR(
        "using non-standard colour depth, gradients may look like a "
        "lolly-pop");
  }

  redmask = 0;
  greenmask = 0;
  bluemask = 0;
  for (int i = (colour_depth / 3) - 1; i >= 0; i--) {
    redmask |= 1 << i;
    greenmask |= 1 << i;
    bluemask |= 1 << i;
  }
  if (colour_depth % 3 == 1) { greenmask |= 1 << (colour_depth / 3); }
  redmask = redmask << (2 * colour_depth / 3 + colour_depth % 3);
  greenmask = greenmask << (colour_depth / 3);
}

/* adjust colour values depending on colour depth */
unsigned int adjust_colours(unsigned int colour) {
  double r, g, b;

  if (colour_depth == 0) { set_up_gradient(); }
  if (colour_depth == 16) {
    r = (colour & 0xff0000) >> 16;
    g = (colour & 0xff00) >> 8;
    b = colour & 0xff;
    colour = static_cast<int>(r * CONST_8_TO_5_BITS) << 11;
    colour |= static_cast<int>(g * CONST_8_TO_6_BITS) << 5;
    colour |= static_cast<int>(b * CONST_8_TO_5_BITS);
  }
  return colour;
}

/* this function returns the next colour between two colours for a gradient */
std::unique_ptr<unsigned long[]> do_gradient(int width,
                                             unsigned long first_colour,
                                             unsigned long last_colour) {
  int red1, green1, blue1;           // first colour
  int red2, green2, blue2;           // last colour
  int reddiff, greendiff, bluediff;  // difference
  short redshift = (2 * colour_depth / 3 + colour_depth % 3);
  short greenshift = (colour_depth / 3);

  // Make sure the width is always at least 2
  width = std::max(2, width);

  std::unique_ptr<unsigned long[]> colours(new unsigned long[width]);

  if (colour_depth == 0) { set_up_gradient(); }
  red1 = (first_colour & redmask) >> redshift;
  green1 = (first_colour & greenmask) >> greenshift;
  blue1 = first_colour & bluemask;
  red2 = (last_colour & redmask) >> redshift;
  green2 = (last_colour & greenmask) >> greenshift;
  blue2 = last_colour & bluemask;
  reddiff = abs(red1 - red2);
  greendiff = abs(green1 - green2);
  bluediff = abs(blue1 - blue2);
#ifdef HAVE_OPENMP
#pragma omp parallel for schedule(dynamic, 10) shared(colours)
#endif /* HAVE_OPENMP */
  for (int i = 0; i < width; i++) {
    int red3 = 0, green3 = 0, blue3 = 0;  // colour components

    float factor = (static_cast<float>(i) / (width - 1));

    /* the '+ 0.5' bit rounds our floats to ints properly */
    if (red1 >= red2) {
      red3 = -(factor * reddiff) - 0.5;
    } else if (red1 < red2) {
      red3 = factor * reddiff + 0.5;
    }
    if (green1 >= green2) {
      green3 = -(factor * greendiff) - 0.5;
    } else if (green1 < green2) {
      green3 = factor * greendiff + 0.5;
    }
    if (blue1 >= blue2) {
      blue3 = -(factor * bluediff) - 0.5;
    } else if (blue1 < blue2) {
      blue3 = factor * bluediff + 0.5;
    }
    red3 += red1;
    green3 += green1;
    blue3 += blue1;
    if (red3 < 0) { red3 = 0; }
    if (green3 < 0) { green3 = 0; }
    if (blue3 < 0) { blue3 = 0; }
    if (red3 > bluemask) { red3 = bluemask; }
    if (green3 > bluemask) { green3 = bluemask; }
    if (blue3 > bluemask) { blue3 = bluemask; }
    colours[i] = (red3 << redshift) | (green3 << greenshift) | blue3;
  }
  return colours;
}

static int hex_nibble_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    } else if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

#ifdef BUILD_GUI
long get_x11_color(const char *name) {
#ifdef BUILD_X11
  XColor color;

  color.pixel = 0;
  if (XParseColor(display, DefaultColormap(display, screen), name, &color) ==
      0) {
    /* lets check if it's a hex colour with the # missing in front
     * if yes, then do something about it */
    char newname[DEFAULT_TEXT_BUFFER_SIZE];

    newname[0] = '#';
    strncpy(&newname[1], name, DEFAULT_TEXT_BUFFER_SIZE - 1);
    /* now lets try again */
    if (XParseColor(display, DefaultColormap(display, screen), &newname[0],
                    &color) == 0) {
      NORM_ERR("can't parse X color '%s'", name);
      return 0xFF00FF;
    }
  }
  if (XAllocColor(display, DefaultColormap(display, screen), &color) == 0) {
    NORM_ERR("can't allocate X color '%s'", name);
  }

  return static_cast<long>(color.pixel);
#elif BUILD_WAYLAND
    unsigned short r, g, b;
    size_t len = strlen(name);
    if (OsLookupColor(-1, name, len, &r, &g, &b)) {
      return 0x000000ff | ((r & 0xff) << 24) | ((g & 0xff) << 16) | ((b & 0xff) << 8);
    }
    if (name[0] == '#') {
      name++;
      len--;
    }
    if (len == 6 || len == 8)
    {
        unsigned char argb[4] = {0xff, 0, 0, 0};
        for (int i = 0; i + 1 < len; i += 2) {
            int nib1 = hex_nibble_value(name[i]);
            int nib2 = hex_nibble_value(name[i+1]);
            if (nib1 < 0 || nib2 < 0) {
              goto err;
            }
            int val = (nib1 << 4) + nib2;

            argb[3 - i / 2] = val;
        }
        long out;
        memcpy(static_cast<void*>(&out), argb, 4);
        return out;
    }
    err:
    NORM_ERR("can't parse X color '%s' (%d)", name, len);
    return 0xFF00FF;
#endif
}

long get_x11_color(const std::string &colour) {
  return get_x11_color(colour.c_str());
}
#endif /*BUILD_GUI*/
