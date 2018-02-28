#include <dlfcn.h>
#include <dirent.h>
#include <net/if.h>
#include <netdb.h>
#include <signal.h>
#include <getopt.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <poll.h>

#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "io.h"

#define DOXYGEN 1

janus_io_info *init(janus_io *janus_io, const char *path) {
	janus_io->init("");
	janus_io_info *io_info = g_malloc(sizeof(janus_io_info));
	io_info->path = path;
	janus_io->io_info_create(io_info);
	printf("----------\n");
	return io_info;
}

void oss_close(janus_io *janus_io, janus_io_info *io_info) {
	if (!janus_io || !io_info) {
		printf("null.....\n");
		return;
	}
	janus_io->io_info_close(io_info);
	g_free(io_info);
}

int oss_write_data(janus_io *janus_io, janus_io_info *io_info) {
	char *buffer = "oss upload test";
	int len = janus_io->write_data(io_info, buffer, strlen(buffer));
	printf("upload data: %d\n", len);
}

int oss_read_data(janus_io *janus_io, janus_io_info *io_info, int pos, int len) {
	char *buffer[1024];
	int len2 = janus_io->read_data_range(io_info, buffer, pos, len);
	printf("upload data: %d ----> %s\n", len2, buffer);
}

int oss_read_data_to_file(janus_io *janus_io, janus_io_info *io_info, char *filename) {
	int len2 = janus_io->read_data_to_file(io_info, filename);
	printf("upload data: ----> %s\n", filename);
}


int main() {
	printf("------------------------\n");
	void *io = dlopen("/opt/janus/lib/janus/io/libjanus_oss.so", RTLD_NOW | RTLD_GLOBAL);
	if (!io) {
		printf("error: %s\n", dlerror());
	} else {
		create_i *create = (create_i*) dlsym(io, "create");
		const char *dlsym_error = dlerror();
		if (dlsym_error) {
			printf("\tCouldn't load symbol 'create': %s\n", dlsym_error);
		}
		janus_io *janus_io = create();
		if (!janus_io)
			printf("error ...... 1111\n");
		printf("get_api_compatibility: %d\n", janus_io->get_api_compatibility());
		janus_io_info *io_info = init(janus_io, "https://spark-courseware.oss-cn-shenzhen.aliyuncs.com/dev/sloth/test.whiteboard");

		oss_write_data(janus_io, io_info);

		oss_read_data(janus_io, io_info, 2, 5);

		oss_close(janus_io, io_info);


		janus_io_info *io_info2 = init(janus_io, "https://spark-courseware.oss-cn-shenzhen.aliyuncs.com/dev/sloth/test.whiteboard2");
		oss_read_data_to_file(janus_io, io_info2, "/mnt/test2");
		oss_close(janus_io, io_info2);

	}
	printf("========================\n");
}