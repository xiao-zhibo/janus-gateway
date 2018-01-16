#include "io.h"
#include "../utils.h"
#include "../config.h"

#include "aos_log.h"
#include "aos_util.h"
#include "aos_string.h"
#include "aos_status.h"
#include "oss_auth.h"
#include "oss_util.h"
#include "oss_api.h"

#define JANUS_OSS_NAME  "JANUS OSS IO"
#define JANUS_OSS_PACKAGE  "janus.io.oss"




static janus_io janus_oss_io =
	JANUS_TRANSPORT_INIT (
		.init = janus_io_init,
		.destroy = janus_io_destroy,

		.get_api_compatibility = janus_io_get_api_compatibility,

		.io_info_create = janus_oss_info_create,
		.write_data = janus_io_write_data,
		.read_data = janus_io_read_data,
		.read_data_range = janus_io_read_data_range,
	);

/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;

/* Static configuration instance */
static janus_config *config = NULL;
static const char *config_folder = NULL;
static janus_mutex config_mutex = JANUS_MUTEX_INITIALIZER;

static char *endpoint = NULL;
static char *access_key_id = NULL;
static char *access_key_secret = NULL;
static char *bucket = NULL;
static char *prefix = NULL;

typedef struct janus_oss {
	aos_pool_t *pool = NULL;
	oss_config_t *config;
	aos_string_t bucket;
	aos_string_t object;
	int64_t position;
} janus_oss;

int pares_oss_path(janus_oss *oss, const char *path);
void janus_oss_init(janus_oss *oss, const char *path);

/* Transport creator */
janus_id *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_OSS_NAME);
	return &janus_oss_io;
}

/* Transport implementation */
int janus_io_init(const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_OSS_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	config = janus_config_parse(filename);
	config_folder = config_path;
	if(config != NULL)
		janus_config_print(config);


	messages = g_async_queue_new_full((GDestroyNotify) janus_videoroom_message_free);

	/* Parse configuration to populate the rooms list */
	if(config != NULL) {
		janus_config_item *key = janus_config_get_item_drilldown(config, "general", "endpoint");
		if(key != NULL && key->value != NULL)
			endpoint = g_strdup(key->value);

		janus_config_item *access_key_id_item = janus_config_get_item_drilldown(config, "general", "access_key_id");
		if(access_key_id_item != NULL && access_key_id_item->value != NULL)
			access_key_id = g_strdup(access_key_id_item->value);

		janus_config_item *access_key_secret_item = janus_config_get_item_drilldown(config, "general", "access_key_secret");
		if(access_key_secret_item != NULL && access_key_secret_item->value != NULL)
			access_key_secret = g_strdup(access_key_secret_item->value);

		janus_config_item *bucket_item = janus_config_get_item_drilldown(config, "general", "bucket");
		if(bucket_item != NULL && bucket_item->value != NULL)
			bucket = g_strdup(bucket_item->value);

		janus_config_item *prefix_item = janus_config_get_item_drilldown(config, "general", "prefix");
		if(prefix_item != NULL && prefix_item->value != NULL)
			prefix = g_strdup(prefix_item->value);
		
		/* Done: we keep the configuration file open in case we get a "create" or "destroy" with permanent=true */
	}

	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_OSS_NAME);
	return 0;
}

void janus_io_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	janus_config_destroy(config);
	g_free(endpoint);
	g_free(access_key_id);
	g_free(access_key_secret);
	g_free(bucket);
	g_free(prefix);

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_OSS_NAME);
}

int janus_http_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_IO_API_VERSION;
}

int pares_oss_path(janus_oss *oss, const char *path) {
	if (oss == NULL || path == NULL) {
		return -1;
	}
	char *p = path;
	p = strstr(p, "//");
	if (p == NULL ) {
		return  -1;
	}
	p = p + 2;
	//解析出bucket name
	char *tmp = strchr(p, '.');	
	if (tmp == NULL ) {
		return  -1;
	}
	*tmp = '\0'
	oss->bucket = aos_string(p);
	*tmp = '.'
	//解析出endpoint和object
	p = tmp + 1;
	char *tmp = strchr(p, '/');	
	if (tmp == NULL ) {
		return  -1;
	}
	*tmp = '\0'
	oss->endpoint = aos_string(p);
	oss->object = aos_string(tmp + 1);
	*tmp = '/';
}

int janus_oss_init(janus_oss *oss, const char *path) {
	if (path != NULL) {
		return pares_oss_path(oss, path);
	} else {
		oss->bucket = aos_string(bucket);
		oss->endpoint = aos_string(endpoint);
		gint64 now = janus_get_real_time();
		char buffer[255];
		g_snprintf(buffer, 255, "%s/oss-%"SCNi64"", prefix, now);
		oss->object = aos_string(buffer);
	}
	return 1;
}

void *janus_oss_info_create(janus_io_info *info) {
	if (info->path == NULL) {
		return -1;
	}
	janus_oss *oss = g_malloc0(sizeof(janus_oss));
	if (janus_oss_init(oss, info->path) < 0) {
		g_free(oss);
		return NULL;
	}
	aos_pool_create(&oss->pool, NULL);
	return oss;
}

void janus_io_write_data(janus_io_info *io_info, char *buf, size_t len) {
	if (info == NULL || info->io_handle == NULL) {
		return -1;
	}
	janus_oss *oss = io_info->io_handle;

    int64_t position = 0;
    aos_status_t *s = NULL;
    aos_table_t *headers1 = NULL;
    aos_table_t *headers2 = NULL;
    aos_table_t *resp_headers = NULL;
    aos_list_t buffer;
    aos_buf_t *content = NULL;
    char *next_append_position = NULL;
    char *object_type = NULL;
    oss_request_options_t *options = NULL;

	options = oss_request_options_create(p);
	options->config = oss_config_create(options->pool);
    aos_str_set(options->config.endpoint, oss->endpoint);
    aos_str_set(options->config.access_key_id, access_key_id);
    aos_str_set(options->config.access_key_secret, access_key_secret);
    options->config.is_cname = 0;
    options->ctl = aos_http_controller_create(options->pool, 0);

    s = oss_head_object(options, &bucket, &object, headers1, &resp_headers);
    if (aos_status_is_ok(s)) {
        object_type = (char*)(apr_table_get(resp_headers, OSS_OBJECT_TYPE));
        if (0 != strncmp(OSS_OBJECT_TYPE_APPENDABLE, object_type, 
                         strlen(OSS_OBJECT_TYPE_APPENDABLE))) 
        {
            printf("object[%s]'s type[%s] is not Appendable\n", OBJECT_NAME, object_type);
            aos_pool_destroy(p);
            return -1;
        }

        next_append_position = (char*)(apr_table_get(resp_headers, OSS_NEXT_APPEND_POSITION));
        position = aos_atoi64(next_append_position);
    }

    headers2 = aos_table_make(p, 0);
    aos_list_init(&buffer);
    content = aos_buf_pack(p, buf, strlen(buf));
    aos_list_add_tail(&content->node, &buffer);
    s = oss_append_object_from_buffer(options, &bucket, &object, 
            position, &buffer, headers2, &resp_headers);

    if (aos_status_is_ok(s))
    {
        printf("append object from buffer succeeded\n");
    } else {
        printf("append object from buffer failed\n");
    }
    return len(buf);
}

int janus_io_read_data(janus_io_info *io_info, char *buf) {
	return -1;
}

int janus_io_read_data_range(janus_io_info *io_info, char *buf, size_t start, size_t size) {\
    int is_cname = 0;
    oss_request_options_t *options = NULL;
    aos_table_t *headers = NULL;
    aos_table_t *params = NULL;
    aos_table_t *resp_headers = NULL;
    aos_status_t *s = NULL;
    aos_list_t buffer;
    aos_buf_t *content = NULL;
    char *buf = NULL;
    int64_t len = 0;
    int64_t size = 0;
    int64_t pos = 0;

    aos_pool_create(&p, NULL);
    options = oss_request_options_create(p);
	options->config = oss_config_create(options->pool);
    aos_str_set(options->config.endpoint, oss->endpoint);
    aos_str_set(options->config.access_key_id, access_key_id);
    aos_str_set(options->config.access_key_secret, access_key_secret);
    options->config.is_cname = 0;
    options->ctl = aos_http_controller_create(options->pool, 0);

    aos_list_init(&buffer);
    headers = aos_table_make(p, 1);

    /* 设置Range，读取文件的指定范围，bytes=20-100包括第20和第100个字符 */
    char *range[255];
    g_snprintf(range, 255, "bytes=%d-%d", start, start + size);
    apr_table_set(headers, "Range", range);

    s = oss_get_object_to_buffer(options, &oss->bucket, &oss->object, 
                                 headers, params, &buffer, &resp_headers);

    if (aos_status_is_ok(s)) {
        printf("get object to buffer succeeded\n");
    }
    else {
        printf("get object to buffer failed\n");  
    }

    //get buffer len
    aos_list_for_each_entry(aos_buf_t, content, &buffer, node) {
        len += aos_buf_size(content);
    }

    buf[len] = '\0';

    //copy buffer content to memory
    aos_list_for_each_entry(aos_buf_t, content, &buffer, node) {
        size = aos_buf_size(content);
        memcpy(buf + pos, content->pos, (size_t)size);
        pos += size;
    }
    return len;
}
