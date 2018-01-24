#include "io.h"

#include <jansson.h>

#include "../apierror.h"
#include "../debug.h"

janus_io_info *janus_io_info_new(const char *path)
{
	janus_io_info *io_info = g_malloc(sizeof(path));
	io_info->path = g_strdup(path);
	return io_info;
}

void janus_io_info_destroy(janus_io_info *io_info)
{
	if (io_info) {
		if (io_info->path) {
			g_free(io_info->path);
			io_info->path = NULL;
		}
		g_free(io_info);
	}
}
