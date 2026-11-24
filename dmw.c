/* See LICENSE file for copyright and license details.
 *
 * dwm made in wayland is a proper port/rewrite of the original suckless
 * dynamic window manager reimagined in a better display protocol. Unlike
 * dwl, this doesn't rely on wlroots (a terrible implementation of the
 * wayland protocol) and instead uses neuswc/neuwld which is much more 
 * minmal and modern.
 *
 * This tries to be 1:1 to dwm, however given the difference between X and
 * wayland, many of the functions had to be rewritten more so than others,
 * however the spirit of dwm is still felt. There is some reorganization
 * of the original functions, however you will be pleased to see that I
 * have tried to preserve as much as possible of the original dwm. 
 * Unfortunately, some features are lost however, due to neuswc's lack of
 * support of various popular wayland protocols (such as urgent tags).
 *
 * dmw runs on the Wayland event loop. When neuswc finds a new screen or
 * window it calls newscreen() or newwindow(). Each sets up a table of
 * handlers and gives neuswc the Monitor or Client they act on. That way
 * entered() gets the client under the pointer directly.
 *
 * Each toplevel window dmw manages is a Client. A dialog with a managed
 * parent starts out floating on its parent's monitor and tags. As in dwm,
 * each monitor keeps two lists of the same clients. One gives the layout
 * its tiling order, the other remembers focus so focusing a window leaves
 * its place in the layout alone. Each tag is one bit. A client is visible
 * when it shares a tag with the monitor's current view.
 *
 * Keys, buttons and rules live in config.h. When a binding runs spawn(),
 * the child starts a new session and runs the command. The event loop
 * reaps exited children on SIGCHLD so dmw can keep handling windows.
 *
 * To understand everything else start reading main().
 */
#include "bar.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <swc.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-server.h>
#include <wld/wld.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "util.h"

/* macros */
#define ISVISIBLE(c) ((c)->tags & (c)->mon->tagset[(c)->mon->seltags])
#define TAGMASK      ((1u << LENGTH(tags)) - 1)

/* enums */
enum { Button1 = 0x110, Button2 = 0x112, Button3 = 0x111 }; /* keep config.h button names portable across bsd/linux */
enum { Stacked, Tiled, Fullscreen }; /* window states */
enum { SchemeNorm, SchemeSel }; /* color schemes */
enum { ColFg, ColBg, ColBorder }; /* colors */
enum { ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle, ClkClientWin, ClkRootWin }; /* clicks */

typedef union {
	int i;
	unsigned int ui;
	float f;
	const void *v;
} Arg;

struct Monitor;
struct Layout {
	const char *symbol;
	void (*arrange)(struct Monitor *);
};
struct Rule {
	const char *app_id, *title;
	unsigned int tags;
	int isfloating, monitor;
};
struct Key {
	unsigned int mod, keysym;
	void (*func)(const Arg *);
	const Arg arg;
};
struct Button {
	unsigned int click, mask, button;
	void (*func)(const Arg *);
	const Arg arg;
};
struct Client {
	struct swc_window *win;
	struct Monitor *mon;
	struct Client *next, *snext;
	struct wl_event_source *pending;
	struct swc_rectangle saved;
	unsigned int tags;
	int isfloating, isfullscreen, mapped, mode;
};
struct Monitor {
	struct Bar bar;
	struct Client *clients, *stack, *sel;
	struct Monitor *next;
	const struct Layout *lt[2];
	unsigned int seltags, sellt, tagset[2];
	int num, showbar, nmaster;
	float mfact;
	char ltsymbol[32];
};

/* function declarations */
static void arrange(struct Monitor *);
static void focus(struct Client *);
static void focusmon(const Arg *), focusstack(const Arg *), incnmaster(const Arg *);
static void killclient(const Arg *), movemouse(const Arg *), quit(const Arg *);
static void resizemouse(const Arg *), setlayout(const Arg *), setmfact(const Arg *);
static void spawn(const Arg *), tag(const Arg *), tagmon(const Arg *), togglebar(const Arg *);
static void togglefloating(const Arg *), togglefullscreen(const Arg *), toggletag(const Arg *);
static void toggleview(const Arg *), view(const Arg *), zoom(const Arg *);
static void monocle(struct Monitor *), tile(struct Monitor *);

/* configuration */
#include "config.h"

/* compile-time check if all tags fit into an unsigned int bit array. */
struct NumTags { char limitexceeded[LENGTH(tags) > 31 ? -1 : 1]; };

/* global vars */
static struct Monitor *mons, *selmon;
static int shutting_down;
static struct wl_display *display;
static struct wl_event_loop *loop;
static uint32_t scheme[2][3];
static char status[256] = "dmw-" VERSION;
static struct {
	struct Client *c;
	struct wl_event_source *timer;
	struct swc_rectangle start;
	int x, y, resize, moved;
	uint32_t button;
} drag;

/* function implementations */
static struct Client *
findclient(struct swc_window *win)
{
	struct Monitor *m;
	struct Client *c;

	for (m = mons; m; m = m->next)
		for (c = m->clients; c; c = c->next)
			if (c->win == win)
				return c;
	return NULL;
}

static struct Client *
nexttiled(struct Client *c)
{
	for (; c && (c->isfloating || c->isfullscreen || !ISVISIBLE(c)); c = c->next);
	return c;
}

static void
detach(struct Client *c)
{
	struct Client **p;

	for (p = &c->mon->clients; *p != c; p = &(*p)->next);
	*p = c->next;
	for (p = &c->mon->stack; *p != c; p = &(*p)->snext);
	*p = c->snext;
	if (c->mon->sel == c)
		c->mon->sel = NULL;
}

static void
attach(struct Client *c, struct Monitor *m)
{
	c->mon = m;
	c->next = m->clients;
	m->clients = c;
	c->snext = m->stack;
	m->stack = c;
}

static void
drawbar(struct Monitor *m)
{
	bar_update(&m->bar, m->showbar && !(m->sel && m->sel->isfullscreen));
}

static void
restack(struct Monitor *m)
{
	struct Client *c = m->sel;

	if (c && ISVISIBLE(c))
		swc_window_raise(c->win);
}

static void
focus(struct Client *c)
{
	struct Client **p;
	struct Monitor *m;

	if (!selmon)
		return;
	if (!c || !ISVISIBLE(c))
		for (c = selmon->stack; c && !ISVISIBLE(c); c = c->snext);
	if (c) {
		selmon = c->mon;
		for (p = &selmon->stack; *p != c; p = &(*p)->snext);
		*p = c->snext;
		c->snext = selmon->stack;
		selmon->stack = c;
	}
	selmon->sel = c;
	restack(selmon);
	swc_window_focus(c ? c->win : NULL);
	for (m = mons; m; m = m->next) {
		struct Client *i;
		for (i = m->clients; i; i = i->next)
			swc_window_set_border(i->win, scheme[i == c][ColBorder],
			                      i->isfullscreen ? 0 : borderpx, 0, 0);
		drawbar(m);
	}
}

static int
dimension(int value, unsigned int min, unsigned int max)
{
	value = MAX(value, (int)barfont->height + 2);
	if (min && (unsigned int)value < min)
		value = min;
	if (max && (unsigned int)value > max)
		value = MAX(min, max);
	return value;
}

static int
place(struct Client *c, int x, int y, int w, int h)
{
	struct swc_rectangle g = { x + borderpx, y + borderpx,
		dimension(w - 2 * (int)borderpx, c->win->min_width, c->win->max_width),
		dimension(h - 2 * (int)borderpx, c->win->min_height, c->win->max_height) };

	swc_window_set_geometry(c->win, &g);
	return g.height + 2 * borderpx;
}

static void
tile(struct Monitor *m)
{
	struct swc_rectangle g = m->bar.screen->usable_geometry;
	struct Client *c;
	int i, n, h, mw, my, ty;

	for (n = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), n++);
	mw = n <= m->nmaster ? g.width : m->nmaster ? g.width * m->mfact : 0;
	for (i = my = ty = 0, c = nexttiled(m->clients); c; c = nexttiled(c->next), i++) {
		if (i < m->nmaster) {
			h = place(c, g.x, g.y + my, mw, ((int)g.height - my) / (MIN(n, m->nmaster) - i));
			if (my + h < (int)g.height)
				my += h;
		} else {
			h = place(c, g.x + mw, g.y + ty, g.width - mw, ((int)g.height - ty) / (n - i));
			if (ty + h < (int)g.height)
				ty += h;
		}
	}
}

static void
monocle(struct Monitor *m)
{
	struct swc_rectangle g = m->bar.screen->usable_geometry;
	struct Client *c;
	int n = 0;

	for (c = m->clients; c; c = c->next)
		if (ISVISIBLE(c))
			n++;
	if (n)
		snprintf(m->ltsymbol, sizeof m->ltsymbol, "[%d]", n);
	for (c = nexttiled(m->clients); c; c = nexttiled(c->next))
		place(c, g.x, g.y, g.width, g.height);
}

static void
arrange(struct Monitor *m)
{
	const struct Layout *lt = m->lt[m->sellt];
	struct Client *c;
	int mode;

	if (!m->sel || !ISVISIBLE(m->sel))
		for (m->sel = m->stack; m->sel && !ISVISIBLE(m->sel); m->sel = m->sel->snext);
	for (c = m->clients; c; c = c->next) {
		mode = c->isfullscreen ? Fullscreen : lt->arrange && !c->isfloating ? Tiled : Stacked;
		if (mode != c->mode) {
			if (mode == Fullscreen)
				swc_window_set_fullscreen(c->win, m->bar.screen);
			else if (mode == Tiled)
				swc_window_set_tiled(c->win);
			else
				swc_window_set_stacked(c->win);
			c->mode = mode;
			swc_window_raise(c->win);
		}
		if (ISVISIBLE(c)) {
			swc_window_show(c->win);
			if (c->isfullscreen)
				swc_window_set_geometry(c->win, &m->bar.screen->geometry);
		} else
			swc_window_hide(c->win);
	}
	snprintf(m->ltsymbol, sizeof m->ltsymbol, "%s", lt->symbol);
	if (lt->arrange)
		lt->arrange(m);
	restack(m);
	drawbar(m);
}

static struct Monitor *
dirtomon(int dir)
{
	struct Monitor *m;

	if (dir > 0)
		return selmon->next ? selmon->next : mons;
	for (m = mons; m->next && m->next != selmon; m = m->next);
	return m;
}

static void
focusmon(const Arg *arg)
{
	selmon = dirtomon(arg->i);
	focus(NULL);
}

static void
sendmon(struct Client *c, struct Monitor *m)
{
	struct Monitor *old = c->mon;

	if (m == old)
		return;
	detach(c);
	attach(c, m);
	c->tags = m->tagset[m->seltags];
	if (!drag.c)
		swc_window_set_position(c->win, m->bar.screen->usable_geometry.x + borderpx,
		                        m->bar.screen->usable_geometry.y + borderpx);
	focus(NULL);
	arrange(old);
	arrange(m);
}

static void
focusstack(const Arg *arg)
{
	struct Client *c = NULL, *i;

	if (!selmon->sel || (selmon->sel->isfullscreen && lockfullscreen))
		return;
	if (arg->i > 0) {
		for (c = selmon->sel->next; c && !ISVISIBLE(c); c = c->next);
		if (!c)
			for (c = selmon->clients; c && !ISVISIBLE(c); c = c->next);
	} else {
		for (i = selmon->clients; i != selmon->sel; i = i->next)
			if (ISVISIBLE(i))
				c = i;
		if (!c)
			for (; i; i = i->next)
				if (ISVISIBLE(i))
					c = i;
	}
	if (c)
		focus(c);
}

static void
incnmaster(const Arg *arg)
{
	selmon->nmaster = MAX(selmon->nmaster + arg->i, 0);
	arrange(selmon);
}

static void
setlayout(const Arg *arg)
{
	if (!arg || !arg->v || arg->v != selmon->lt[selmon->sellt])
		selmon->sellt ^= 1;
	if (arg && arg->v)
		selmon->lt[selmon->sellt] = arg->v;
	arrange(selmon);
}

static void
setmfact(const Arg *arg)
{
	float f;

	if (!arg || !selmon->lt[selmon->sellt]->arrange)
		return;
	f = arg->f < 1.0 ? arg->f + selmon->mfact : arg->f - 1.0;
	if (f < 0.05 || f > 0.95)
		return;
	selmon->mfact = f;
	arrange(selmon);
}

static void
tag(const Arg *arg)
{
	if (selmon->sel && arg->ui & TAGMASK) {
		selmon->sel->tags = arg->ui & TAGMASK;
		focus(NULL);
		arrange(selmon);
	}
}

static void
tagmon(const Arg *arg)
{
	if (!selmon->sel || !mons->next)
		return;
	sendmon(selmon->sel, dirtomon(arg->i));
}

static void
toggletag(const Arg *arg)
{
	unsigned int newtags;

	if (!selmon->sel)
		return;
	newtags = selmon->sel->tags ^ (arg->ui & TAGMASK);
	if (newtags) {
		selmon->sel->tags = newtags;
		focus(NULL);
		arrange(selmon);
	}
}

static void
toggleview(const Arg *arg)
{
	unsigned int newtagset = selmon->tagset[selmon->seltags] ^ (arg->ui & TAGMASK);

	if (newtagset) {
		selmon->tagset[selmon->seltags] = newtagset;
		focus(NULL);
		arrange(selmon);
	}
}

static void
view(const Arg *arg)
{
	if ((arg->ui & TAGMASK) == selmon->tagset[selmon->seltags])
		return;
	selmon->seltags ^= 1; /* toggle sel tagset */
	if (arg->ui & TAGMASK)
		selmon->tagset[selmon->seltags] = arg->ui & TAGMASK;
	focus(NULL);
	arrange(selmon);
}

static void
zoom(const Arg *arg)
{
	struct Client *c = selmon->sel;

	if (!selmon->lt[selmon->sellt]->arrange || !c || c->isfloating || c->isfullscreen)
		return;
	if (c == nexttiled(selmon->clients) && !(c = nexttiled(c->next)))
		return;
	detach(c);
	attach(c, selmon);
	focus(c);
	arrange(selmon);
}

static int
fixed(struct Client *c)
{
	return c->win->min_width && c->win->min_height &&
	       c->win->min_width == c->win->max_width &&
	       c->win->min_height == c->win->max_height;
}

static void
setfullscreen(void *data, bool on)
{
	struct Client *c = data;

	if (c->isfullscreen == on)
		return;
	if (on)
		swc_window_get_geometry(c->win, &c->saved);
	c->isfullscreen = on;
	arrange(c->mon);
	if (!on && c->mode == Stacked && c->saved.width && c->saved.height)
		swc_window_set_geometry(c->win, &c->saved);
	swc_window_set_border(c->win, scheme[c->mon == selmon && selmon->sel == c][ColBorder],
	                      on ? 0 : borderpx, 0, 0);
}

static void
togglefullscreen(const Arg *arg)
{
	if (selmon->sel)
		setfullscreen(selmon->sel, !selmon->sel->isfullscreen);
}

static void
togglefloating(const Arg *arg)
{
	struct Client *c = selmon->sel;
	if (c && !c->isfullscreen) {
		c->isfloating = !c->isfloating || fixed(c);
		arrange(selmon);
	}
}

static void
togglebar(const Arg *arg)
{
	selmon->showbar = !selmon->showbar;
	drawbar(selmon);
}

static void
killclient(const Arg *arg)
{
	if (selmon->sel)
		swc_window_close(selmon->sel->win);
}

static void
quit(const Arg *arg)
{
	wl_display_terminate(display);
}

static void
spawn(const Arg *arg)
{
	char *const *cmd = arg->v;
	pid_t pid;
	sigset_t mask;

	if ((pid = fork()) < 0) {
		perror("dmw: fork");
		return;
	}
	if (pid == 0) {
		setsid();
		sigemptyset(&mask);
		sigprocmask(SIG_SETMASK, &mask, NULL);
		if (!isatty(STDIN_FILENO)) {
			int fd = open("/dev/null", O_RDONLY);
			if (fd >= 0 && fd != STDIN_FILENO) {
				dup2(fd, STDIN_FILENO);
				close(fd);
			}
		}
		execvp(cmd[0], cmd);
		perror(cmd[0]);
		_exit(127);
	}
}

static void
pointerpos(int *x, int *y)
{
	int32_t fx, fy;

	swc_cursor_position(&fx, &fy);
	*x = wl_fixed_to_int(fx);
	*y = wl_fixed_to_int(fy);
}

static struct Monitor *
pointmon(int x, int y)
{
	struct Monitor *m;
	struct swc_rectangle g;

	for (m = mons; m; m = m->next) {
		g = m->bar.screen->geometry;
		if (x >= g.x && y >= g.y && x - g.x < (int)g.width && y - g.y < (int)g.height)
			return m;
	}
	return selmon;
}

static int
dragmotion(void *data)
{
	struct Client *c = drag.c;
	struct swc_rectangle g = drag.start, area;
	int x, y, dx, dy, right, bottom;

	if (!c || c->isfullscreen || !ISVISIBLE(c))
		return 0;
	pointerpos(&x, &y);
	area = pointmon(x, y)->bar.screen->usable_geometry;
	dx = x - drag.x;
	dy = y - drag.y;
	if (!drag.moved && c->mode == Tiled && MAX(abs(dx), abs(dy)) <= (int)snap)
		goto next;
	if (!drag.moved && (dx || dy)) {
		if (c->mode == Tiled) {
			c->isfloating = 1;
			arrange(c->mon);
		}
		drag.moved = 1;
	}
	if (!drag.moved)
		goto next;
	if (drag.resize) {
		g.width = dimension((int)g.width + dx, c->win->min_width, c->win->max_width);
		g.height = dimension((int)g.height + dy, c->win->min_height, c->win->max_height);
		swc_window_set_size(c->win, g.width, g.height);
	} else {
		g.x += dx;
		g.y += dy;
		right = area.x + (int)area.width - borderpx - g.width;
		bottom = area.y + (int)area.height - borderpx - g.height;
		if (abs(g.x - area.x - (int)borderpx) < (int)snap)
			g.x = area.x + borderpx;
		else if (abs(g.x - right) < (int)snap)
			g.x = right;
		if (abs(g.y - area.y - (int)borderpx) < (int)snap)
			g.y = area.y + borderpx;
		else if (abs(g.y - bottom) < (int)snap)
			g.y = bottom;
		swc_window_set_position(c->win, g.x, g.y);
	}
next:
	wl_event_source_timer_update(drag.timer, MAX(1, 1000 / MAX(1, refreshrate)));
	return 0;
}

static void
startdrag(int resize)
{
	struct Client *c = selmon->sel;
	if (drag.c || !c || c->isfullscreen || !swc_window_get_geometry(c->win, &drag.start))
		return;
	drag.c = c;
	drag.resize = resize;
	drag.moved = 0;
	pointerpos(&drag.x, &drag.y);
	dragmotion(NULL);
}

static void
movemouse(const Arg *arg)
{
	startdrag(0);
}

static void
resizemouse(const Arg *arg)
{
	startdrag(1);
}

struct Bar *
bar_for_screen(struct swc_screen *screen)
{
	struct Monitor *m;

	for (m = mons; m; m = m->next)
		if (m->bar.screen == screen)
			return &m->bar;
	return NULL;
}

static int
label(struct wld_renderer *r, int x, int w, int pad, const char *s, int selected, int box, int filled)
{
	int h = barfont->height + 2;
	int size = MAX(2, h / 6);
	uint32_t *col = scheme[selected];

	if (r && w > 0) {
		wld_set_clip(r, x, 0, w, h);
		wld_fill_rectangle(r, col[ColBg], x, 0, w, h);
		wld_draw_text(r, barfont, col[ColFg], x + pad,
		              (h - (int)barfont->height) / 2 + barfont->ascent, s, -1, NULL);
		if (box) {
			wld_fill_rectangle(r, col[ColFg], x + 1, 1, size, size);
			if (!filled && size > 2)
				wld_fill_rectangle(r, col[ColBg], x + 2, 2, size - 2, size - 2);
		}
	}
	return x + w;
}

static unsigned int
baritems(struct Monitor *m, struct wld_renderer *r, int px, Arg *arg)
{
	struct Client *c;
	unsigned int i, occ = 0, bit;
	int x = 0, w = m->bar.width, end, pad = barfont->height;

	for (c = m->clients; c; c = c->next)
		occ |= c->tags;
	for (i = 0; i < LENGTH(tags); i++) {
		bit = 1u << i;
		x = label(r, x, textwidth(tags[i]) + pad, pad / 2, tags[i], !!(m->tagset[m->seltags] & bit),
		          occ & bit, m == selmon && m->sel && (m->sel->tags & bit));
		if (px >= 0 && px < x) {
			arg->ui = bit;
			return ClkTagBar;
		}
	}
	x = label(r, x, textwidth(m->ltsymbol) + pad, pad / 2, m->ltsymbol, 0, 0, 0);
	if (px >= 0 && px < x)
		return ClkLtSymbol;
	end = m == selmon ? MAX(x, w - textwidth(status) - 2) : w;
	c = m->sel;
	label(r, x, end - x, pad / 2, c && c->win->title ? c->win->title : "",
	      m == selmon && c, c && c->isfloating, c && fixed(c));
	if (end < w)
		label(r, end, w - end, 0, status, 0, 0, 0);
	return px >= end ? ClkStatusText : ClkWinTitle;
}

void
bar_paint(struct Bar *bar, struct wld_renderer *r)
{
	struct Monitor *m = wl_container_of(bar, m, bar);
	baritems(m, r, -1, NULL);
}

static void
click(unsigned int region, unsigned int mask, unsigned int button, const Arg *tagarg)
{
	size_t i;
	for (i = 0; i < LENGTH(buttons); i++) {
		const struct Button *b = &buttons[i];
		if (b->click == region && b->mask == mask && b->button == button && b->func)
			b->func(region == ClkTagBar && !b->arg.ui ? tagarg : &b->arg);
	}
}

static void
clickbar(struct Monitor *m, int x, unsigned int mask, unsigned int button)
{
	Arg arg = {0};
	unsigned int region;

	if (selmon != m) {
		selmon = m;
		focus(NULL);
	}
	region = baritems(m, NULL, x, &arg);
	click(region, mask, button, &arg);
}

void
bar_click(struct Bar *bar, int x, uint32_t button)
{
	struct Monitor *m = wl_container_of(bar, m, bar);
	clickbar(m, x, 0, button);
}

static void
buttonbinding(void *data, uint32_t time, uint32_t button, uint32_t state)
{
	const struct Button *binding = data;
	struct Client *c;
	struct Monitor *m;
	struct swc_rectangle g;
	int x, y, by;

	pointerpos(&x, &y);
	m = pointmon(x, y);
	if (!state) {
		if (drag.c && button == drag.button) {
			c = drag.c;
			dragmotion(NULL);
			if (drag.moved && m != c->mon) {
				sendmon(c, m);
				focus(c);
			}
			drag.c = NULL;
		}
		return;
	}
	if (!m)
		return;
	g = m->bar.screen->geometry;
	by = topbar ? g.y : g.y + (int)g.height - m->bar.height;
	if (m->bar.shown && y >= by && y < by + m->bar.height) {
		clickbar(m, x - g.x, binding->mask, button);
		return;
	}
	c = findclient(swc_window_at(x, y));
	if (c)
		focus(c);
	if (!drag.c)
		drag.button = button;
	click(c ? ClkClientWin : ClkRootWin, binding->mask, button, NULL);
}

static void
keybinding(void *data, uint32_t time, uint32_t key, uint32_t state)
{
	const struct Key *k = data;
	if (state && selmon && k->func)
		k->func(&k->arg);
}

static void
bindings(void)
{
	size_t i, j;
	for (i = 0; i < LENGTH(keys); i++)
		if (swc_add_binding(SWC_BINDING_KEY, keys[i].mod, keys[i].keysym, keybinding, (void *)&keys[i]) < 0)
			die("dmw: cannot bind keysym %#x", keys[i].keysym);
	for (i = 0; i < LENGTH(buttons); i++) {
		if (!buttons[i].mask && buttons[i].click < ClkClientWin)
			continue;
		for (j = 0; j < i; j++)
			if (buttons[j].mask == buttons[i].mask && buttons[j].button == buttons[i].button &&
			    (buttons[j].mask || buttons[j].click >= ClkClientWin))
				break;
		if (j == i && swc_add_binding(SWC_BINDING_BUTTON, buttons[i].mask, buttons[i].button,
		                              buttonbinding, (void *)&buttons[i]) < 0)
			die("dmw: cannot bind button %u", buttons[i].button);
	}
}

static void
applyrules(struct Client *c)
{
	struct Monitor *m, *target = c->mon;
	struct Client *parent = findclient(c->win->parent);
	const struct Rule *r;
	unsigned int mask = 0;

	if (parent) {
		target = parent->mon;
		mask = parent->tags;
		c->isfloating = 1;
	} else {
		for (r = rules; r < rules + LENGTH(rules); r++) {
			if ((r->title && (!c->win->title || !strstr(c->win->title, r->title))) ||
			    (r->app_id && (!c->win->app_id || !strstr(c->win->app_id, r->app_id))))
				continue;
			c->isfloating = r->isfloating;
			mask |= r->tags;
			for (m = mons; m; m = m->next)
				if (m->num == r->monitor)
					target = m;
		}
	}
	if (target != c->mon) {
		m = c->mon;
		detach(c);
		attach(c, target);
		arrange(m);
	}
	c->tags = mask & TAGMASK ? mask & TAGMASK : target->tagset[target->seltags];
	c->isfloating |= fixed(c);
}

static void
manage(void *data)
{
	struct Client *c = data;
	struct swc_rectangle g;

	c->pending = NULL;
	applyrules(c);
	g = c->mon->bar.screen->usable_geometry;
	swc_window_set_border(c->win, scheme[SchemeNorm][ColBorder], c->isfullscreen ? 0 : borderpx, 0, 0);
	swc_window_set_position(c->win, g.x + borderpx, g.y + borderpx);
	if (c->mon == selmon && ISVISIBLE(c))
		focus(c);
	else if (ISVISIBLE(c))
		c->mon->sel = c;
	arrange(c->mon);
}

static void
parentchanged(void *data)
{
	struct Client *c = data;

	if (!c->pending && c->win->parent) {
		applyrules(c);
		arrange(c->mon);
		focus(NULL);
	}
}

static void
metadatachanged(void *data)
{
	struct Client *c = data;

	if (!c->pending && !c->mapped) {
		applyrules(c);
		if (c->mon == selmon)
			focus(ISVISIBLE(c) ? c : NULL);
		arrange(c->mon);
	} else
		drawbar(c->mon);
}

static void
sizechanged(void *data)
{
	struct Client *c = data;
	struct swc_rectangle g;

	if (!c->mapped && swc_window_get_geometry(c->win, &g) && g.width && g.height) {
		c->mapped = 1;
		if (fixed(c) && !c->isfloating) {
			c->isfloating = 1;
			arrange(c->mon);
		}
	}
}

static void
entered(void *data)
{
	struct Client *c = data;
	if (!drag.c && !c->pending && ISVISIBLE(c) && (c->mon != selmon || selmon->sel != c))
		focus(c);
}

static void
unmanage(void *data)
{
	struct Client *c = data;
	struct Monitor *m = c->mon;

	if (drag.c == c)
		drag.c = NULL;
	if (c->pending)
		wl_event_source_remove(c->pending);
	detach(c);
	free(c);
	if (shutting_down)
		return;
	if (m == selmon)
		focus(NULL);
	arrange(m);
}

static void
newwindow(struct swc_window *win)
{
	static const struct swc_window_handler handler = {
		.destroy = unmanage, .entered = entered, .title_changed = metadatachanged,
		.app_id_changed = metadatachanged, .parent_changed = parentchanged,
		.size_changed = sizechanged,
	};
	struct Client *c;

	if (!selmon)
		return;
	c = ecalloc(1, sizeof *c);
	c->win = win;
	c->mode = -1;
	attach(c, selmon);
	swc_window_set_handler(win, &handler, c);
#ifdef SWC_WINDOW_FULLSCREEN_REQUEST
	swc_window_set_fullscreen_handler(win, setfullscreen);
#endif
	win->motion_throttle_ms = 1000 / MAX(1, refreshrate);
	/* wait for the title and app id before matching rules for a new toplevel: */
	c->pending = wl_event_loop_add_idle(loop, manage, c);
	if (!c->pending)
		die("dmw: scheduling new window:");
}

static void
screenentered(void *data)
{
	if (selmon != data && !drag.c) {
		selmon = data;
		focus(NULL);
	}
}

static void
screenchanged(void *data)
{
	struct Monitor *m = data;
	struct swc_rectangle area = m->bar.screen->usable_geometry, g;
	struct Client *c;
	int x, y;

	for (c = m->clients; c; c = c->next) {
		if (c->mode != Stacked || !swc_window_get_geometry(c->win, &g))
			continue;
		x = area.x + (int)borderpx;
		y = area.y + (int)borderpx;
		swc_window_set_position(c->win,
		    MAX(x, MIN(g.x, area.x + (int)area.width - (int)g.width - (int)borderpx)),
		    MAX(y, MIN(g.y, area.y + (int)area.height - (int)g.height - (int)borderpx)));
	}
	arrange(m);
}

static void
screenclosed(void *data)
{
	struct Monitor *m = data, **p;
	struct Client *c;

	for (p = &mons; *p != m; p = &(*p)->next);
	*p = m->next;
	if (selmon == m)
		selmon = mons;
	while ((c = m->clients) && mons) {
		detach(c);
		attach(c, mons);
		c->tags = mons->tagset[mons->seltags];
	}
	if (c)
		die("dmw: last output removed");
	bar_close(&m->bar);
	free(m);
	if (mons && !shutting_down) {
		arrange(mons);
		focus(NULL);
	}
}

static void
newscreen(struct swc_screen *screen)
{
	static const struct swc_screen_handler handler = {
		.destroy = screenclosed, .entered = screenentered,
		.geometry_changed = screenchanged, .usable_geometry_changed = screenchanged,
	};
	struct Monitor **p, *m = ecalloc(1, sizeof *m);

	for (p = &mons; *p; p = &(*p)->next)
		m->num = (*p)->num + 1;
	*p = m;
	m->bar.screen = screen;
	m->mfact = mfact;
	m->nmaster = nmaster;
	m->showbar = showbar;
	m->tagset[0] = m->tagset[1] = 1;
	m->lt[0] = &layouts[0];
	m->lt[1] = &layouts[1 % LENGTH(layouts)];
	if (!selmon)
		selmon = m;
	swc_screen_set_handler(screen, &handler, m);
	arrange(m);
}

static struct wl_event_source *status_source;

static int
readstatus(int fd, uint32_t events, void *data)
{
	static char line[sizeof status];
	static size_t len;
	struct Monitor *m;
	char buf[4096];
	ssize_t i, n;

	n = read(fd, buf, sizeof buf);
	if (n < 0 && (errno == EINTR || errno == EAGAIN))
		return 0;
	for (i = 0; i < n; i++) {
		if (buf[i] == '\n') {
			line[len] = '\0';
			memcpy(status, line, len + 1);
			len = 0;
		} else if (len + 1 < sizeof line)
			line[len++] = buf[i];
	}
	if (n <= 0) {
		if (len) {
			line[len] = '\0';
			memcpy(status, line, len + 1);
		}
		if (status_source) {
			wl_event_source_remove(status_source);
			status_source = NULL;
		}
	}
	for (m = mons; m; m = m->next)
		drawbar(m);
	return n > 0;
}

static int
signalhandler(int sig, void *data)
{
	if (sig == SIGCHLD)
		while (waitpid(-1, NULL, WNOHANG) > 0);
	else
		quit(NULL);
	return 0;
}

int
main(int argc, char **argv)
{
	static const struct swc_manager manager = { .new_screen = newscreen, .new_window = newwindow };
	const char *socket;
	struct stat st;
	struct wl_event_source *signals[3];
	int i, j, sig[] = { SIGCHLD, SIGTERM, SIGINT };

	if (argc == 2 && !strcmp(argv[1], "-v")) {
		puts("dmw-" VERSION);
		return 0;
	}
	if (argc != 1)
		die("usage: dmw [-v]");
	if (!(display = wl_display_create()))
		die("dmw: cannot create Wayland display");
	loop = wl_display_get_event_loop(display);
	for (i = 0; i < 2; i++)
		for (j = 0; j < 3; j++)
			if (!wld_lookup_named_color(colors[i][j], &scheme[i][j]))
				die("dmw: unknown color %s", colors[i][j]);
	if (!swc_initialize(display, loop, &manager))
		die("dmw: cannot initialize neuswc");
	if (!(socket = wl_display_add_socket_auto(display)) || setenv("WAYLAND_DISPLAY", socket, 1) < 0)
		die("dmw: cannot publish Wayland socket:");
	unsetenv("WAYLAND_SOCKET");
	bar_init(display, font, topbar);
	bindings();
	drag.timer = wl_event_loop_add_timer(loop, dragmotion, NULL);
	if (!drag.timer)
		die("dmw: cannot create drag timer:");
	for (i = 0; i < 3; i++)
		if (!(signals[i] = wl_event_loop_add_signal(loop, sig[i], signalhandler, NULL)))
			die("dmw: cannot watch signal:");
	if (!fstat(STDIN_FILENO, &st)) {
		if (S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode)) {
			status_source = wl_event_loop_add_fd(loop, STDIN_FILENO, WL_EVENT_READABLE, readstatus, NULL);
			if (!status_source)
				die("dmw: cannot watch status input:");
		} else if (S_ISREG(st.st_mode))
			while (readstatus(STDIN_FILENO, 0, NULL));
	}
	wl_display_run(display);
	shutting_down = 1;
	for (struct Monitor *m = mons; m; m = m->next)
		bar_close(&m->bar);
	bar_finish();
	wl_display_destroy_clients(display);
	for (i = 0; i < 3; i++)
		wl_event_source_remove(signals[i]);
	if (status_source)
		wl_event_source_remove(status_source);
	wl_event_source_remove(drag.timer);
	swc_finalize();
	wl_display_destroy(display);
	return 0;
}
