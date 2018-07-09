#include <dlfcn.h>
#include "whiteboard.h"
#include <sys/stat.h>
#include <sys/time.h>
#include "debug.h"
#include "utils.h"

/*
 * 白板数据和白板头部分开成两个文件，均采用以下形式存储
 *
 * 头部
 *               |--------------|       | data length |
 *               |     header   |------>| ----------- |
 *               |--------------|       | binary data |
 *
 * 数据
 *               |  frame pkt1  |---|
 *               |--------------|   |
 *               |  frame pkt2  |   |   | data length |
 *               |--------------|   |-->| ----------- |
 *               |  frame pkt3  |       | binary data |
 *               |--------------|
 *               |  frame pkt4  |
 *               |--------------|
 *               |    ......    |
 * 这样的好处是：头部可以定时保存，避免数据文件无法找到索引导致无法读取，同时可以避免存在同一文件开头时造成的性能问题
 * 也避免了存同一文件尾部只能在房间关闭时才能保存头部的尴尬，容易丢失数据。
 */
// FIXME:Rison: 使用队列可能更好维护

/*! 本地函数，前置声明 */
int64_t  janus_whiteboard_get_current_time_l(void);
gboolean janus_whiteboard_check_diretory(const char *dir);
char	 janus_whiteboard_package_check(janus_whiteboard *whiteboard, Pb__Package *package);
int      janus_whiteboard_read_packet_from_file_l(void* dst, size_t len, FILE *src_file);
int      janus_whiteboard_write_packet_to_file_l(void* src, size_t len, FILE *dst_file);
int      janus_whiteboard_remove_packets_l(Pb__Package** packages, int start_index, int len);
int      janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard);
void     janus_whiteboard_add_pkt_to_packages_l(GPtrArray *packages, Pb__Package* dst_pkg);
void     janus_whiteboard_packed_data_l(GPtrArray *packages, janus_whiteboard_result* result);
int      janus_whiteboard_scene_page_data_l(janus_whiteboard *whiteboard, int scene, int page, GPtrArray* packages);
int      janus_whiteboard_on_receive_keyframe_l(janus_whiteboard *whiteboard, Pb__Package *package);
int      janus_whiteboard_on_receive_switch_scene_l(janus_whiteboard *whiteboard, Pb__Package *package);
int      janus_whiteboard_generate_and_save_l(janus_whiteboard *whiteboard);
int 	 janus_whiteboard_add_scene_l(janus_whiteboard *whiteboard, Pb__Scene *newScene);
janus_scene *janus_whiteboard_get_scene(janus_whiteboard *whiteboard, int scene_index);
static void janus_whiteboard_scene_free(janus_scene *scene);

static janus_io *janus_oss_io = NULL;

void oss_init(const char *io_folder) {
	void *io = dlopen(io_folder, RTLD_NOW | RTLD_GLOBAL);
	if (!io) {
		JANUS_LOG(LOG_ERR, "open janus oss error: %s\n", dlerror());
	} else {
		create_i *create = (create_i*) dlsym(io, "create");
		const char *dlsym_error = dlerror();
		if (dlsym_error) {
			JANUS_LOG(LOG_ERR, "\tCouldn't load symbol 'create': %s\n", dlsym_error);
		}
		janus_oss_io = create();
		if (!janus_oss_io) {
			JANUS_LOG(LOG_ERR, "create janus oss io error.\n");
		}
		janus_oss_io->init("");
		JANUS_LOG(LOG_ERR, "oss get_api_compatibility: %d\n", janus_oss_io->get_api_compatibility());
	}
}

janus_whiteboard *janus_whiteboard_create_with_oss(const char *oss_path, const char *dir, const char *filename);
janus_whiteboard *janus_whiteboard_create_with_file(const char *dir, const char *filename);

janus_whiteboard *janus_whiteboard_create_with_oss(const char *oss_path, const char *dir, const char *filename) {
	if(!janus_whiteboard_check_diretory(dir)) {
		return NULL;
	}
	dir == NULL ? "" : dir;

	janus_whiteboard *whiteboard = g_malloc0(sizeof(janus_whiteboard));
	if(whiteboard == NULL) {
		JANUS_LOG(LOG_FATAL, "Out of Memory when alloc memory for struct whiteboard!\n");
		return NULL;
	}

	const size_t length = 1024;
	/* generate filename */
	whiteboard->scene_info = NULL;
	whiteboard->header_info = NULL;
	whiteboard->page_info = NULL;
	whiteboard->packet_info = NULL;
	if (oss_path && strlen(oss_path) > 0) {
		char packet_path[length], header_path[length], scene_path[length], page_path[length];
		memset(scene_path,   0, length);
		memset(header_path, 0, length);
		memset(page_path,  0, length);
		memset(packet_path,  0, length);
		g_snprintf(packet_path,   length, "%s/%s.data", oss_path, filename);
		g_snprintf(header_path, length, "%s/%s.head", oss_path, filename);
		g_snprintf(scene_path, length, "%s/%s.scene", oss_path, filename);
		g_snprintf(page_path, length, "%s/%s.page", oss_path, filename);

		JANUS_LOG(LOG_WARN, "new scene io info: %s!\n", scene_path);
		whiteboard->scene_info = janus_io_info_new(scene_path);
		JANUS_LOG(LOG_WARN, "new header io info!\n");
		whiteboard->header_info = janus_io_info_new(header_path);
		JANUS_LOG(LOG_WARN, "new page io info!\n");
		whiteboard->page_info = janus_io_info_new(page_path);
		JANUS_LOG(LOG_WARN, "new packet io info!\n");
		whiteboard->packet_info = janus_io_info_new(packet_path);
		JANUS_LOG(LOG_WARN, "whiteboard io info new success!\n");
		
		JANUS_LOG(LOG_WARN, "init scene io info!\n");
		if (janus_oss_io && janus_oss_io->io_info_create(whiteboard->scene_info) < 0) {
			JANUS_LOG(LOG_WARN, "init scene io info error!\n");
			// g_free(whiteboard);
			return NULL;
		}
		JANUS_LOG(LOG_WARN, "init header io info!\n");
		if (janus_oss_io && janus_oss_io->io_info_create(whiteboard->header_info) < 0) {
			JANUS_LOG(LOG_WARN, "init header io info error!\n");
			g_free(whiteboard);
			return NULL;
		}
		JANUS_LOG(LOG_WARN, "init page io info!\n");
		if (janus_oss_io && janus_oss_io->io_info_create(whiteboard->page_info) < 0) {
			JANUS_LOG(LOG_WARN, "init page io info error!\n");
			g_free(whiteboard);
			return NULL;
		}
		JANUS_LOG(LOG_WARN, "init packet io info!\n");
		if (janus_oss_io && janus_oss_io->io_info_create(whiteboard->packet_info) < 0) {
			JANUS_LOG(LOG_WARN, "init packet io info error!\n");
			g_free(whiteboard);
			return NULL;
		}
	}
	JANUS_LOG(LOG_WARN, "whiteboard io info init success!\n");

	/* generate filename */
	whiteboard->dir = NULL;
	whiteboard->filename = NULL;
	whiteboard->header_file = NULL;
	whiteboard->file = NULL;
	char data_file_name[length], header_file_name[length], scene_file_name[length], page_file_name[length];
	memset(data_file_name,   0, length);
	memset(header_file_name, 0, length);
	memset(scene_file_name,  0, length);
	memset(page_file_name,  0, length);
	g_snprintf(data_file_name,   length, "%s/%s.data", dir, filename);
	g_snprintf(header_file_name, length, "%s/%s.head", dir, filename);
	g_snprintf(scene_file_name, length, "%s/%s.scene", dir, filename);
	g_snprintf(page_file_name, length, "%s/%s.page", dir, filename);

	if (janus_oss_io && whiteboard->scene_info && whiteboard->header_info && whiteboard->page_info && whiteboard->packet_info) {
		if (janus_oss_io->read_data_to_file(whiteboard->scene_info, scene_file_name) < 0) {
			g_free(whiteboard);
			return NULL;
		}
		if (janus_oss_io->read_data_to_file(whiteboard->header_info, header_file_name) < 0) {
			g_free(whiteboard);
			return NULL;
		}
		if (janus_oss_io->read_data_to_file(whiteboard->page_info, page_file_name) < 0) {
			g_free(whiteboard);
			return NULL;
		}
		if (janus_oss_io->read_data_to_file(whiteboard->packet_info, data_file_name) < 0) {
			g_free(whiteboard);
			return NULL;
		}
	}

	/* Try opening the data file */
	whiteboard->file = fopen(data_file_name, "ab+");
	if(whiteboard->file == NULL) {
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", data_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	/* Try opening the header file */
	whiteboard->header_file = fopen(header_file_name, "ab+");
	if(whiteboard->header_file == NULL) {
		/* avoid memory leak */
		fclose(whiteboard->file);
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", header_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	/* Try opening the scene data file */
	whiteboard->scene_file = fopen(scene_file_name, "ab+");
	if (whiteboard->scene_file == NULL) {
		fclose(whiteboard->file);
		fclose(whiteboard->header_file);
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", scene_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	/* Try opening the page index file */
	whiteboard->page_file = fopen(page_file_name, "ab+");
	if (whiteboard->page_file == NULL) {
		fclose(whiteboard->file);
		fclose(whiteboard->header_file);
		fclose(whiteboard->scene_file);
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", page_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	if(dir)
		whiteboard->dir = g_strdup(dir);
	whiteboard->filename = g_strdup(filename);
	return whiteboard;
}

janus_whiteboard *janus_whiteboard_create_with_file(const char *dir, const char *filename) {
	if(!janus_whiteboard_check_diretory(dir)) {
		return NULL;
	}
	dir == NULL ? "" : dir;

	janus_whiteboard *whiteboard = g_malloc0(sizeof(janus_whiteboard));
	if(whiteboard == NULL) {
		JANUS_LOG(LOG_FATAL, "Out of Memory when alloc memory for struct whiteboard!\n");
		return NULL;
	}

	/* generate filename */
	const size_t length = 1024;
	whiteboard->dir = NULL;
	whiteboard->filename = NULL;
	whiteboard->header_file = NULL;
	whiteboard->file = NULL;
	char data_file_name[length], header_file_name[length], scene_file_name[length], page_file_name[length];
	memset(data_file_name,   0, length);
	memset(header_file_name, 0, length);
	memset(scene_file_name,  0, length);
	memset(page_file_name,  0, length);
	g_snprintf(data_file_name,   length, "%s/%s.data", dir, filename);
	g_snprintf(header_file_name, length, "%s/%s.head", dir, filename);
	g_snprintf(scene_file_name, length, "%s/%s.scene", dir, filename);
	g_snprintf(page_file_name, length, "%s/%s.page", dir, filename);

	JANUS_LOG(LOG_ERR, "open file: %s\n%s\n%s\n%s\n", scene_file_name, header_file_name, data_file_name, page_file_name);
	/* Try opening the data file */
	whiteboard->file = fopen(data_file_name, "ab+");
	if(whiteboard->file == NULL) {
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", data_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	/* Try opening the header file */
	whiteboard->header_file = fopen(header_file_name, "ab+");
	if(whiteboard->header_file == NULL) {
		/* avoid memory leak */
		fclose(whiteboard->file);
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", header_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	/* Try opening the scene data file */
	whiteboard->scene_file = fopen(scene_file_name, "ab+");
	if (whiteboard->scene_file == NULL) {
		fclose(whiteboard->file);
		fclose(whiteboard->header_file);
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", scene_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	/* Try opening the page index file */
	whiteboard->page_file = fopen(page_file_name, "ab+");
	if (whiteboard->page_file == NULL) {
		fclose(whiteboard->file);
		fclose(whiteboard->header_file);
		fclose(whiteboard->scene_file);
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", page_file_name, errno);
		g_free(whiteboard);
		return NULL;
	}
	if(dir)
		whiteboard->dir = g_strdup(dir);
	whiteboard->filename = g_strdup(filename);
	return whiteboard;
}

int64_t janus_whiteboard_get_current_time_l(void) {
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

gboolean janus_whiteboard_check_diretory(const char *dir) {
	/* Check if this directory exists, and create it if needed */
	if(dir != NULL) {
		struct stat s;
		int err = stat(dir, &s);
		if(err == -1) {
			if(ENOENT == errno) {
				/* Directory does not exist, try creating it */
				if(janus_mkdir(dir, 0755) < 0) {
					JANUS_LOG(LOG_ERR, "mkdir error: %d\n", errno);
					return FALSE;
				}
			} else {
				JANUS_LOG(LOG_ERR, "stat error: %d\n", errno);
				return FALSE;
			}
		} else {
			if(S_ISDIR(s.st_mode)) {
				/* Directory exists */
				JANUS_LOG(LOG_VERB, "Directory exists: %s\n", dir);
			} else {
				/* File exists but it's not a directory, try creating it */
				if(janus_mkdir(dir, 0755) < 0) {
					JANUS_LOG(LOG_ERR, "mkdir error: %d\n", errno);
					return FALSE;
				}
				return FALSE;
			}
		}
	}
	return TRUE;
}

char janus_whiteboard_package_check(janus_whiteboard *whiteboard, Pb__Package *package) {
	int scene_num = g_hash_table_size(whiteboard->scenes);
	if (package->scene < 0 || package->scene >= scene_num) {
		JANUS_LOG(LOG_ERR, "\"janus_whiteboard_package_check\"  scene: %d, scene_num: %d.", package->scene, scene_num);
		return 0;
	}
	janus_scene *scene_data = janus_whiteboard_get_scene(whiteboard, package->scene);
	if (package->page < 0 || scene_data == NULL || package->page >= scene_data->page_num) {
		JANUS_LOG(LOG_ERR, "\"janus_whiteboard_package_check\"  scene: %d, page: %d.", package->scene, package->page);
		if (scene_data != NULL) {
			JANUS_LOG(LOG_ERR, "\"janus_whiteboard_package_check\" page_num:%d.", scene_data->page_num);
		}
		return 0;
	}
	return 1;
}

/*! 从指定文件读取len字节到dst，
    @returns 正常读取返回 0，读取失败返回 -1 */
int janus_whiteboard_read_packet_from_file_l(void* dst, size_t len, FILE *src_file) {
	size_t ret = 0, total = len;
	while(total > 0) {
		ret = fread(dst + (len-total), sizeof(unsigned char), total, src_file);
		if (ret >= total) {
			return 0;
		} else if (ret <= 0) {
			JANUS_LOG(LOG_ERR, "Error reading packet...\n");
			return -1;
		}
		total -= ret;
	}
	return 0;
}

char janus_whiteboard_read_packet_from_file(uint8_t **buf, size_t *len, FILE *src_file) {
	char *buffer = NULL;
	size_t pkt_len = 0;
	*buf = NULL;

	if(fread(&pkt_len, sizeof(size_t), 1, src_file) != 1 ) {
		JANUS_LOG(LOG_ERR, "Error happens when reading data from file: %d\n", ferror(src_file));
		return -1;
	}
	if (pkt_len > 0 && pkt_len < MAX_PACKET_CAPACITY) {
		buffer = g_malloc0(pkt_len);
		if (janus_whiteboard_read_packet_from_file_l(buffer, pkt_len, src_file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading data from file\n");
			g_free(buffer);
			return -1;
		}
	}
	*len = pkt_len;
	*buf = buffer;
	return 1;
}

/*! 写入len字节到目标文件 
    @returns 正常写入返回0， 异常返回 -1 */
int janus_whiteboard_write_packet_to_file_l(void* src, size_t len, FILE *dst_file) {
	size_t ret = 0, total = len;
	while (total > 0) {
		ret = fwrite(src + (len-total), sizeof(unsigned char), total, dst_file);
		if (ret >= total) {
			fflush(dst_file);
			return 0;
		} else if (ret <= 0) {
			JANUS_LOG(LOG_ERR, "Error saving packet...\n");//应该表明写入了多少
			fflush(dst_file);
			return -1;
		}
		total -= ret;
	}
	fflush(dst_file);
	return 0;
}

void janus_whiteboard_package_free(Pb__Package *package) {
	if (package) {
		pb__package__free_unpacked(package, NULL);
	}
}

/*! 由于比较多地方需要 free packets，独立成一个函数比较好管理. 需要对packets长度进行判断防止崩溃 */
int janus_whiteboard_remove_packets_l(Pb__Package** packages, int start_index, int len) {
	int end_index = start_index + len;
	for(int index = start_index; index < end_index; index++) {
		Pb__Package **tmp_package = &packages[index];
		if (*tmp_package) {
			pb__package__free_unpacked(*tmp_package, NULL);
		}
		*tmp_package = NULL;
	}
	return 0;
}

int janus_whiteboard_init_scene_from_file_l(janus_whiteboard *whiteboard) {
	whiteboard->scenes = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)janus_whiteboard_scene_free);

	fseek(whiteboard->scene_file, 0, SEEK_SET);
	JANUS_LOG(LOG_WARN, "scene file seek: %d\n", ferror(whiteboard->scene_file));
	size_t pkt_len = 0;
	uint8_t *buffer = NULL;
	while ( janus_whiteboard_read_packet_from_file(&buffer, &pkt_len, whiteboard->scene_file) > 0) {
		if (buffer == NULL) {
			JANUS_LOG(LOG_WARN, "parse whiteboard scene package error: %s\n", whiteboard->filename);
			continue;
		}
		Pb__Scene *tmp_scene = pb__scene__unpack(NULL, pkt_len, (const uint8_t*)buffer);
		if (tmp_scene == NULL) {
			JANUS_LOG(LOG_WARN, "Parse whiteboard scene data error.\n");
			fseek(whiteboard->scene_file, 0, SEEK_END);
			g_free(buffer);
			return -1;
		}

		janus_scene *j_scene 					= g_malloc0(sizeof(janus_scene));
		j_scene->source_url 					= g_strdup(tmp_scene->resource);
		j_scene->page_num 						= tmp_scene->pagecount;
		j_scene->page_keyframes       			= g_malloc0(sizeof(Pb__KeyFrame*) * j_scene->page_num);
		j_scene->page_keyframe_maxnum 			= 0;
		j_scene->index 							= tmp_scene->index;
		if (tmp_scene->resourceid != NULL) {
			j_scene->source_id = g_strdup(tmp_scene->resourceid);
		} else {
			j_scene->source_id = "";
		}

		g_hash_table_insert(whiteboard->scenes, GINT_TO_POINTER(tmp_scene->index), j_scene);
		JANUS_LOG(LOG_INFO, "whiteboard:janus scene: %d %s, %d\n", tmp_scene->index, j_scene->source_url, j_scene->page_num);

		pb__scene__free_unpacked(tmp_scene, NULL);
		g_free(buffer);
		buffer = NULL;
	}
	fseek(whiteboard->scene_file, 0, SEEK_END);
	JANUS_LOG(LOG_INFO, "--->init scene data from file success!\n");
	return 0;
}

/*! 初始化header
    @returns 正常初始化返回header_len长度（非负数）， 异常返回 -1 */
int janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard) {

	if(!whiteboard || !whiteboard->header_file)
		return -1;

	if (janus_whiteboard_init_scene_from_file_l(whiteboard) != 0) {
		JANUS_LOG(LOG_WARN, "init whiteboard scene data error.\n");
		return -1;
	}

	int ret = 0;
	size_t pkt_len;
	whiteboard->page  = 0;
	whiteboard->scene = 0;
	ret = fseek(whiteboard->header_file, 0, SEEK_SET);
	if (ret < 0) {
		JANUS_LOG(LOG_ERR, "seek header file error.\n");
		return -1;
	}

	// 尝试解析数据到whiteboard->header，如果不成功则执行创建操作
	size_t keyframe_len = 0;
	char *keyframe_buffer = NULL;
	while(janus_whiteboard_read_packet_from_file(&keyframe_buffer, &keyframe_len, whiteboard->header_file) > 0) {
		if(keyframe_buffer == NULL) {
			continue;
		}
		Pb__KeyFrame *tmp_keyframe = pb__key_frame__unpack(NULL, keyframe_len, (const uint8_t*)keyframe_buffer);
		if (tmp_keyframe != NULL) {
			//JANUS_LOG(LOG_INFO, "%s scene(%d) page(%d) keyFrame offset: %d\n", whiteboard->filename, tmp_keyframe->scene, tmp_keyframe->page, tmp_keyframe->offset);
			ret = fseek(whiteboard->file, tmp_keyframe->offset, SEEK_SET);
			if (ret < 0) {
				JANUS_LOG(LOG_ERR, "seek file keyframe offset error.\n");
				g_free(keyframe_buffer);
				keyframe_buffer = NULL;
				continue;
			}
			char *buf = NULL;
			if (janus_whiteboard_read_packet_from_file(&buf, &pkt_len, whiteboard->file) > 0) {
				if (buf == NULL) {
					g_free(keyframe_buffer);
					keyframe_buffer = NULL;
					continue;
				}

				Pb__Package *package = pb__package__unpack(NULL, pkt_len, (const uint8_t*)buf);
				if (package == NULL) {
					JANUS_LOG(LOG_WARN, "Parse whiteboard scene data error.\n");
					fseek(whiteboard->file, 0, SEEK_END);
					g_free(buf);
					g_free(keyframe_buffer);
					return -1;
				}

				Pb__KeyFrame *next = g_malloc0(sizeof(Pb__KeyFrame));
				pb__key_frame__init(next);
				next->offset    = tmp_keyframe->offset;
				next->scene     = tmp_keyframe->scene;
				next->page 		= tmp_keyframe->page;
				next->timestamp = tmp_keyframe->timestamp;

				janus_scene *tmp_scene = janus_whiteboard_get_scene(whiteboard, package->scene);
				if (tmp_scene && package->page >= 0 && package->page < tmp_scene->page_num) {
					Pb__KeyFrame **target_keyframe = &tmp_scene->page_keyframes[package->page];
					if (*target_keyframe != NULL) {
						g_free(*target_keyframe);
					}
					*target_keyframe = next;
					if (package->page >= tmp_scene->page_keyframe_maxnum) {
						tmp_scene->page_keyframe_maxnum = package->page + 1;//scene 从 0 开始
					}

					// 最后一个关键帧最接近上次白板的最后一页
					whiteboard->scene = package->scene;
					whiteboard->page = package->page;
				}

				// remove tmp package
				pb__package__free_unpacked(package, NULL);
				g_free(buf);
				buf = NULL;
			}

			pb__key_frame__free_unpacked(tmp_keyframe, NULL);
		}
		g_free(keyframe_buffer);
		keyframe_buffer = NULL;
	}

	// 一直读到file的末尾，以得出最后页面所在的场景。用于恢复
	janus_scene *scene = janus_whiteboard_get_scene(whiteboard, whiteboard->scene);
	if (whiteboard->scene >= 0 && scene != NULL && 
		whiteboard->page >= 0 && whiteboard->page < scene->page_num && scene->page_keyframes[whiteboard->page] != NULL) {

		fseek(whiteboard->file, scene->page_keyframes[whiteboard->page]->offset, SEEK_SET);
		Pb__Package *tmp_pkt = NULL;
		char *buffer = NULL;
		while(janus_whiteboard_read_packet_from_file(&buffer, &pkt_len, whiteboard->file) > 0) {
			//JANUS_LOG(LOG_INFO, "pakcage len: %d\n", pkt_len);
			if(buffer == NULL) {
				continue;
			}

			if (tmp_pkt != NULL) {
				// 释放先前解包留下的packet
				pb__package__free_unpacked(tmp_pkt, NULL);
				tmp_pkt = NULL;
			}
			tmp_pkt = pb__package__unpack(NULL, pkt_len, (const uint8_t*)buffer);
			if (tmp_pkt == NULL) {
				JANUS_LOG(LOG_WARN, "Get an invalid packet when parse header\n");
			}
			g_free(buffer);
		}
		if (tmp_pkt != NULL) {
			/* package 的 timestamp 是以毫秒为单位的持续时间，不是日期的时间戳 */
			whiteboard->scene = tmp_pkt->scene;
			whiteboard->page = tmp_pkt->page;
			whiteboard->start_timestamp = janus_whiteboard_get_current_time_l() - tmp_pkt->timestamp;
		} else {
			int64_t last_timestamp = scene->page_keyframes[whiteboard->page]->timestamp;
			whiteboard->start_timestamp = janus_whiteboard_get_current_time_l() - last_timestamp;
		}
	} else {
		JANUS_LOG(LOG_WARN, "Get an invalid packet when parse header\n");
		whiteboard->start_timestamp = janus_whiteboard_get_current_time_l();
	}

	fseek(whiteboard->file, 0, SEEK_END);
	fseek(whiteboard->scene_file, 0, SEEK_END);
	fseek(whiteboard->page_file, 0, SEEK_END);
	JANUS_LOG(LOG_INFO, "whiteboard：--->Parse or create header success:(%d, %d)\n", whiteboard->scene, whiteboard->page);

	return 0;
}

/*! 创建白板模块，如果创建成功，则尝试从文件里读取历史保留的数据到当前场景(scene) */
janus_whiteboard *janus_whiteboard_create(const char *oss_path, const char *local_dir, const char *filename) {
	/* Create the recorder */
	janus_whiteboard *whiteboard = janus_whiteboard_create_with_file(local_dir, filename);
	if(whiteboard == NULL) {
		JANUS_LOG(LOG_FATAL, "Out of Memory when init whiteboard with oss!\n");
		return NULL;
	}

    if (janus_whiteboard_parse_or_create_header_l(whiteboard) < 0) {
    	    JANUS_LOG(LOG_ERR, "Parse or create header error.\n");
    	    return NULL;
    }
	janus_mutex_init(&whiteboard->mutex);

    //从文件恢复历史保存的白板数据
	whiteboard->packages = g_ptr_array_new_full(BASE_PACKET_CAPACITY, (GDestroyNotify)janus_whiteboard_package_free);
	if (!whiteboard->packages) {
		JANUS_LOG(LOG_ERR, "Out of Memory when alloc memory for %d struct scene page packages!\n", MAX_PACKET_CAPACITY);
		g_free(whiteboard);
		return NULL;
	}
	janus_whiteboard_scene_page_data_l(whiteboard, whiteboard->scene, whiteboard->page, whiteboard->packages);

	return whiteboard;
}

/*! */
int janus_whiteboard_add_scene_l(janus_whiteboard *whiteboard, Pb__Scene *newScene) {
	janus_scene *j_scene = g_malloc0(sizeof(janus_scene));
	j_scene->type       = newScene->type; 
	j_scene->source_url = g_strdup(newScene->resource);
	j_scene->page_num = newScene->pagecount;
	j_scene->page_keyframes = g_malloc0(sizeof(Pb__KeyFrame*) * j_scene->page_num);
	j_scene->page_keyframe_maxnum = 0;
	if (newScene->resourceid != NULL) {
		j_scene->source_id = g_strdup(newScene->resourceid);
	} else {
		j_scene->source_id = "";
	}

	int scene_num = g_hash_table_size (whiteboard->scenes);
	if (newScene->index == -1) {
		newScene->index = scene_num;
	}
	j_scene->index = newScene->index;
	
	if (newScene->index > scene_num) {
		JANUS_LOG(LOG_INFO, "whiteboard:janus_whiteboard_add_scene_l: error index: %d\n", newScene->index);
		return -1;
	} else {
		janus_scene *tmp_scene = janus_whiteboard_get_scene(whiteboard, newScene->index);
		if (tmp_scene) {
			JANUS_LOG(LOG_INFO, "whiteboard:source_url: %s, %s\n", tmp_scene->source_url, j_scene->source_url);
			if (strcmp(tmp_scene->source_url, j_scene->source_url) == 0) {
				g_free(j_scene->page_keyframes);
				g_free(j_scene);
				return -1;
			}
			if (tmp_scene->page_num > 0 && tmp_scene->page_keyframes) {
				JANUS_LOG(LOG_INFO, "janus_whiteboard_add_scene_l 000000000000000\n");
				// g_free(tmp_scene->page_keyframes);
				// g_free(tmp_scene->source_url);
			}
			// g_free(tmp_scene);
		}
		g_hash_table_insert(whiteboard->scenes, GINT_TO_POINTER(newScene->index), j_scene);
	}
	
	/*! 将 keyframe 保存到文件 */
	size_t length = pb__scene__get_packed_size(newScene);
	void *buffer = g_malloc0(length);
	if (length!= 0 && buffer == NULL) {
		JANUS_LOG(LOG_WARN, "Save scene data fail. Out of memory when allocating memory for tmp file buffer\n");
		return -1;
	}
	JANUS_LOG(LOG_INFO, "janus_whiteboard_add_scene_l  44444444444444444\n");
	pb__scene__pack(newScene, buffer);
	fseek(whiteboard->scene_file, 0, SEEK_END);
	size_t ret = fwrite(&length, sizeof(size_t), 1, whiteboard->scene_file);
	if (ret == 1) {
	    ret = janus_whiteboard_write_packet_to_file_l(buffer, length, whiteboard->scene_file);
	    ret = (ret==0) ? 1 : 0;//由于以上函数封装的关系，此处需要对返回的结果处理下 
	}
	if (ret == 0) {
		JANUS_LOG(LOG_ERR, "Error happens when saving keyframe packet index to basefile: %s\n", whiteboard->filename);
		return -1;
	}
	JANUS_LOG(LOG_INFO, "janus_whiteboard_add_scene_l  5555555555555\n");
	return 1;
}

janus_scene *janus_whiteboard_get_scene(janus_whiteboard *whiteboard, int scene_index) {
	return (janus_scene*)g_hash_table_lookup(whiteboard->scenes, GINT_TO_POINTER(scene_index));
}

janus_whiteboard_result janus_whiteboard_add_scene(janus_whiteboard *whiteboard, int package_type, char *resource_id, char *resource, int page_count, int type, int index) {
	JANUS_LOG(LOG_INFO, "janus_whiteboard_add_scene: %s, %d, %d\n", resource, page_count, type);
	janus_whiteboard_result result = 
	{
		.ret          = -1, /* 大于0时表示发起者发出了指令，将有数据需要返回. */
		.keyframe_len = 0,
		.keyframe_buf = NULL,
		.command_len  = 0,
		.command_buf  = NULL,
		.package_type = KLPackageType_None,
	};

	if(!whiteboard) {
		JANUS_LOG(LOG_ERR, "Error saving frame. Whiteboard is empty\n");
		return result;
	}
	if(page_count <= 0) {
		JANUS_LOG(LOG_WARN, "scene page count <= 0: invalid parameter.\n");
		return result;
	}
	
	Pb__Package package;
	pb__package__init(&package);
	package.type = package_type;
	package.timestamp = janus_whiteboard_get_current_time_l() - whiteboard->start_timestamp;
	package.newscene = g_malloc0(sizeof(Pb__Scene));
	if (!package.newscene) {
		JANUS_LOG(LOG_WARN, "Error malloc Pb__Scene\n");
		return result;
	}	
	pb__scene__init(package.newscene);
	package.newscene->type = type;
	package.newscene->resource = g_strdup(resource);
	package.newscene->pagecount = page_count;
	package.newscene->index = index;
	package.newscene->resourceid = g_strdup(resource_id);

	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if(!whiteboard->file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		g_free(package.newscene);
		JANUS_LOG(LOG_WARN, "Error saving frame. whiteboard->file is empty\n");
		return result;
	}

	result.ret = janus_whiteboard_add_scene_l(whiteboard, package.newscene);
	if (result.ret <= 0) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		g_free(package.newscene);
		JANUS_LOG(LOG_WARN, "Error add scene.\n");
		return result;
	}
	JANUS_LOG(LOG_INFO, "whiteboard:newscene: %s, %d, %d\n", package.newscene->resource, package.newscene->pagecount, package.newscene->index);
	result.ret = package.newscene->index;
	result.command_len = pb__package__get_packed_size(&package);
	result.command_buf = g_malloc0(result.command_len);
	pb__package__pack(&package, result.command_buf);
	result.package_type = KLPackageType_AddScene;

	g_free(package.newscene);
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	return result;
}

/*! */
int janus_whiteboard_scenes_data(janus_whiteboard * whiteboard, Pb__Scene **scenes) {
	GHashTableIter iter;
	gpointer key, value;
	int scene_num = 0;
	g_hash_table_iter_init (&iter, whiteboard->scenes);
	while (g_hash_table_iter_next (&iter, &key, &value))
	{
		janus_scene *janusScene = (janus_scene*)value;
		if (janusScene != NULL) {
			int i = janusScene->index;
			scenes[i] = g_malloc0(sizeof(Pb__Scene));
			pb__scene__init(scenes[i]);
			scenes[i]->index = janusScene->index;
			scenes[i]->type = janusScene->type;
			scenes[i]->resource = g_strdup(janusScene->source_url);
			scenes[i]->pagecount = janusScene->page_num;
			if (janusScene->source_id != NULL) {
				scenes[i]->resourceid = g_strdup(janusScene->source_id);
			} else {
				scenes[i]->resourceid = "";
			}

			if (i >= scene_num) {
				scene_num = i + 1;
			}
		}
	}
	return scene_num;
}

/*! 将所有的指令集合到一个pkp，再打包为 buffer 二进制数据返回给客户端
    len=0的时候继续打包，返回一个清屏指令 */
void janus_whiteboard_packed_data_l(GPtrArray *packages, janus_whiteboard_result* result) {
	if (packages == NULL || result == NULL) {
		result->ret = -1;
		return;
	}

	// 打包 command 数据
	int total_cmd_num = 0;
	for (int i = 0; i < packages->len; i ++) {
		Pb__Package *package = (Pb__Package*)g_ptr_array_index(packages, i);
		if (package != NULL)
			total_cmd_num += package->n_cmd;
	}

	Pb__Package out_pkg;
	if (packages->len == 0) {
		pb__package__init(&out_pkg);
		out_pkg.scene = 0;
		out_pkg.type  = KLPackageType_CleanDraw;
	} else {
		pb__package__init(&out_pkg);
		Pb__Package *package = (Pb__Package*)g_ptr_array_index(packages, 0);
		out_pkg.page  = package->page;
		out_pkg.scene = package->scene;
		out_pkg.type  = KLPackageType_DrawCommand;
		out_pkg.n_cmd = total_cmd_num;
		out_pkg.cmd   = g_malloc0(sizeof(Pb__Command) * total_cmd_num);
	}

	size_t cmd_index = 0;
	for (int i = 0; i < packages->len; i ++) {
		Pb__Package *package = (Pb__Package*)g_ptr_array_index(packages, i);
		Pb__Command **cmd = package->cmd;
		for (size_t j = 0; j < package->n_cmd; j ++) {
			out_pkg.cmd[cmd_index++] = cmd[j];
		}
	}

	result->command_len = pb__package__get_packed_size(&out_pkg);
	result->command_buf = g_malloc0(result->command_len);
	pb__package__pack(&out_pkg, result->command_buf);
	g_free(out_pkg.cmd);

	// 打包 keyframe 数据
	if (packages->len > 0) {
		Pb__Package *first_package = g_ptr_array_index(packages, 0);
		if (first_package->type == KLPackageType_KeyFrame) {
			result->keyframe_len = pb__package__get_packed_size(first_package);
			result->keyframe_buf = g_malloc0(result->keyframe_len);
			pb__package__pack(first_package, result->keyframe_buf);
		}
	}
}

void janus_whiteboard_add_pkt_to_packages_l(GPtrArray *packages, Pb__Package* dst_pkg) {
	if (packages == NULL || dst_pkg == NULL)
		return;

	if (dst_pkg->type == KLPackageType_CleanDraw) {
	    // 清屏指令，移除已经存在的包.
	    g_ptr_array_set_size(packages, 0);
    } else if (dst_pkg->type == KLPackageType_KeyFrame) {
		// 遇到关键帧，移除已经存在的包.
		g_ptr_array_set_size(packages, 0);
		g_ptr_array_add(packages, dst_pkg);
	} else if (dst_pkg->type == KLPackageType_SwitchScenePage) {
		// 只需要存一个占位
		if (packages->len == 0) {
			g_ptr_array_add(packages, dst_pkg);
		}
	} else if (dst_pkg->type != KLPackageType_ScenePageData) {
		// FIXME:Rison 有新的指令过来需要考虑这里. 过滤掉特殊指令
		g_ptr_array_add(packages, dst_pkg);
	}
}

/*! 从指定的场景获取白板笔迹。先移到当前场景最接近keyframe附近开始查找，需要对clean以及keyframe做额外处理
    @returns 成功获取返回 pkt 的数目， 获取失败返回 -1 */
int janus_whiteboard_scene_page_data_l(janus_whiteboard *whiteboard, int scene, int page, GPtrArray* packages) {
	if (whiteboard == NULL || whiteboard->file == NULL)
		return -1;
	int scene_num = g_hash_table_size(whiteboard->scenes);
	if (scene < 0) {
		JANUS_LOG(LOG_WARN, "\"scene_page_data_l\" got a current scene data");
		scene = whiteboard->scene;
	}
	if (scene <0 || scene >= scene_num) {
		JANUS_LOG(LOG_ERR, "\"scene_page_data_l\" got a request with invalid scene(%d).", scene);
		return -1;
	}
	int package_data_offset = 0;
	janus_scene *scene_data = janus_whiteboard_get_scene(whiteboard, scene);
	
	if (scene_data == NULL) {
		JANUS_LOG(LOG_ERR, "\"scene_page_data_l\" no scene(%d) data.", scene);
		return -1;
	}
	if (page < 0) {
		page  = whiteboard->page;
	}
	if (page < 0 || page >= scene_data->page_num) {
		JANUS_LOG(LOG_ERR, "\"scene_page_data_l\" got a request with invalid page(%d).", page);
		return -1;
	}

	Pb__KeyFrame *target_keyframe = scene_data->page_keyframes[page];
	if (target_keyframe != NULL) {
		package_data_offset = target_keyframe->offset;
		JANUS_LOG(LOG_VERB, "Get scene data offset %d\n", package_data_offset);
	}
	// seek 到文件开头。FIXME:Rison 使用数组存起来 scene--->offset, 就不需要每次从头开始读取了
	fseek(whiteboard->file, package_data_offset, SEEK_SET);
	size_t pkt_len, out_len = 0;
	char *buffer = NULL;

	while(janus_whiteboard_read_packet_from_file(&buffer, &pkt_len, whiteboard->file)) {
		if(buffer == NULL) {
			JANUS_LOG(LOG_VERB, "janus_whiteboard_read_packet_from_file error %d\n", pkt_len);
			out_len = -1;
			break;
		}

		Pb__Package *package = pb__package__unpack(NULL, pkt_len, (const uint8_t*)buffer);
		if (package == NULL) {
			JANUS_LOG(LOG_WARN, "Parse whiteboard scene data error.\n");
			fseek(whiteboard->file, 0, SEEK_END);
			g_free(buffer);
			out_len = -1;
			break; //FIXME:Rison: break or continue?
		}

		if (package->scene == scene && package->page == page) {
			janus_whiteboard_add_pkt_to_packages_l(packages, package);
		} else {
			pb__package__free_unpacked(package, NULL);
		}

		g_free(buffer);
		buffer = NULL;
	}

	fseek(whiteboard->file, 0, SEEK_END);
	return out_len;
}

int janus_whiteboard_have_keyframe_l(janus_whiteboard *whiteboard, int scene, int page) {
	janus_scene *scene_data = janus_whiteboard_get_scene(whiteboard, scene);
	if (scene_data && scene_data->page_keyframes) {
		Pb__KeyFrame *keyFrame = scene_data->page_keyframes[page];
		if (keyFrame != NULL) {
			return 1;
		}
	}
	return 0;
}

int janus_whiteboard_on_receive_keyframe_l(janus_whiteboard *whiteboard, Pb__Package *package) {
	if (!whiteboard->header_file || !package)
		return -1;

	if (package->scene < 0 || package->scene >= MAX_PACKET_CAPACITY) {
		JANUS_LOG(LOG_WARN, "Save keyframe fail. current is %d, MAX_PACKET_CAPACITY is %d\n", package->scene, MAX_PACKET_CAPACITY);
		return -1;
	}

	Pb__KeyFrame *next = g_malloc0(sizeof(Pb__KeyFrame));
	if (next == NULL) {
		JANUS_LOG(LOG_WARN, "Save keyframe fail. Out of memory when allocating memory for new frame\n");
		return -1;
	}
	fseek(whiteboard->file, 0, SEEK_END);
	pb__key_frame__init(next);
	next->offset    = ftell(whiteboard->file);
	next->scene     = package->scene;
	next->page 		= package->page;
	next->timestamp = package->timestamp;

	janus_scene *scene_data = janus_whiteboard_get_scene(whiteboard, package->scene);
	int page_index = package->page;
	Pb__KeyFrame **target_keyframe = &scene_data->page_keyframes[page_index];
	if (*target_keyframe != NULL) {
		g_free(*target_keyframe);
	}
	*target_keyframe = next;
	if (page_index >= scene_data->page_keyframe_maxnum) {
	    scene_data->page_keyframe_maxnum = page_index + 1;//scene 从 0 开始
	}

	/*! 将 keyframe 保存到文件 */
	size_t length = pb__key_frame__get_packed_size(*target_keyframe);
	void *buffer = g_malloc0(length);
	if (length!= 0 && buffer == NULL) {
		JANUS_LOG(LOG_WARN, "Save keyframe fail. Out of memory when allocating memory for tmp file buffer\n");
		return -1;
	}

	pb__key_frame__pack(*target_keyframe, buffer);
	fseek(whiteboard->header_file, 0, SEEK_END);
	size_t ret = fwrite(&length, sizeof(size_t), 1, whiteboard->header_file);
	if (ret == 1) {
	    ret = janus_whiteboard_write_packet_to_file_l(buffer, length, whiteboard->header_file);
	    ret = (ret==0) ? 1 : 0;//由于以上函数封装的关系，此处需要对返回的结果处理下 
	}
	if (ret == 0) {
		JANUS_LOG(LOG_ERR, "Error happens when saving keyframe packet index to basefile: %s\n", whiteboard->filename);
		return -1;
	}

	return 0;
}

/*! 将用户发过来的switch scene存起来，用于回访的时候快速定位 */
int janus_whiteboard_on_receive_switch_scene_l(janus_whiteboard *whiteboard, Pb__Package *package) {
	if (!whiteboard->scene_file || !package)
		return -1;
	int scene_num = g_hash_table_size(whiteboard->scenes);
	if (package->scene < 0 || package->scene >= scene_num) {
		JANUS_LOG(LOG_WARN, "Out of index on saving switch scene package, it's is %d\n", package->scene);
		return -1;
	}

	Pb__PageIndex nextPage;
	pb__page_index__init(&nextPage);
	nextPage.scene     = package->scene;
	nextPage.page 	   = package->page;
	nextPage.timestamp = package->timestamp;
	size_t length = pb__page_index__get_packed_size(&nextPage);
	void *buffer = g_malloc0(length);
	if (length != 0 && buffer == NULL) {
		JANUS_LOG(LOG_ERR, "Out of memory when allocating memory for tmp switch scene index buffer\n");
		return -1;
	}
	pb__page_index__pack(&nextPage, buffer);
	size_t ret = fwrite(&length, sizeof(length), 1, whiteboard->page_file);
	if (ret == 1) {
		ret = janus_whiteboard_write_packet_to_file_l(buffer, length, whiteboard->page_file);
		ret = (ret==0) ? 1 : 0;
	}
	if (ret == 0) {
		JANUS_LOG(LOG_ERR, "Error happens when saving switch scene index to basefile: %s\n", whiteboard->filename);
		return -1;
	}

	return 0;
}

/*! 核心函数
    本函数尝试以白板数据进行解包，对 场景切换，获取场景数据进行额外处理。其余情况正常保存。
    //FIXME:Rison 是否需要保存场景切换这些包，因为回访也需要用到?
    @returns 保存成功返回非负数。如果有数据返回则表示返回数据的长度，*/
janus_whiteboard_result janus_whiteboard_save_package(janus_whiteboard *whiteboard, char *buffer, size_t length) {
	janus_whiteboard_result result = 
	{
		.ret          = -1, /* 大于0时表示发起者发出了指令，将有数据需要返回. */
		.keyframe_len = 0,
		.keyframe_buf = NULL,
		.command_len  = 0,
		.command_buf  = NULL,
		.package_type = KLPackageType_None,
	};

	if(!whiteboard) {
		JANUS_LOG(LOG_ERR, "Error saving frame. Whiteboard is empty\n");
		return result;
	}
	if(!buffer || length <= 0) {
		JANUS_LOG(LOG_WARN, "Error saving frame. Invalid params\n");
		return result;
	}

	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if(!whiteboard->file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_WARN, "Error saving frame. whiteboard->file is empty\n");
		return result;
	}

	Pb__Package *package = pb__package__unpack(NULL, length, (const uint8_t*)buffer);
	if (package == NULL) {
		JANUS_LOG(LOG_WARN, "Error saving frame. Invalid whiteboard packet\n");
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return result;
	}
	package->timestamp = janus_whiteboard_get_current_time_l() - whiteboard->start_timestamp;
	JANUS_LOG(LOG_INFO, "whiteboard: package——type：%d\n", package->type);
	if (package->type == KLPackageType_AddScene) {
		// add whiteboard scene
		result.ret = janus_whiteboard_add_scene_l(whiteboard, package->newscene);
		JANUS_LOG(LOG_INFO, "whiteboard:newscene: %s, %d, %d\n", package->newscene->resource, package->newscene->pagecount, package->newscene->index);
		result.command_len = pb__package__get_packed_size(package);
		result.command_buf = g_malloc0(result.command_len);
		pb__package__pack(package, result.command_buf);
		result.package_type = KLPackageType_AddScene;
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		pb__package__free_unpacked(package, NULL);
		return result;
	} else if (package->type == KLPackageType_SceneData) {
		// get whiteboard scene data
		int scene_num = g_hash_table_size(whiteboard->scenes);
		result.ret = 1;
		if (scene_num > 0) {
			JANUS_LOG(LOG_INFO, "whiteboard：KLPackageType_SceneData\n");
			Pb__Package out_package = *package;
			out_package.page   = whiteboard->page;
			out_package.scene  = whiteboard->scene;
			result.command_len = pb__package__get_packed_size(&out_package);
			JANUS_LOG(LOG_INFO, "whiteboard:packed_size%d\n", result.command_len);
			result.command_buf = g_malloc0(result.command_len);
			int size = pb__package__pack(&out_package, result.command_buf);
			JANUS_LOG(LOG_INFO, "whiteboard:command_buf_size%d\n", size);
		} else {
			result.ret = -1;
		}
		result.package_type = KLPackageType_SceneData;
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		pb__package__free_unpacked(package, NULL);
		return result;
	} else if (package->type == KLPackageType_SwitchScenePage) {
	    // 切换白板场景
		if (package->scene == whiteboard->scene && package->page == whiteboard->page) {
			JANUS_LOG(LOG_WARN, "Get a request to switch scene page, but currenttly the whiteboard is on the target %d scene %d page\n", package->scene, package->page);
		    janus_mutex_unlock_nodebug(&whiteboard->mutex);
		    result.ret = 0;
		    pb__package__free_unpacked(package, NULL);
			return result;
		} else if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a request to switch scene page, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
		    result.ret = -1;
		    pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
		janus_whiteboard_on_receive_switch_scene_l(whiteboard, package);
		g_ptr_array_set_size(whiteboard->packages, 0);
		whiteboard->scene = package->scene;
		whiteboard->page = package->page;
		int ret = janus_whiteboard_scene_page_data_l(whiteboard, whiteboard->scene, whiteboard->page, whiteboard->packages);
		if (ret < 0) {
			JANUS_LOG(LOG_WARN, "Something wrong happens when fetching scene data with reselt %d\n", ret);
		}
	} else if(package->type == KLPackageType_CleanDraw) {
		if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a request get scene page, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
		    result.ret = -1;
		    pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
	    // 清屏
		janus_scene *scene = janus_whiteboard_get_scene(whiteboard, package->scene);
		if (whiteboard->scene == package->scene && whiteboard->page == package->page) {
			g_ptr_array_set_size(whiteboard->packages, 0);
			JANUS_LOG(LOG_INFO, "Get a clear screen command, clear local cache data now\n");
		}
	} else if (package->type == KLPackageType_ScenePageData) {
		if (package->scene < 0 && package->page < 0) {
			package->scene = whiteboard->scene;
			package->page = whiteboard->page;
		}
		if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a request get scene page, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
		    result.ret = -1;
		    pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
		JANUS_LOG(LOG_VERB, "whiteboard：Get scene page data(%d, %d)/(%d, %d)\n", package->scene, package->page, whiteboard->scene, whiteboard->page);
	    // 请求指定场景的白板数据
		if (package->scene == whiteboard->scene && package->page == whiteboard->page) {
			// -1的情况表示请求当前场景
			janus_whiteboard_packed_data_l(whiteboard->packages, &result);
		} else {
			// 其他场景
			GPtrArray *packages = g_ptr_array_new_full(BASE_PACKET_CAPACITY, (GDestroyNotify)janus_whiteboard_package_free);
			int num = janus_whiteboard_scene_page_data_l(whiteboard, package->scene, package->page, packages);
			janus_whiteboard_packed_data_l(packages, &result);
			g_ptr_array_unref(packages);
		}
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_VERB, "whiteboard：Get scene data with keyframe:%d, command:%d\n", result.keyframe_len, result.command_len);
		pb__package__free_unpacked(package, NULL);
		result.ret = 1;
		return result;
	}

	//if (package->type != KLPackageType_ScenePageData)// 此处无需考虑非scene data的情况，因为这种情况已在前面处理并返回
	if (package->type == KLPackageType_KeyFrame || package->type == KLPackageType_CleanDraw) {
		if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a request to switch scene page, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
		    result.ret = -1;
		    pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
	    // 额外处理关键帧
		janus_whiteboard_on_receive_keyframe_l(whiteboard, package);
	} else if (package->type == KLPackageType_SwitchScenePage) {
		// 切换到新页面或者新场景的数据为0也可以认为是关键帧
		if (whiteboard->packages->len == 0) {
			janus_whiteboard_on_receive_keyframe_l(whiteboard, package);
		}
	} else if (!janus_whiteboard_have_keyframe_l(whiteboard, package->scene, package->page)) {
		// 修复第一个包不是关键帧的问题
		janus_whiteboard_on_receive_keyframe_l(whiteboard, package);
	}

	// 写入到文件记录保存
	fseek(whiteboard->file, 0, SEEK_END);
	size_t len = pb__package__get_packed_size(package);
	size_t ret = fwrite(&len, sizeof(size_t), 1, whiteboard->file);
	if (ret == 1) {
		void*  buf = g_malloc0(len);
		if (buf != NULL) {
			pb__package__pack(package, buf);
			ret = janus_whiteboard_write_packet_to_file_l(buf, len, whiteboard->file);
			g_free(buf);
		} else {
			JANUS_LOG(LOG_WARN, "Get packed size is 0 when saving packet to file\n");
			ret = -1;
		}
		ret = (ret==0) ? 1 : 0;//由于以上函数封装的关系，此处需要对返回的结果处理下 
	}
	if (ret == 0) {
		JANUS_LOG(LOG_ERR, "Error happens when saving scene data packet to basefile: %s\n", whiteboard->filename);
	}
	
	// 保存数据到当前场景（内存），以便快速处理KLPackageType_ScenePageData指令
	if (package->scene == whiteboard->scene && package->page == whiteboard->page) {
		janus_whiteboard_add_pkt_to_packages_l(whiteboard->packages, package);
	} else {
		pb__package__free_unpacked(package, NULL);
	}

	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	result.ret = 0;
	return result;
}

int janus_whiteboard_close(janus_whiteboard *whiteboard) {
	if (!whiteboard)
		return -1;
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if (whiteboard->file) {
		fseek(whiteboard->file, 0L, SEEK_END);
		size_t fsize = ftell(whiteboard->file);
		JANUS_LOG(LOG_INFO, "whiteboard data file is %zu bytes: %s\n", fsize, whiteboard->filename);
	}
	if (whiteboard->header_file) {
		fseek(whiteboard->header_file, 0L, SEEK_END);
		size_t fsize = ftell(whiteboard->header_file);
		JANUS_LOG(LOG_INFO, "whiteboard header file is %zu bytes: %s\n", fsize, whiteboard->filename);
	}
	if (whiteboard->scene_file) {
		fseek(whiteboard->scene_file, 0L, SEEK_END);
		size_t fsize = ftell(whiteboard->scene_file);
		JANUS_LOG(LOG_INFO, "whiteboard scene file is %zu bytes: %s\n", fsize, whiteboard->filename);
	}
	if (whiteboard->page_file) {
		fseek(whiteboard->page_file, 0L, SEEK_END);
		size_t fsize = ftell(whiteboard->page_file);
		JANUS_LOG(LOG_INFO, "whiteboard scene file is %zu bytes: %s\n", fsize, whiteboard->filename);
	}

	if (janus_oss_io && whiteboard->scene_info) {
		janus_oss_io->io_info_close(whiteboard->scene_info);
		janus_io_info_destroy(whiteboard->scene_info);
		whiteboard->scene_info = NULL;
		JANUS_LOG(LOG_INFO, "close scene info to oss.");
	}
	if (janus_oss_io && whiteboard->header_info) {
		janus_oss_io->io_info_close(whiteboard->header_info);
		janus_io_info_destroy(whiteboard->header_info);
		whiteboard->header_info = NULL;
		JANUS_LOG(LOG_INFO, "close header info to oss.");
	}
	if (janus_oss_io && whiteboard->page_info) {
		janus_oss_io->io_info_close(whiteboard->page_info);
		janus_io_info_destroy(whiteboard->page_info);
		whiteboard->page_info = NULL;
		JANUS_LOG(LOG_INFO, "close page info to oss.");
	}
	if (janus_oss_io && whiteboard->packet_info) {
		janus_oss_io->io_info_close(whiteboard->packet_info);
		janus_io_info_destroy(whiteboard->packet_info);
		whiteboard->packet_info = NULL;
		JANUS_LOG(LOG_INFO, "close data info to oss.");
	}
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	return 0;
}

/*! 将header和data保存到一个文件
    @returns 失败返回-1， 正常返回1 */
int janus_whiteboard_generate_and_save_l(janus_whiteboard *whiteboard) {
	if (!whiteboard || !whiteboard->header_file || !whiteboard->file)
		return -1;
	Pb__Header header;
	pb__header__init(&header);
	header.version     = 1;
	header.duration    = janus_whiteboard_get_current_time_l() - whiteboard->start_timestamp;
	header.n_keyframes = 0;
	header.keyframes   = g_malloc0(sizeof(Pb__KeyFrame*) * MAX_PACKET_CAPACITY);
	if (header.keyframes == NULL) {
		JANUS_LOG(LOG_ERR, "Oop, out of memory when alloc memory for creating header.keyframes\n");
		return -1;
	}
	header.n_pageindexs = 0;
	header.pageindexs   = g_malloc0(sizeof(Pb__PageIndex*) * MAX_PACKET_CAPACITY);
	if (header.pageindexs == NULL) {
		JANUS_LOG(LOG_ERR, "Oop, out of memory when alloc memory for creating header.pageindexs\n");
		return -1;
	}
	header.n_scenes = g_hash_table_size(whiteboard->scenes);
	header.scenes = g_malloc0(sizeof(Pb__Scene*) * header.n_scenes);

	// 将keyframe读取进内存
	fseek(whiteboard->header_file, 0L, SEEK_SET);
	size_t keyframe_len = 0;
	while(fread(&keyframe_len, sizeof(keyframe_len), 1, whiteboard->header_file) == 1) {
		char *buffer = g_malloc0(keyframe_len);
		if (buffer == NULL) {
			JANUS_LOG(LOG_ERR, "Oop, out of memory when alloc memory for reading keyframe file.\n");
			break;
		}
		if (janus_whiteboard_read_packet_from_file_l(buffer, keyframe_len, whiteboard->header_file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading keyframe index packet from basefile: %s\n", whiteboard->filename);
			g_free(buffer);
			break;
		}
		Pb__KeyFrame *tmp_keyframe = pb__key_frame__unpack(NULL, keyframe_len, (const uint8_t*)buffer);
		if (tmp_keyframe != NULL) {
			header.keyframes[header.n_keyframes] = tmp_keyframe;
			header.n_keyframes ++;
			if (header.n_keyframes >= MAX_PACKET_CAPACITY) {
				JANUS_LOG(LOG_WARN, "Can not push more keyframes now. header is bigger than MAX_PACKET_CAPACITY:%d\n", MAX_PACKET_CAPACITY);
				g_free(buffer);
				break;
			}
		}
		g_free(buffer);
	}

	// 将switch scene读进内存
	fseek(whiteboard->page_file, 0L, SEEK_SET);
	size_t switchscene_len = 0;
	while(fread(&switchscene_len, sizeof(switchscene_len), 1, whiteboard->page_file) == 1) {
		char *buffer = g_malloc0(switchscene_len);
		if (buffer == NULL) {
			JANUS_LOG(LOG_ERR, "Oop, out of memory when alloc memory for reading scene file\n");
			break;
		}
		if (janus_whiteboard_read_packet_from_file_l(buffer, switchscene_len, whiteboard->page_file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading switch scene index packet from basefile: %s\n", whiteboard->filename);
			g_free(buffer);
			break;
		}
		Pb__PageIndex *tmp_switchscene = pb__page_index__unpack(NULL, switchscene_len, (const uint8_t*)buffer);
		if (tmp_switchscene != NULL) {
			header.pageindexs[header.n_pageindexs] = tmp_switchscene;
			header.n_pageindexs ++;
			if (header.n_pageindexs >= MAX_PACKET_CAPACITY) {
				JANUS_LOG(LOG_WARN, "Can't push more switchscene package now. it is bigger than MAX_PACKET_CAPACITY:%d\n", MAX_PACKET_CAPACITY);
				g_free(buffer);
				break;
			}
		}
		g_free(buffer);
	}

	// 将scene data读进内存
	fseek(whiteboard->scene_file, 0L, SEEK_SET);
	size_t scene_data_len = 0;
	while(fread(&scene_data_len, sizeof(scene_data_len), 1, whiteboard->scene_file) == 1) {
		char *buffer = g_malloc0(scene_data_len);
		if (buffer == NULL) {
			JANUS_LOG(LOG_ERR, "Oop, out of memory when alloc memory for reading scene file\n");
			break;
		}
		if (janus_whiteboard_read_packet_from_file_l(buffer, scene_data_len, whiteboard->scene_file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading switch scene data from basefile: %s\n", whiteboard->filename);
			g_free(buffer);
			break;
		}
		Pb__Scene *tmp_scene = pb__scene__unpack(NULL, scene_data_len, (const uint8_t*)buffer);
		if (tmp_scene != NULL && tmp_scene->index < header.n_scenes) {
			header.scenes[tmp_scene->index] = tmp_scene;
		}
		g_free(buffer);
	}

	// 输出到文件
	const size_t file_len = 1024;
	char file_name[file_len];
	memset(file_name, 0, file_len);
	if(whiteboard->dir) {
		g_snprintf(file_name, file_len, "%s/%s", whiteboard->dir, whiteboard->filename);
	} else {
		g_snprintf(file_name, file_len, "%s", whiteboard->filename);
	}
	FILE *file = fopen(file_name, "ab+");
	if (file != NULL) {
		// 打包header
		size_t header_len = pb__header__get_packed_size(&header);
		size_t ret = fwrite(&header_len, sizeof(size_t), 1, file);
		if (ret == 1) {
			void * header_buf = g_malloc0(header_len);
			if (header_len != 0 && header_buf == NULL) {
				JANUS_LOG(LOG_ERR, "Oop, out of memory when alloc memory for saving headers.\n");
				g_free(header.keyframes);
				fclose(file);
				return -1;
			}
			pb__header__pack(&header, header_buf);

			// 及时腾出内存空间
			size_t i;
			for(i=0; i<header.n_keyframes; i++) {
				if (header.keyframes[i] != NULL) {
					pb__key_frame__free_unpacked(header.keyframes[i], NULL);
					header.keyframes[i] = NULL;
				}
			}
			g_free(header.keyframes);
			header.n_keyframes = 0;
			for (i=0; i<header.n_pageindexs; i++) {
				if (header.pageindexs[i] != NULL) {
					pb__page_index__free_unpacked(header.pageindexs[i], NULL);
					header.pageindexs[i] = NULL;
				}
			}
			g_free(header.pageindexs);
			header.n_pageindexs = 0;

			ret = janus_whiteboard_write_packet_to_file_l(header_buf, header_len, file);
			g_free(header_buf);
		}

		// 从whiteboard->file复制数据
		fseek(whiteboard->file, 0L, SEEK_SET);
		size_t tmp_len = 1024*4;//4k
		void*  tmp_buf = g_malloc0(tmp_len);
		while(1) {
			size_t read_len = fread(tmp_buf, sizeof(unsigned char), tmp_len, whiteboard->file);
			if (read_len <= 0) {
				JANUS_LOG(LOG_INFO, "Whiteboard save all data success.\n");
				break;
			}
			if (janus_whiteboard_write_packet_to_file_l(tmp_buf, read_len, file) < 0) {
				JANUS_LOG(LOG_WARN, "Error happens when saving whiteboard data.\n");
				break;
			}
		}
		g_free(tmp_buf);
		fclose(file);
		file = NULL;
	}

	return 1;
}

static void janus_whiteboard_scene_free(janus_scene *scene) {
	for (int start_index = 0; start_index < scene->page_num; start_index++) {
		if (scene->page_keyframes[start_index] != NULL) {
			g_free(scene->page_keyframes[start_index]);
			scene->page_keyframes[start_index] = NULL;
		}
	}
	g_free(scene->page_keyframes);
	scene->page_keyframes = NULL;
	g_free(scene);
}
/*! 清理内部变量 */
int janus_whiteboard_free(janus_whiteboard *whiteboard) {
	if(!whiteboard)
		return -1;
	janus_whiteboard_close(whiteboard);
	// janus_whiteboard_generate_and_save_l(whiteboard);
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	g_free(whiteboard->dir);
	whiteboard->dir = NULL;
	g_free(whiteboard->filename);
	whiteboard->filename = NULL;
	fclose(whiteboard->scene_file);
	whiteboard->scene_file = NULL;
	fclose(whiteboard->header_file);
	whiteboard->header_file = NULL;
	fclose(whiteboard->page_file);
	whiteboard->page_file = NULL;
	fclose(whiteboard->file);
	whiteboard->file = NULL;
	/*! 清理用于快速定位的关键帧索引 */
	g_hash_table_unref(whiteboard->scenes);
	
	/*! 清理当前场景缓存的白板数据 */
	g_ptr_array_unref(whiteboard->packages);

	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	g_free(whiteboard);
	return 0;
}

janus_whiteboard_result janus_whiteboard_packet_extension(janus_whiteboard *whiteboard, int package_type, char *extension) {
	janus_whiteboard_result result = 
	{
		.ret          = -1, /* 大于0时表示发起者发出了指令，将有数据需要返回. */
		.keyframe_len = 0,
		.keyframe_buf = NULL,
		.command_len  = 0,
		.command_buf  = NULL,
		.package_type = KLPackageType_None,
	};

	if(!whiteboard) {
		JANUS_LOG(LOG_ERR, "Error saving frame. Whiteboard is empty\n");
		return result;
	}

	Pb__Package package;
	pb__package__init(&package);
	package.type = package_type;
	
	package.timestamp = janus_whiteboard_get_current_time_l() - whiteboard->start_timestamp;
	package.extension = g_strdup(extension);
 
	JANUS_LOG(LOG_INFO, "janus_whiteboard_packet_extension: %d, %s\n", package_type, extension);

	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if(!whiteboard->file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_WARN, "Error saving frame. whiteboard->file is empty\n");
		return result;
	}
	package.scene = whiteboard->scene;
	package.page = whiteboard->page;

	result.ret = 0;
	result.command_len = pb__package__get_packed_size(&package);
	result.command_buf = g_malloc0(result.command_len);
	pb__package__pack(&package, result.command_buf);

	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	return result;
}