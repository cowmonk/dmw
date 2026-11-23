/* See LICENSE file for copyright and license details. */
#include <stdint.h>

struct swc_screen;
struct wl_display;
struct wld_font;
struct wld_renderer;

struct Bar {
	struct swc_screen *screen;
	struct wl_surface *surface;
	struct swc_panel *panel;
	struct swc_screen *output;
	struct Canvas *buffers;
	struct wl_event_source *idle;
	int width, height, shown, wanted;
};

void bar_init(struct wl_display *, const char *, int);
void bar_finish(void);
void bar_close(struct Bar *);
void bar_update(struct Bar *, int);
int textwidth(const char *);
extern struct wld_font *barfont;

struct Bar *bar_for_screen(struct swc_screen *);
void bar_paint(struct Bar *, struct wld_renderer *);
void bar_click(struct Bar *, int, uint32_t);
