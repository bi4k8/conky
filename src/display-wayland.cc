/*
 *
 * Conky, a system monitor, based on torsmo
 *
 * Please see COPYING for details
 *
 * Copyright (C) 2018-2021 François Revol et al.
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

#include <config.h>

#ifdef BUILD_WAYLAND
#include <wayland-client.h>
//#include "wayland.h"
#include <pango/pangocairo.h>
#include <pango/pangofc-fontmap.h>
#include <cairo.h>
#include <fontconfig/fontconfig.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <wayland-client-protocol.h>
#include <xdg-shell-client-protocol.h>

#endif /* BUILD_WAYLAND */

#include <iostream>
#include <sstream>
#include <unordered_map>

#include "conky.h"
#include "display-wayland.hh"
#include "llua.h"
#include "gui.h"
#ifdef BUILD_X11
#include "x11.h"
#endif
#ifdef BUILD_WAYLAND
#include "fonts.h"
#endif

/* TODO: cleanup global namespace */
#ifdef BUILD_WAYLAND

static int
set_cloexec_or_close(int fd)
{
	long flags;

	if (fd == -1)
		return -1;

	flags = fcntl(fd, F_GETFD);
	if (flags == -1)
		goto err;

	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
		goto err;

	return fd;

err:
	close(fd);
	return -1;
}

static int
create_tmpfile_cloexec(char *tmpname)
{
	int fd;

#ifdef HAVE_MKOSTEMP
	fd = mkostemp(tmpname, O_CLOEXEC);
	if (fd >= 0)
		unlink(tmpname);
#else
	fd = mkstemp(tmpname);
	if (fd >= 0) {
		fd = set_cloexec_or_close(fd);
		unlink(tmpname);
	}
#endif

	return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 *
 * If the C library implements posix_fallocate(), it is used to
 * guarantee that disk space is available for the file at the
 * given size. If disk space is insufficent, errno is set to ENOSPC.
 * If posix_fallocate() is not supported, program may receive
 * SIGBUS on accessing mmap()'ed file contents instead.
 */
static int
os_create_anonymous_file(off_t size)
{
	static const char templ[] = "/weston-shared-XXXXXX";
	const char *path;
	char *name;
	int fd;
	int ret;

	path = getenv("XDG_RUNTIME_DIR");
	if (!path) {
		errno = ENOENT;
		return -1;
	}

	name = static_cast<char*>(malloc(strlen(path) + sizeof(templ)));
	if (!name)
		return -1;

	strcpy(name, path);
	strcat(name, templ);

	fd = create_tmpfile_cloexec(name);

	free(name);

	if (fd < 0)
		return -1;
	ret = posix_fallocate(fd, 0, size);
	if (ret != 0) {
		close(fd);
		errno = ret;
		return -1;
	}
	return fd;
}

// TODO: cleanup externs (move to conky.h ?)
#ifdef OWN_WINDOW
extern int fixed_size, fixed_pos;
#endif
extern int text_start_x, text_start_y;   /* text start position in window */
extern int text_offset_x, text_offset_y; /* offset for start position */
extern int text_width,
    text_height; /* initially 1 so no zero-sized window is created */
extern double current_update_time, next_update_time, last_update_time;
void update_text();
extern int need_to_update;
int get_border_total();
extern conky::range_config_setting<int> maximum_width;
extern long current_color;
static int xft_dpi = -1;

/* for pango_fonts */
struct pango_font {
  PangoFontDescription *desc;

  struct {
    uint32_t ascent;
    uint32_t descent;
  } metrics;
  int font_alpha;

  pango_font()
      : desc(nullptr),
        metrics({0, 0}),
        font_alpha(0xffff)
  {
  }
};

static std::vector<pango_font> pango_fonts; /* indexed by selected_font */

#ifdef BUILD_XFT
namespace {
class xftalpha_setting : public conky::simple_config_setting<float> {
  using Base = conky::simple_config_setting<float>;

 protected:
  void lua_setter(lua::state &l, bool init) override {
    lua::stack_sentry s(l, -2);

    Base::lua_setter(l, init);

    if (init && out_to_wayland.get(*state)) {
      pango_fonts.resize(std::max(1, static_cast<int>(fonts.size())));
      pango_fonts[0].desc = nullptr;
      pango_fonts[0].font_alpha = do_convert(l, -1).first * 0xffff;
    }

    ++s;
  }

 public:
  xftalpha_setting() : Base("xftalpha", 1.0, false) {}
};

xftalpha_setting xftalpha;
}  // namespace
#endif /* BUILD_XFT */

static void wayland_create_window();

/*struct _x11_stuff_s {
  Region region;
#ifdef BUILD_XDAMAGE
  Damage damage;
  XserverRegion region2, part;
  int event_base, error_base;
#endif
} x11_stuff;*/

static void wayland_create_window() {
  setup_fonts();
  load_fonts(utf8_mode.get(*state));
  //TODO: dpi
#if 0
  if (use_xft.get(*state)) {
    auto dpi = XGetDefault(display, "Xft", "dpi");
    if (dpi) { xft_dpi = atoi(dpi); }
  }
#endif
  update_text_area(); /* to position text/window on screen */

#ifdef OWN_WINDOW
  if (own_window.get(*state)) {
    if (fixed_pos == 0) {
      //XMoveWindow(display, window.window, window.x, window.y);
      //TODO
    }

    //set_transparent_background(window.window);
  }
#endif

  //create_gc();

  //draw_stuff();

  //x11_stuff.region = XCreateRegion();
/*
#ifdef BUILD_XDAMAGE
  x11_stuff.damage =
      XDamageCreate(display, window.window, XDamageReportNonEmpty);
  x11_stuff.region2 = XFixesCreateRegionFromWindow(display, window.window, 0);
  x11_stuff.part = XFixesCreateRegionFromWindow(display, window.window, 0);
#endif
*/

  selected_font = 0;
  update_text_area(); /* to get initial size of the window */
}

#endif /* BUILD_WAYLAND */

namespace conky {
namespace {

#ifdef BUILD_WAYLAND
conky::display_output_wayland wayland_output;
#else
conky::disabled_display_output wayland_output_disabled("wayland", "BUILD_WAYLAND");
#endif

}  // namespace
extern void init_wayland_output() {}

namespace priv {}  // namespace priv

#ifdef BUILD_WAYLAND

display_output_wayland::display_output_wayland() : display_output_base("wayland") {
  is_graphical = true;
  priority = 2;
}

bool display_output_wayland::detect() {
  if (out_to_wayland.get(*state)) {
    DBGP2("Wayland display output '%s' enabled in config.", name.c_str());
    return true;
  }
  return false;
}

static int epoll_fd;
static struct epoll_event ep[1];

static struct window *global_window;
static wl_display *global_display;
static bool global_running = true;

struct rectangle {
	size_t x, y, width, height;
};

struct window {
	struct rectangle rectangle;
	struct wl_shm *shm;
	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	cairo_surface_t *cairo_surface;
	cairo_t *cr;
	PangoLayout *layout;
	PangoContext* pango_context;
};

struct {
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
/*	struct wl_shell *shell;
	struct wl_shell_surface *shell_surface;*/
	struct wl_surface *surface;
	struct wl_seat *seat;
/*	struct wl_pointer *pointer;*/
	struct wl_output *output;
	struct xdg_wm_base *shell;
} wl_globals;


static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *shell, uint32_t serial)
{
    xdg_wm_base_pong(shell, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = &xdg_wm_base_ping,
};


void
registry_handle_global(void *data, struct wl_registry *registry, uint32_t name,
	  const char *interface, uint32_t version) {
	if(strcmp(interface, "wl_compositor") == 0) {
		wl_globals.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name,
					  &wl_compositor_interface, 3));
	} /*else if(strcmp(interface, "wl_shell") == 0) {
		registry.shell = wl_registry_bind(registry, name,
					    &wl_shell_interface, 1);
	}*/ else if(strcmp(interface, "wl_shm") == 0) {
		wl_globals.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
	} else if(strcmp(interface, "wl_seat") == 0) {
		wl_globals.seat = static_cast<wl_seat*>(wl_registry_bind(registry, name,
					   &wl_seat_interface, 1));
	} else if(strcmp(interface, "wl_output") == 0) {
		wl_globals.output = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface, 1));
	} else if(strcmp(interface, "xdg_wm_base") == 0) {
		wl_globals.shell = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
		xdg_wm_base_add_listener(wl_globals.shell, &xdg_wm_base_listener, nullptr);
	}
}

void
registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
}


static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};


static void
xdg_toplevel_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                       int32_t width, int32_t height, struct wl_array *states)
{
    int border_total = get_border_total();

    text_width = width - 2 * border_total;
    text_height = height - 2 * border_total;
    global_window->rectangle.width = width;
    global_window->rectangle.height = height;
    //int mw = this->dpi_scale(maximum_width.get(*state));
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = &xdg_toplevel_configure,
    .close = &xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                      uint32_t serial)
{
    xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = &xdg_surface_configure,
};

struct window *
window_create(struct wl_surface* surface, struct wl_shm* shm, int width, int height);

void
window_resize(struct window *window, int width, int height);

void
window_destroy(struct window *window);

void
window_commit_buffer(struct window *window);

void
window_get_width_height(struct window *window, int *w, int *h);

bool display_output_wayland::initialize() {
	epoll_fd = epoll_create1(0);
	if(epoll_fd < 0) {
		perror("epoll_create");
		return false;
	}
	global_display = wl_display_connect(NULL);
	wl_globals.registry = wl_display_get_registry(global_display);
	wl_registry_add_listener(wl_globals.registry, &registry_listener, NULL);

	wl_display_roundtrip(global_display);

  struct wl_surface *surface = wl_compositor_create_surface(wl_globals.compositor);
  global_window = window_create(surface, wl_globals.shm, 1, 1);

  global_window->xdg_surface = xdg_wm_base_get_xdg_surface(wl_globals.shell, global_window->surface);
  xdg_surface_add_listener(global_window->xdg_surface, &xdg_surface_listener, nullptr);

  global_window->xdg_toplevel = xdg_surface_get_toplevel(global_window->xdg_surface);
  xdg_toplevel_add_listener(global_window->xdg_toplevel, &xdg_toplevel_listener, nullptr);

  xdg_toplevel_set_app_id(global_window->xdg_toplevel, "conky");
  xdg_toplevel_set_title(global_window->xdg_toplevel, "conky");
  xdg_toplevel_set_parent(global_window->xdg_toplevel, 0);
  wl_surface_set_buffer_scale(global_window->surface, 1);
  wl_surface_commit(global_window->surface);
  wl_display_roundtrip(global_display);

  wayland_create_window();
  return true;
}

typedef void (*display_global_handler_t)(struct display *display,
					 uint32_t name,
					 const char *interface,
					 uint32_t version, void *data);
typedef void (*display_output_handler_t)(struct output *output, void *data);

bool display_output_wayland::shutdown() { return false; }

#define ARRAY_LENGTH(x) (sizeof(x)/sizeof(x[0]))

bool added = false;

bool display_output_wayland::main_loop_wait(double t) {
	while (wl_display_prepare_read(global_display) != 0)
		wl_display_dispatch_pending(global_display);
	wl_display_flush(global_display);

	if (t < 0.0) {
		t = 0.0;
	}
	int ms = t * 1000;
	printf("epoll(%lf, %d)\n", t, ms);

	ep[0].events =
		EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP;
	ep[0].data.ptr = nullptr;

	if (!added && epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wl_display_get_fd(global_display), &ep[0]) == -1) {
		perror("epoll_ctl: add");
		return false;
	}
	added = true;

    /* wait for Wayland event or timeout */
	int ep_count = epoll_wait(epoll_fd,
			   ep, ARRAY_LENGTH(ep), ms);

	/*if (ep[0].events & EPOLLIN) {
		wl_display_dispatch_pending(global_display);
	}*/
	wl_display_read_events(global_display); wl_display_dispatch_pending(global_display);

	update_text();

  if (need_to_update != 0) {
#ifdef OWN_WINDOW
    int wx = window.x, wy = window.y;
#endif

    need_to_update = 0;
    selected_font = 0;
    update_text_area();

//#ifdef OWN_WINDOW
//    if (own_window.get(*state)) {
      int changed = 0;
      int border_total = get_border_total();

      int width, height;
      window_get_width_height(global_window, &width, &height);

      int fixed_size = 0;

      /* resize window if it isn't right size */
      if ((fixed_size == 0) &&
          (text_width + 2 * border_total != width ||
           text_height + 2 * border_total != height)) {
        width = text_width + 2 * border_total;
        height = text_height + 2 * border_total;
        printf("resizing\n");
        window_resize(global_window, width, height); /* resize window */

        changed++;
        /* update lua window globals */
        llua_update_window_table(text_start_x, text_start_y, text_width,
                                 text_height);
      }

      /* move window if it isn't in right position */
      #ifdef POSITION
      if ((fixed_pos == 0) && (window.x != wx || window.y != wy)) {
        //XMoveWindow(display, window.window, window.x, window.y);
        changed++;
      }
      #endif

      /* update struts */
      if (changed != 0) {
        int sidenum = -1;

        DBGP("%s", _(PACKAGE_NAME ": defining struts\n"));
        fflush(stderr);

        switch (text_alignment.get(*state)) {
          case TOP_LEFT:
          case TOP_RIGHT:
          case TOP_MIDDLE: {
            sidenum = 2;
            break;
          }
          case BOTTOM_LEFT:
          case BOTTOM_RIGHT:
          case BOTTOM_MIDDLE: {
            sidenum = 3;
            break;
          }
          case MIDDLE_LEFT: {
            sidenum = 0;
            break;
          }
          case MIDDLE_RIGHT: {
            sidenum = 1;
            break;
          }

          case NONE:
          case MIDDLE_MIDDLE: /* XXX What about these? */;
        }

        //set_struts(sidenum);
      }
//    }
//#endif

    clear_text(1);

	puts("drawing stuff 1");
    draw_stuff();
  }

#ifdef X_EVENTS
  /* handle X events */
  while (XPending(display) != 0) {
    XEvent ev;

    XNextEvent(display, &ev);
    switch (ev.type) {
#ifdef OWN_WINDOW
      case ConfigureNotify:
        if (own_window.get(*state)) {
          /* if window size isn't what expected, set fixed size */
          if (ev.xconfigure.width != window.width ||
              ev.xconfigure.height != window.height) {
            if (window.width != 0 && window.height != 0) { fixed_size = 1; }

            /* clear old stuff before screwing up
             * size and pos */
            clear_text(1);

            {
              XWindowAttributes attrs;
              if (XGetWindowAttributes(display, window.window, &attrs) != 0) {
                window.width = attrs.width;
                window.height = attrs.height;
              }
            }

            int border_total = get_border_total();

            text_width = window.width - 2 * border_total;
            text_height = window.height - 2 * border_total;
            int mw = this->dpi_scale(maximum_width.get(*state));
            if (text_width > mw && mw > 0) { text_width = mw; }
          }

          /* if position isn't what expected, set fixed pos
           * total_updates avoids setting fixed_pos when window
           * is set to weird locations when started */
          /* // this is broken
          if (total_updates >= 2 && !fixed_pos
              && (window.x != ev.xconfigure.x
              || window.y != ev.xconfigure.y)
              && (ev.xconfigure.x != 0
              || ev.xconfigure.y != 0)) {
            fixed_pos = 1;
          } */
        }
        break;

#ifdef INPUT
      case ButtonPress:
        if (own_window.get(*state)) {
          /* if an ordinary window with decorations */
          if ((own_window_type.get(*state) == TYPE_NORMAL &&
               !TEST_HINT(own_window_hints.get(*state), HINT_UNDECORATED)) ||
              own_window_type.get(*state) == TYPE_DESKTOP) {
            /* allow conky to hold input focus. */
            break;
          }
          /* forward the click to the desktop window */
          XUngrabPointer(display, ev.xbutton.time);
          ev.xbutton.window = window.desktop;
          ev.xbutton.x = ev.xbutton.x_root;
          ev.xbutton.y = ev.xbutton.y_root;
          XSendEvent(display, ev.xbutton.window, False, ButtonPressMask, &ev);
          XSetInputFocus(display, ev.xbutton.window, RevertToParent,
                         ev.xbutton.time);
        }
        break;

      case ButtonRelease:
        if (own_window.get(*state)) {
          /* if an ordinary window with decorations */
          if ((own_window_type.get(*state) == TYPE_NORMAL) &&
              !TEST_HINT(own_window_hints.get(*state), HINT_UNDECORATED)) {
            /* allow conky to hold input focus. */
            break;
          }
          /* forward the release to the desktop window */
          ev.xbutton.window = window.desktop;
          ev.xbutton.x = ev.xbutton.x_root;
          ev.xbutton.y = ev.xbutton.y_root;
          XSendEvent(display, ev.xbutton.window, False, ButtonReleaseMask, &ev);
        }
        break;
#endif

#endif

      default:
        break;
    }
  }

	puts("drawing stuff 2");
    draw_stuff();

#endif /* X_EVENTS */


  // handled
  return true;
}

void display_output_wayland::sigterm_cleanup() {
}

void display_output_wayland::cleanup() {
  if (global_window != nullptr) {
    window_destroy(global_window);
    global_window = nullptr;
  }
  free_fonts(utf8_mode.get(*state));
}

void display_output_wayland::set_foreground_color(long c) {
#ifdef BUILD_ARGB
    current_color = c | (own_window_argb_value.get(*state) << 24);
#endif /* BUILD_ARGB */
    current_color = c;
#ifdef BUILD_ARGB
  }
#endif /* BUILD_ARGB */
  uint8_t r = current_color >> 24;
  uint8_t g = current_color >> 16;
  uint8_t b = current_color >> 8;
  uint8_t a = current_color;
  if (global_window->cr) {
    cairo_set_source_rgba(global_window->cr, r / 255.0, g / 255.0, b / 255.0, a / 255.0);
  }
}

int display_output_wayland::calc_text_width(const char *s) {
  struct window *window = global_window;
  size_t slen = strlen(s);
  pango_layout_set_text(window->layout, s, slen);
  PangoRectangle margin_rect;
  pango_layout_set_font_description(window->layout, pango_fonts[selected_font].desc);
  pango_layout_get_pixel_extents(window->layout, nullptr, &margin_rect);
  return margin_rect.width;
}

static void adjust_coords(int& x, int& y) {
  struct window *window = global_window;
  x += window->rectangle.width / 2;
  y += window->rectangle.height;
  int border = get_border_total();
  x += border;
  y += border;
}

void display_output_wayland::draw_string_at(int x, int y, const char *s, int w) {
  struct window *window = global_window;
  //y += pango_fonts[selected_font].metrics.ascent;// + pango_fonts[selected_font].metrics.descent;
  adjust_coords(x, y);
  pango_layout_set_text(window->layout, s, strlen(s));
  cairo_save(window->cr);
  uint8_t r = current_color >> 24;
  uint8_t g = current_color >> 16;
  uint8_t b = current_color >> 8;
  cairo_set_source_rgba(window->cr, r, g, b, pango_fonts[selected_font].font_alpha / 65535.);
  cairo_move_to(window->cr, x, y);
  pango_cairo_show_layout(window->cr, window->layout);
  cairo_restore(window->cr);
}

void display_output_wayland::set_line_style(int w, bool solid) {
  struct window *window = global_window;
  static double dashes[2] = {1.0, 1.0};
  if(solid)
    cairo_set_dash(window->cr, nullptr, 0, 0);
  else
    cairo_set_dash(window->cr, dashes, 2, 0);
  cairo_set_line_width(window->cr, w);
}

void display_output_wayland::set_dashes(char *s) {
  struct window *window = global_window;
  size_t len = strlen(s);
  double* dashes = new double[len];
  for (size_t i = 0; i < len; i++) {
    dashes[i] = s[i];
  }
  cairo_set_dash(window->cr, dashes, len, 0);
  delete[] dashes;
}

void display_output_wayland::draw_line(int x1, int y1, int x2, int y2) {
  struct window *window = global_window;
  adjust_coords(x1, y1);
  adjust_coords(x2, y2);
  cairo_save(window->cr);
  cairo_move_to(window->cr, x1, y1);
  cairo_line_to(window->cr, x2, y2);
  cairo_stroke(window->cr);
  cairo_restore(window->cr);
}

static void do_rect(int x, int y, int w, int h, bool fill) {
  struct window *window = global_window;
  adjust_coords(x, y);
  cairo_save(window->cr);
  cairo_rectangle(window->cr, x, y + h * 3, w, h);
  if (fill) {
    cairo_fill(window->cr);
  }
  cairo_stroke(window->cr);
  cairo_restore(window->cr);
}

void display_output_wayland::draw_rect(int x, int y, int w, int h) {
  do_rect(x, y, w, h, false);
}

void display_output_wayland::fill_rect(int x, int y, int w, int h) {
  do_rect(x, y, w, h, true);
}

void display_output_wayland::draw_arc(int x, int y, int w, int h, int a1, int a2) {
  struct window *window = global_window;
  adjust_coords(x, y);
  cairo_save(window->cr);
  //cairo_move_to(cr, x, y);
  // cairo_new_sub_path() ?
  cairo_translate(window->cr, x + w / 2., y + h / 2.);
  cairo_scale(window->cr, w / 2., h / 2.);
  cairo_arc(window->cr, 0., 0., 1., a2, a2);
  cairo_restore(window->cr);
  //cairo_stroke();?
}

void display_output_wayland::move_win(int x, int y) {
  //window.x = x;
  //window.y = y;
  //TODO
}

int display_output_wayland::dpi_scale(int value) {
  return value;
}

void display_output_wayland::end_draw_stuff() {
  window_commit_buffer(global_window);
}

void display_output_wayland::clear_text(int exposures) {
	struct window *window = global_window;
	cairo_save (window->cr);
	cairo_set_source_rgba (window->cr, 0, 0, 0, 0);
	cairo_set_operator (window->cr, CAIRO_OPERATOR_SOURCE);
	cairo_paint (window->cr);
	cairo_restore (window->cr);
}

int display_output_wayland::font_height(unsigned int f) {
  if (pango_fonts.size() == 0) {
  	return 2;
  }
  assert(f < pango_fonts.size());
  return pango_fonts[f].metrics.ascent + pango_fonts[f].metrics.descent;
}

int display_output_wayland::font_ascent(unsigned int f) {
  if (pango_fonts.size() == 0) {
  	return 1;
  }
  assert(f < pango_fonts.size());
  return pango_fonts[f].metrics.ascent;
}

int display_output_wayland::font_descent(unsigned int f) {
  if (pango_fonts.size() == 0) {
  	return 1;
  }
  assert(f < pango_fonts.size());
  return pango_fonts[f].metrics.descent;
}

void display_output_wayland::setup_fonts(void) {
  /* Nothing to do here */
}

void display_output_wayland::set_font(unsigned int f) {
  assert(f < pango_fonts.size());
  if (pango_fonts.size() > f && pango_fonts[f].desc != nullptr) {
    pango_layout_set_font_description(global_window->layout, pango_fonts[f].desc);
  }
}

void display_output_wayland::free_fonts(bool utf8) {
  for (auto &font : pango_fonts) {
    if (font.desc != nullptr) { pango_font_description_free(font.desc); font.desc = nullptr; }
  }
  pango_fonts.clear();
}
void display_output_wayland::load_fonts(bool utf8) {
  pango_fonts.resize(fonts.size());
  for (unsigned int i = 0; i < fonts.size(); i++) {
    auto &font = fonts[i];
    auto &pango_font_entry = pango_fonts[i];
    FcPattern* fc_pattern = FcNameParse(reinterpret_cast<const unsigned char*>(font.name.c_str()));
    pango_font_entry.desc = pango_fc_font_description_from_pattern(fc_pattern, true);
    FcPatternDestroy(fc_pattern);
    PangoFont* pango_font = pango_context_load_font(global_window->pango_context, pango_font_entry.desc);
    PangoFontMetrics* font_metrics = pango_font_get_metrics(pango_font, nullptr);
    auto ascent = pango_font_metrics_get_ascent(font_metrics) / 1024;
    pango_font_entry.metrics.ascent = ascent;
    auto descent = pango_font_metrics_get_descent(font_metrics) / 1024;
    pango_font_entry.metrics.descent = descent;
    //printf("asc %d desc %d height: %d\n", ascent, descent, pango_font_metrics_get_height(font_metrics));
    //printf("%s: asc %d desc %d\n", font.name.c_str(), ascent, descent);
    //printf("size=%d\n", pango_font_description_get_size(pango_font_entry.desc));
    pango_font_metrics_unref(font_metrics);
    g_object_unref(pango_font);
  }
}


struct shm_pool {
	struct wl_shm_pool *pool;
	size_t size;
	size_t used;
	void *data;
};

struct shm_surface_data {
	struct wl_buffer *buffer;
	struct shm_pool *pool;
};

static const cairo_user_data_key_t shm_surface_data_key = {0};

struct wl_buffer *
get_buffer_from_cairo_surface(cairo_surface_t *surface)
{
	struct shm_surface_data *data;

	data = static_cast<struct shm_surface_data *>(cairo_surface_get_user_data(surface, &shm_surface_data_key));

	return data->buffer;
}

static void
shm_pool_destroy(struct shm_pool *pool);

static void
shm_surface_data_destroy(void *p)
{
	struct shm_surface_data *data = static_cast<struct shm_surface_data *>(p);
	wl_buffer_destroy(data->buffer);
	if (data->pool)
		shm_pool_destroy(data->pool);

	delete data;
}

static struct wl_shm_pool *
make_shm_pool(struct wl_shm *shm, int size, void **data)
{
	struct wl_shm_pool *pool;
	int fd;

	fd = os_create_anonymous_file(size);
	if (fd < 0) {
		fprintf(stderr, "creating a buffer file for %d B failed: %m\n",
			size);
		return NULL;
	}

	*data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (*data == MAP_FAILED) {
		fprintf(stderr, "mmap failed: %m\n");
		close(fd);
		return NULL;
	}

	pool = wl_shm_create_pool(shm, fd, size);

	close(fd);

	return pool;
}

static struct shm_pool *
shm_pool_create(struct wl_shm *shm, size_t size)
{
	struct shm_pool *pool = new struct shm_pool;

	if (!pool)
		return NULL;

	pool->pool = make_shm_pool(shm, size, &pool->data);
	if (!pool->pool) {
		delete pool;
		return NULL;
	}

	pool->size = size;
	pool->used = 0;

	return pool;
}

static void *
shm_pool_allocate(struct shm_pool *pool, size_t size, int *offset)
{
	if (pool->used + size > pool->size)
		return NULL;

	*offset = pool->used;
	pool->used += size;

	return (char *) pool->data + *offset;
}

/* destroy the pool. this does not unmap the memory though */
static void
shm_pool_destroy(struct shm_pool *pool)
{
	munmap(pool->data, pool->size);
	wl_shm_pool_destroy(pool->pool);
	delete pool;
}

static int
data_length_for_shm_surface(struct rectangle *rect)
{
	int stride;

	stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32,
						rect->width);
	return stride * rect->height;
}

static cairo_surface_t *
create_shm_surface_from_pool(void *none,
							struct rectangle *rectangle,
							struct shm_pool *pool)
{
	struct shm_surface_data *data;
	uint32_t format;
	cairo_surface_t *surface;
	cairo_format_t cairo_format;
	int stride, length, offset;
	void *map;

	data = new struct shm_surface_data;
	if (data == NULL)
		return NULL;

	cairo_format = CAIRO_FORMAT_ARGB32; /*or CAIRO_FORMAT_RGB16_565 who knows??*/

	stride = cairo_format_stride_for_width (cairo_format, rectangle->width);
	length = stride * rectangle->height;
	data->pool = NULL;
	map = shm_pool_allocate(pool, length, &offset);

	if (!map) {
		delete data;
		return NULL;
	}

	surface = cairo_image_surface_create_for_data (static_cast<unsigned char*>(map),
							cairo_format,
							rectangle->width,
							rectangle->height,
							stride);

	cairo_surface_set_user_data(surface, &shm_surface_data_key,
				    data, shm_surface_data_destroy);

	format = WL_SHM_FORMAT_ARGB8888; /*or WL_SHM_FORMAT_RGB565*/
	
	data->buffer = wl_shm_pool_create_buffer(pool->pool, offset,
							rectangle->width,
							rectangle->height,
							stride, format);

	return surface;
}

void
window_allocate_buffer(struct window *window) {
	assert(window->shm != nullptr);
	struct shm_pool *pool;
	pool = shm_pool_create(window->shm,
			       data_length_for_shm_surface(&window->rectangle));
	if (!pool) {
		printf("could not allocate shm pool\n");
		return;
	}

	window->cairo_surface =
		create_shm_surface_from_pool(window->shm, &window->rectangle, pool);

	if (!window->cairo_surface) {
		shm_pool_destroy(pool);
		return;
	}

	window->cr = cairo_create(window->cairo_surface);
	window->layout = pango_cairo_create_layout(window->cr);
	window->pango_context = pango_cairo_create_context(window->cr);

	/* make sure we destroy the pool when the surface is destroyed */
	struct shm_surface_data *data;
	data = static_cast<struct shm_surface_data *>(cairo_surface_get_user_data(window->cairo_surface, &shm_surface_data_key));
	data->pool = pool;
}

struct window *
window_create(struct wl_surface* surface, struct wl_shm* shm, int width, int height) {
	struct window *window;
	window = new struct window;

	window->rectangle.x = 0;
	window->rectangle.y = 0;
	window->rectangle.width = width;
	window->rectangle.height = height;

	window->surface = surface;
	window->shm = shm;

	window_allocate_buffer(window);

	return window;
}

void
window_free_buffer(struct window* window) {
	cairo_surface_destroy(window->cairo_surface);
	cairo_destroy(window->cr);
	g_object_unref(window->layout);
	g_object_unref(window->pango_context);
	window->cairo_surface = nullptr;
	window->cr = nullptr;
	window->layout = nullptr;
	window->pango_context = nullptr;
}

void
window_destroy(struct window *window) {
	cairo_surface_destroy(window->cairo_surface);
	cairo_destroy(window->cr);
	g_object_unref(window->layout);
	g_object_unref(window->pango_context);
}

void
window_resize(struct window *window, int width, int height) {
    printf("resize(%d x %d)\n", width, height);
	window_free_buffer(window);
	window->rectangle.width = width;
	window->rectangle.height = height;
	window_allocate_buffer(window);
}

void
window_commit_buffer(struct window *window) {
	assert(window->cairo_surface != nullptr);
	wl_surface_attach(window->surface, get_buffer_from_cairo_surface(window->cairo_surface), 0, 0);
	/* repaint all the pixels in the surface, change size to only repaint changed area*/
	wl_surface_damage(window->surface, window->rectangle.x,
							window->rectangle.y,
							window->rectangle.width,
							window->rectangle.height);
	wl_surface_commit(window->surface);
}

void
window_get_width_height(struct window *window, int *w, int *h) {
	*w = window->rectangle.width;
	*h = window->rectangle.height;
}

}  // namespace conky

#endif /* BUILD_WAYLAND */