#include <assert.h>
#include <limits.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <wayland-client.h>
#include "background-image.h"
#include "cairo.h"
#include "log.h"

enum background_mode parse_background_mode(const char *mode) {
	if (strcmp(mode, "stretch") == 0) {
		return BACKGROUND_MODE_STRETCH;
	} else if (strcmp(mode, "fill") == 0) {
		return BACKGROUND_MODE_FILL;
	} else if (strcmp(mode, "fit") == 0) {
		return BACKGROUND_MODE_FIT;
	} else if (strcmp(mode, "center") == 0) {
		return BACKGROUND_MODE_CENTER;
	} else if (strcmp(mode, "tile") == 0) {
		return BACKGROUND_MODE_TILE;
	} else if (strcmp(mode, "solid_color") == 0) {
		return BACKGROUND_MODE_SOLID_COLOR;
	}
	swaybg_log(LOG_ERROR, "Unsupported background mode: %s", mode);
	return BACKGROUND_MODE_INVALID;
}

struct cache_entry {
	char *path;
	cairo_surface_t *surface;

	time_t mtim_sec;
	long mtim_nsec;

	time_t ctim_sec;
	long ctim_nsec;

	struct wl_list link;
};

cairo_surface_t *load_background_image(struct wl_list *image_cache, const char *path) {
	cairo_surface_t *image;
	struct cache_entry *entry;
	struct stat sb;
	char real[PATH_MAX], curpath[PATH_MAX];

	if (!realpath(path, &real[0])) {
		swaybg_log_errno(LOG_ERROR, "Failed to resolve image path (%s)", path);
		return NULL;
	}
	wl_list_for_each(entry, image_cache, link) {
		if (realpath(entry->path, &curpath[0]) && !strcmp(real, curpath)) {
			swaybg_log(LOG_INFO, "Found image %s (%s) at %s (%s)", path, real, entry->path, curpath);
			if (access(curpath, F_OK)) {
				// file does not exist now
				// return cached copy
				swaybg_log(LOG_INFO, "Loading image %s from cache!", path);
				return entry->surface;
			}
			stat(curpath, &sb);
			if (sb.st_mtim.tv_sec == entry->mtim_sec &&
				sb.st_mtim.tv_nsec == entry->mtim_nsec &&
				sb.st_ctim.tv_sec == entry->ctim_sec &&
				sb.st_ctim.tv_nsec == entry->ctim_nsec) {
				swaybg_log(LOG_INFO, "Loading image %s from cache!", path);
				return entry->surface;
			}
			break;
		}
	}

	stat(real, &sb);

#if HAVE_GDK_PIXBUF
	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &err);
	if (!pixbuf) {
		swaybg_log(LOG_ERROR, "Failed to load background image (%s).",
				err->message);
		return NULL;
	}
	image = gdk_cairo_image_surface_create_from_pixbuf(pixbuf);
	g_object_unref(pixbuf);
#else
	image = cairo_image_surface_create_from_png(path);
#endif // HAVE_GDK_PIXBUF
	if (!image) {
		swaybg_log(LOG_ERROR, "Failed to read background image.");
		return NULL;
	}
	if (cairo_surface_status(image) != CAIRO_STATUS_SUCCESS) {
		swaybg_log(LOG_ERROR, "Failed to read background image: %s."
#if !HAVE_GDK_PIXBUF
				"\nSway was compiled without gdk_pixbuf support, so only"
				"\nPNG images can be loaded. This is the likely cause."
#endif // !HAVE_GDK_PIXBUF
				, cairo_status_to_string(cairo_surface_status(image)));
		return NULL;
	}
	if (!entry || &entry->link == image_cache) {
		entry = calloc(1, sizeof(struct cache_entry));
		wl_list_insert(image_cache, &entry->link);
		entry->path = strdup(path);
	}
	entry->surface = image;
	entry->mtim_sec = sb.st_mtim.tv_sec;
	entry->mtim_nsec = sb.st_mtim.tv_nsec;
	entry->ctim_sec = sb.st_ctim.tv_sec;
	entry->ctim_nsec = sb.st_ctim.tv_nsec;
	return image;
}

void render_background_image(cairo_t *cairo, cairo_surface_t *image,
		enum background_mode mode, int buffer_width, int buffer_height) {
	double width = cairo_image_surface_get_width(image);
	double height = cairo_image_surface_get_height(image);

	cairo_save(cairo);
	switch (mode) {
	case BACKGROUND_MODE_STRETCH:
		cairo_scale(cairo,
				(double)buffer_width / width,
				(double)buffer_height / height);
		cairo_set_source_surface(cairo, image, 0, 0);
		break;
	case BACKGROUND_MODE_FILL: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					0, (double)buffer_height / 2 / scale - height / 2);
		} else {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					(double)buffer_width / 2 / scale - width / 2, 0);
		}
		break;
	}
	case BACKGROUND_MODE_FIT: {
		double window_ratio = (double)buffer_width / buffer_height;
		double bg_ratio = width / height;

		if (window_ratio > bg_ratio) {
			double scale = (double)buffer_height / height;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					(double)buffer_width / 2 / scale - width / 2, 0);
		} else {
			double scale = (double)buffer_width / width;
			cairo_scale(cairo, scale, scale);
			cairo_set_source_surface(cairo, image,
					0, (double)buffer_height / 2 / scale - height / 2);
		}
		break;
	}
	case BACKGROUND_MODE_CENTER:
		cairo_set_source_surface(cairo, image,
				(double)buffer_width / 2 - width / 2,
				(double)buffer_height / 2 - height / 2);
		break;
	case BACKGROUND_MODE_TILE: {
		cairo_pattern_t *pattern = cairo_pattern_create_for_surface(image);
		cairo_pattern_set_extend(pattern, CAIRO_EXTEND_REPEAT);
		cairo_set_source(cairo, pattern);
		break;
	}
	case BACKGROUND_MODE_SOLID_COLOR:
	case BACKGROUND_MODE_INVALID:
		assert(0);
		break;
	}
	cairo_paint(cairo);
	cairo_restore(cairo);
}

void flush_image_cache(struct wl_list *image_cache) {
	struct cache_entry *entry, *tmp;
	wl_list_for_each_safe(entry, tmp, image_cache, link) {
		cairo_surface_destroy(entry->surface);
		free(entry->path);
		wl_list_remove(&entry->link);
		free(entry);
	}
}
