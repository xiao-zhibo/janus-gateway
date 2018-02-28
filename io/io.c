#include "io.h"

#include <jansson.h>

#include "../apierror.h"
#include "../debug.h"


static int janus_pares_oss_path(janus_io_info *io_info) {
	JANUS_LOG(LOG_INFO, "--->  pares_oss_path: %s\n", io_info->path);
	if (io_info == NULL || io_info->path == NULL) {
		JANUS_LOG(LOG_ERR, "parse path error.");
		return -1;
	}

	char *p = strstr(io_info->path, "//");
	if (p == NULL ) {
		JANUS_LOG(LOG_ERR, "parse path error.");
		return  -1;
	}
	JANUS_LOG(LOG_INFO, "-->  %s\n", p);
	p = p + 2;
	JANUS_LOG(LOG_INFO, "-->  %s\n", p);
	//解析出bucket name
	char *tmp = strchr(p, '.');	
	if (tmp == NULL ) {
		JANUS_LOG(LOG_ERR, "parse path error.");
		return  -1;
	}
	JANUS_LOG(LOG_INFO, "---> %s\n", tmp);
	*tmp = '\0';
	JANUS_LOG(LOG_INFO, "bucket: %s\n", p);
	io_info->bucket = g_strdup(p);
	*tmp = '.';

	//解析出endpoint和object
	p = tmp + 1;
	tmp = strchr(p, '/');
	if (tmp == NULL ) {
		JANUS_LOG(LOG_ERR, "parse path error.");
		return  -1;
	}
	*tmp = '\0';
	JANUS_LOG(LOG_INFO, "endpoint: %s\n", p);
	io_info->endpoint = g_strdup(p);
	JANUS_LOG(LOG_INFO, "object: %s\n", tmp + 1);
	io_info->object = g_strdup(tmp + 1);
	*tmp = '/';
	return 1;
}

janus_io_info *janus_io_info_new(const char *path)
{
	janus_io_info *io_info = (janus_io_info *)g_malloc(sizeof(janus_io_info));
	io_info->io_handle = NULL;
	io_info->path = g_strdup(path);
	janus_pares_oss_path(io_info);
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
