#include "io.h"

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

typedef struct janus_oss {
	aos_pool_t *p = NULL;
	oss_config_t *config;
	aos_string_t bucket;
	aos_string_t object;
} janus_oss;

/* Transport creator */
janus_transport *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_REST_NAME);
	return &janus_http_transport;
}

/* Transport implementation */
int janus_io_init(janus_transport_callbacks *callback, const char *config_path) {
	
	return 0;
}

void janus_io_destroy(void) {
	
}

int janus_http_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_IO_API_VERSION;
}

void pares_oss_path(const char *path, char *bucket, char *object) {
	
}

int janus_oss_info_create(janus_io_info *info, char *path) {

}

void janus_io_write_data(void *io_info, char *buf, size_t len) {

}

void janus_io_read_data(void *io_info, char *buf) {

}

void janus_io_read_data_range(void *io_info, char *buf, size_t start, size_t end) {

}
