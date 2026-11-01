/* Copyright 2021-2023 nsxiv contributors
 *
 * This file is a part of rsxiv.
 *
 * rsxiv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * rsxiv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with rsxiv.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Wallpaper support, modelled after feh's --bg-* options.
 *
 * The image is rendered into a pixmap the size of the screen, which is
 * installed as the root window background and advertised through the
 * _XROOTPMAP_ID / ESETROOT_PMAP_ID properties (the Esetroot convention, so
 * compositors and transparent terminals pick it up). The pixmap is created
 * on a private X connection that is closed with RetainPermanent, so it
 * outlives rsxiv.
 */

#include "rsxiv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

static const char *const wp_flags[WP_COUNT] = {
	"--bg-center", "--bg-fill", "--bg-max", "--bg-scale", "--bg-tile"
};

static int wp_xerror(Display *dpy, XErrorEvent *ev)
{
	(void)dpy;
	(void)ev;
	return 0;
}

/* scale (w, h) to fit inside (bw, bh) or, if cover is set, to cover it */
static void wp_fit(int w, int h, int bw, int bh, bool cover, int *nw, int *nh)
{
	/* compare aspect ratios without floating point: bw/w vs bh/h */
	bool width_limited = (long long)bw * h <= (long long)bh * w;

	if (width_limited != cover) {
		*nw = bw;
		*nh = (int)(((long long)h * bw + (cover ? w - 1 : 0)) / w);
	} else {
		*nh = bh;
		*nw = (int)(((long long)w * bh + (cover ? h - 1 : 0)) / h);
	}
	if (*nw < 1)
		*nw = 1;
	if (*nh < 1)
		*nh = 1;
}

/* draw im onto the drawable (pm) according to mode. Imlib's context must
 * already point at the display and drawable.
 */
static void wp_render(Imlib_Image im, int sw, int sh, wpmode_t mode)
{
	int w, h, x, y, nw, nh;

	imlib_context_set_image(im);
	w = imlib_image_get_width();
	h = imlib_image_get_height();

	switch (mode) {
	case WP_CENTER:
		imlib_render_image_on_drawable((sw - w) / 2, (sh - h) / 2);
		break;
	case WP_FILL:
	case WP_MAX:
		wp_fit(w, h, sw, sh, mode == WP_FILL, &nw, &nh);
		imlib_render_image_on_drawable_at_size((sw - nw) / 2, (sh - nh) / 2, nw, nh);
		break;
	case WP_SCALE:
		imlib_render_image_on_drawable_at_size(0, 0, sw, sh);
		break;
	case WP_TILE:
		for (y = 0; y < sh; y += h) {
			for (x = 0; x < sw; x += w)
				imlib_render_image_on_drawable(x, y);
		}
		break;
	case WP_COUNT:
		break;
	}
}

/* publish pm as the root pixmap, like Esetroot does */
static void wp_set_root(Display *dpy, Window root, Pixmap pm)
{
	Atom prop_root = XInternAtom(dpy, "_XROOTPMAP_ID", False);
	Atom prop_eset = XInternAtom(dpy, "ESETROOT_PMAP_ID", False);
	Atom type;
	int format;
	unsigned long length, after;
	unsigned char *data_root = NULL, *data_eset = NULL;
	int (*old_handler)(Display *, XErrorEvent *);

	old_handler = XSetErrorHandler(wp_xerror);

	/* free the pixmap of the previous wallpaper, but only if it was set
	 * following the Esetroot convention (both properties agree).
	 */
	XGetWindowProperty(dpy, root, prop_root, 0L, 1L, False, AnyPropertyType,
	                   &type, &format, &length, &after, &data_root);
	if (data_root != NULL && type == XA_PIXMAP && format == 32 && length == 1) {
		XGetWindowProperty(dpy, root, prop_eset, 0L, 1L, False, AnyPropertyType,
		                   &type, &format, &length, &after, &data_eset);
		if (data_eset != NULL && type == XA_PIXMAP && format == 32 && length == 1 &&
		    *(Pixmap *)data_root == *(Pixmap *)data_eset)
			XKillClient(dpy, *(Pixmap *)data_root);
	}
	if (data_root != NULL)
		XFree(data_root);
	if (data_eset != NULL)
		XFree(data_eset);

	XChangeProperty(dpy, root, prop_root, XA_PIXMAP, 32, PropModeReplace,
	                (unsigned char *)&pm, 1);
	XChangeProperty(dpy, root, prop_eset, XA_PIXMAP, 32, PropModeReplace,
	                (unsigned char *)&pm, 1);
	XSetWindowBackgroundPixmap(dpy, root, pm);
	XClearWindow(dpy, root);
	XSync(dpy, False);

	XSetErrorHandler(old_handler);
}

static void wp_quote(FILE *f, const char *s)
{
	fputc('\'', f);
	for (; *s != '\0'; s++) {
		if (*s == '\'')
			fputs("'\\''", f);
		else
			fputc(*s, f);
	}
	fputc('\'', f);
}

/* write a small shell script that restores the wallpaper (feh's ~/.fehbg) */
static void wp_write_script(const fileinfo_t *file, wpmode_t mode)
{
	const char *base, *abspath, *suffix = "";
	char *path, *dir, *slash;
	size_t n;
	FILE *f;

	if ((abspath = file_realpath(file, false)) == NULL)
		return;
	if ((base = getenv("XDG_CONFIG_HOME")) == NULL || base[0] == '\0') {
		if ((base = getenv("HOME")) == NULL || base[0] == '\0')
			return;
		suffix = "/.config";
	}

	n = strlen(base) + strlen(suffix) + sizeof("/rsxiv/rsxivbg");
	path = emalloc(n);
	snprintf(path, n, "%s%s/rsxiv/rsxivbg", base, suffix);

	dir = estrdup(path);
	if ((slash = strrchr(dir, '/')) != NULL)
		*slash = '\0';
	if (r_mkdir(dir) != 0) {
		/* r_mkdir() already reported the problem */
	} else if ((f = fopen(path, "w")) == NULL) {
		error(0, errno, "%s", path);
	} else {
		fputs("#!/bin/sh\n", f);
		fprintf(f, "%s --no-bgscript %s ", progname, wp_flags[mode]);
		wp_quote(f, abspath);
		fputc('\n', f);
		if (fclose(f) != 0)
			error(0, errno, "%s", path);
		else if (chmod(path, 0755) != 0)
			error(0, errno, "%s", path);
	}
	free(dir);
	free(path);
}

bool wp_set(const fileinfo_t *file, wpmode_t mode)
{
	Imlib_Image im, prev;
	Imlib_Context ctx;
	Display *dpy;
	Window root;
	Pixmap pm;
	GC gc;
	int scr, sw, sh, depth;
	const char *fmt;

	if ((int)mode < 0 || (int)mode >= WP_COUNT)
		return false;

	/* img_open() makes the loaded image current, remember the old one so
	 * the caller's Imlib state is untouched afterwards.
	 */
	prev = imlib_context_get_image();
	if ((im = img_open(file)) == NULL) {
		if (!(file->flags & FF_WARN)) /* img_open() didn't say why */
			error(0, 0, "%s: Error opening image", file->name);
		if (prev != NULL)
			imlib_context_set_image(prev);
		return false;
	}
	(void)fmt;
#if HAVE_LIBEXIF
	if ((fmt = imlib_image_format()) != NULL && !STREQ(fmt, "jpeg") && !STREQ(fmt, "jpg"))
		exif_auto_orientate(file);
#endif

	/* private connection: its resources are kept alive after it closes */
	if ((dpy = XOpenDisplay(NULL)) == NULL) {
		error(0, 0, "Error opening X display");
		img_free(im, false);
		if (prev != NULL)
			imlib_context_set_image(prev);
		return false;
	}
	XSetCloseDownMode(dpy, RetainPermanent);

	scr = DefaultScreen(dpy);
	root = RootWindow(dpy, scr);
	sw = DisplayWidth(dpy, scr);
	sh = DisplayHeight(dpy, scr);
	depth = DefaultDepth(dpy, scr);

	pm = XCreatePixmap(dpy, root, sw, sh, depth);
	gc = XCreateGC(dpy, pm, 0, NULL);
	XSetForeground(dpy, gc, BlackPixel(dpy, scr));
	XFillRectangle(dpy, pm, gc, 0, 0, sw, sh);
	XFreeGC(dpy, gc);

	ctx = imlib_context_new();
	imlib_context_push(ctx);
	imlib_context_set_display(dpy);
	imlib_context_set_visual(DefaultVisual(dpy, scr));
	imlib_context_set_colormap(DefaultColormap(dpy, scr));
	imlib_context_set_drawable(pm);
	imlib_context_set_blend(1);
	imlib_context_set_anti_alias(1);
	wp_render(im, sw, sh, mode);
	imlib_context_pop();
	imlib_context_free(ctx);

	wp_set_root(dpy, root, pm);
	XCloseDisplay(dpy);

	img_free(im, false);
	if (prev != NULL)
		imlib_context_set_image(prev);

	if (options->write_bgscript)
		wp_write_script(file, mode);
	return true;
}
