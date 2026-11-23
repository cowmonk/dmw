/* See LICENSE file for copyright and license details. */
#include "bar.h"
#include "swc-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-server-core.h>
#include <wld/wld.h>
#include <wld/pixman.h>

#include "util.h"

struct Canvas {
	struct wl_buffer *buffer;
	struct wld_buffer *pixels;
	void *addr;
	size_t size;
	int width, busy;
	struct Canvas *next;
};

static struct wl_client *peer;
static struct wl_display *client;
static struct wl_event_loop *loop;
static struct wl_event_source *source;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_shm *shm;
static struct swc_panel_manager *panels;
static struct wld_renderer *renderer;
static struct wld_font_context *fonts;
struct wld_font *barfont;
static struct Bar *hover;
static int pointerx, top;

static void
flush(void)
{
	uint32_t events = WL_EVENT_READABLE;

	if (wl_display_flush(client) < 0) {
		if (errno != EAGAIN)
			die("dmw: bar connection:");
		events |= WL_EVENT_WRITABLE;
	}
	wl_event_source_fd_update(source, events);
}

static int
dispatch(int fd, uint32_t events, void *data)
{
	if (events & (WL_EVENT_HANGUP | WL_EVENT_ERROR) ||
	    ((events & WL_EVENT_READABLE) && wl_display_dispatch(client) < 0))
		die("dmw: bar disconnected");
	flush();
	return 0;
}

static void
released(void *data, struct wl_buffer *buffer)
{
	struct Canvas *c = data;
	c->busy = 0;
}

static struct Canvas *
canvas(struct Bar *b)
{
	static const struct wl_buffer_listener listener = { .release = released };
	char path[] = "/tmp/dmw-bar-XXXXXX";
	struct Canvas *c;
	struct wl_shm_pool *pool;
	int fd;

	for (c = b->buffers; c; c = c->next)
		if (!c->busy && c->width == b->width)
			return c;
	c = ecalloc(1, sizeof *c);
	c->size = (size_t)b->width * 4 * b->height;
	if ((fd = mkstemp(path)) < 0)
		die("dmw: creating bar buffer:");
	unlink(path);
	if (ftruncate(fd, c->size) < 0 ||
	    (c->addr = mmap(NULL, c->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED)
		die("dmw: mapping bar:");
	pool = wl_shm_create_pool(shm, fd, c->size);
	close(fd);
	c->width = b->width;
	c->buffer = wl_shm_pool_create_buffer(pool, 0, b->width, b->height, b->width * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	c->pixels = wld_import_buffer(wld_pixman_context, WLD_OBJECT_DATA, (union wld_object){ .ptr = c->addr },
	                              b->width, b->height, WLD_FORMAT_XRGB8888, b->width * 4);
	if (!c->pixels)
		die("dmw: cannot import bar pixels");
	wl_buffer_add_listener(c->buffer, &listener, c);
	c->next = b->buffers;
	return b->buffers = c;
}

static void
freecanvas(struct Canvas *c)
{
	wl_buffer_destroy(c->buffer);
	wld_buffer_unreference(c->pixels);
	munmap(c->addr, c->size);
	free(c);
}

static void
paint(void *data)
{
	struct Bar *b = data;
	struct Canvas *c, **p;

	b->idle = NULL;
	for (p = &b->buffers; (c = *p); ) {
		if (c->width != b->width) {
			*p = c->next;
			freecanvas(c);
		} else
			p = &c->next;
	}
	if (b->shown != b->wanted) {
		b->shown = b->wanted;
		swc_panel_set_strut(b->panel, b->shown ? b->height : 0, 0, b->width);
	}
	if (b->shown) {
		c = canvas(b);
		if (!wld_set_target_buffer(renderer, c->pixels))
			die("dmw: cannot draw bar");
		bar_paint(b, renderer);
		wld_flush(renderer);
		wl_surface_attach(b->surface, c->buffer, 0, 0);
		wl_surface_damage(b->surface, 0, 0, b->width, b->height);
		c->busy = 1;
	} else
		wl_surface_attach(b->surface, NULL, 0, 0);
	wl_surface_commit(b->surface);
	flush();
}

void
bar_update(struct Bar *b, int show)
{
	b->wanted = show;
	if (client && b->width && !b->idle)
		b->idle = wl_event_loop_add_idle(loop, paint, b);
}

static void
docked(void *data, struct swc_panel *panel, uint32_t length)
{
	struct Bar *b = data;
	b->width = length;
	bar_update(b, b->wanted);
}

static void
screenbound(void *data, struct wl_callback *sync, uint32_t serial)
{
	static const struct swc_panel_listener listener = { .docked = docked };
	struct swc_screen *output = data;
	struct wl_resource *res;
	struct Bar *b;

	wl_callback_destroy(sync);
	/* the panel protocol omits output coordinates, so match screens by resource: */
	res = wl_client_get_object(peer, wl_proxy_get_id((struct wl_proxy *)output));
	b = res ? bar_for_screen(wl_resource_get_user_data(res)) : NULL;
	if (!b) {
		swc_screen_destroy(output);
		return;
	}
	b->output = output;
	b->height = barfont->height + 2;
	b->surface = wl_compositor_create_surface(compositor);
	wl_surface_set_user_data(b->surface, b);
	b->panel = swc_panel_manager_create_panel(panels, b->surface);
	swc_panel_add_listener(b->panel, &listener, b);
	swc_panel_dock(b->panel, top ? SWC_PANEL_EDGE_TOP : SWC_PANEL_EDGE_BOTTOM, output, 0);
}

static void
enter(void *data, struct wl_pointer *ptr, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	hover = s ? wl_surface_get_user_data(s) : NULL;
	pointerx = wl_fixed_to_int(x);
}

static void
leave(void *data, struct wl_pointer *ptr, uint32_t serial, struct wl_surface *s)
{
	hover = NULL;
}

static void
motion(void *data, struct wl_pointer *ptr, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	pointerx = wl_fixed_to_int(x);
}

static void
button(void *data, struct wl_pointer *ptr, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	if (hover && state == WL_POINTER_BUTTON_STATE_PRESSED)
		bar_click(hover, pointerx, key);
}

static void
axis(void *data, struct wl_pointer *ptr, uint32_t time, uint32_t axis, wl_fixed_t value)
{
}

static void
capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	static const struct wl_pointer_listener listener = {
		.enter = enter, .leave = leave, .motion = motion, .button = button, .axis = axis,
	};
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_destroy(pointer);
		pointer = NULL;
		hover = NULL;
	}
}

static void
global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	static const struct wl_seat_listener seat_listener = { .capabilities = capabilities };
	static const struct wl_callback_listener bound = { .done = screenbound };
	struct swc_screen *output;

	if (!strcmp(iface, "wl_shm"))
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "wl_compositor"))
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, 1);
	else if (!strcmp(iface, "swc_panel_manager"))
		panels = wl_registry_bind(reg, name, &swc_panel_manager_interface, 1);
	else if (!strcmp(iface, "swc_screen")) {
		output = wl_registry_bind(reg, name, &swc_screen_interface, 1);
		wl_callback_add_listener(wl_display_sync(client), &bound, output);
	} else if (!strcmp(iface, "wl_seat")) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}

static void
removed(void *data, struct wl_registry *reg, uint32_t name)
{
}

void
bar_init(struct wl_display *display, const char *font, int topbar)
{
	static const struct wl_registry_listener listener = { global, removed };
	int fds[2];

	top = topbar;
	loop = wl_display_get_event_loop(display);
	fonts = wld_font_create_context();
	if (!fonts || !(barfont = wld_font_open_name(fonts, font)) ||
	    !(renderer = wld_create_renderer(wld_pixman_context)))
		die("dmw: cannot initialize bar font %s", font);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
		die("dmw: creating bar connection:");
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	if (!(peer = wl_client_create(display, fds[0])) ||
	    !(client = wl_display_connect_to_fd(fds[1])))
		die("dmw: cannot connect bar");
	source = wl_event_loop_add_fd(loop, fds[1], WL_EVENT_READABLE, dispatch, NULL);
	if (!source)
		die("dmw: cannot watch bar connection:");
	registry = wl_display_get_registry(client);
	wl_registry_add_listener(registry, &listener, NULL);
	flush();
}

int
textwidth(const char *s)
{
	struct wld_extents ext;
	wld_font_text_extents(barfont, s, &ext);
	return ext.advance;
}

void
bar_close(struct Bar *b)
{
	struct Canvas *c;
	if (hover == b)
		hover = NULL;
	if (b->idle)
		wl_event_source_remove(b->idle);
	while ((c = b->buffers)) {
		b->buffers = c->next;
		freecanvas(c);
	}
	if (b->panel) {
		swc_panel_destroy(b->panel);
		wl_surface_destroy(b->surface);
		swc_screen_destroy(b->output);
	}
	b->panel = NULL;
	b->width = 0;
	b->idle = NULL;
	if (client)
		flush();
}

void
bar_finish(void)
{
	if (pointer)
		wl_pointer_destroy(pointer);
	if (seat)
		wl_seat_destroy(seat);
	if (shm)
		wl_shm_destroy(shm);
	if (panels)
		swc_panel_manager_destroy(panels);
	if (compositor)
		wl_compositor_destroy(compositor);
	wl_registry_destroy(registry);
	wl_event_source_remove(source);
	wl_display_disconnect(client);
	client = NULL;
	wld_destroy_renderer(renderer);
	wld_font_close(barfont);
	wld_font_destroy_context(fonts);
}
