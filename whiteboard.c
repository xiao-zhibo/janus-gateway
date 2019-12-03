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
janus_page *janus_whiteboard_get_page(janus_whiteboard *whiteboard, int scene, int page);
janus_page *janus_whiteboard_set_page(janus_whiteboard *whiteboard, Pb__Page *page_info);
static void janus_whiteboard_scene_free(janus_scene *scene);
janus_whiteboard *janus_whiteboard_create_with_file(const char *dir, const char *filename);



janus_whiteboard *janus_whiteboard_create_with_file(const char *dir, const char *filename) {
	if(!janus_whiteboard_check_diretory(dir)) {
		return NULL;
	}
	if (dir == NULL) {
		dir = "";
	}

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

janus_scene *janus_whiteboard_get_scene(janus_whiteboard *whiteboard, int scene_index) {
	return (janus_scene*)g_hash_table_lookup(whiteboard->scenes, GINT_TO_POINTER(scene_index));
}

janus_page *janus_whiteboard_get_page(janus_whiteboard *whiteboard, int scene, int page) {
	janus_scene *scene_info = janus_whiteboard_get_scene(whiteboard, scene);
	if (scene_info && scene_info->pages && page >= 0 && page < scene_info->page_num) {
		if (scene_info->pages[page] == NULL) {
			janus_page *page_info = g_malloc0(sizeof(janus_page));
			page_info->scene = scene;
			page_info->page = page;
			page_info->scale = 1;
			scene_info->pages[page] = page_info;
		}
		return scene_info->pages[page];
	}
	return NULL;
}

janus_page *janus_whiteboard_set_page(janus_whiteboard *whiteboard, Pb__Page *page_info) {
	janus_scene *scene_info = janus_whiteboard_get_scene(whiteboard, page_info->scene);
	if (scene_info && scene_info->pages && page_info->page >= 0 && page_info->page < scene_info->page_num) {
		janus_page *page = scene_info->pages[page_info->page];
		if (page == NULL) {
			page = g_malloc0(sizeof(janus_page));
			if (!page) {
				return NULL;
			}
			scene_info->pages[page_info->page] = page;
		}
		page->scene 	= page_info->scene;
		page->page 	= page_info->page;
		page->angle 	= page_info->angle;
		page->scale 	= page_info->scale;
		page->move_x = page_info->move_x;
		page->move_y = page_info->move_y;
		return page;
	}
	return NULL;
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

	// init scene data from scene file
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
			JANUS_LOG(LOG_WARN, "parse whiteboard scene data error.\n");
			fseek(whiteboard->scene_file, 0, SEEK_END);
			g_free(buffer);
			return -1;
		}

		janus_scene *j_scene 					= g_malloc0(sizeof(janus_scene));
		j_scene->index 							= tmp_scene->index;
		j_scene->source_url 					= g_strdup(tmp_scene->resource);
		j_scene->page_num 						= tmp_scene->pagecount;
		j_scene->pages 							= g_malloc0(sizeof(janus_page*) * j_scene->page_num);
		if (tmp_scene->resourceid != NULL) {
			j_scene->source_id = g_strdup(tmp_scene->resourceid);
		} else {
			j_scene->source_id = NULL;
		}

		g_hash_table_insert(whiteboard->scenes, GINT_TO_POINTER(tmp_scene->index), j_scene);
		JANUS_LOG(LOG_INFO, "whiteboard: janus scene: %d, %s, %d\n", tmp_scene->index, j_scene->source_url, j_scene->page_num);

		pb__scene__free_unpacked(tmp_scene, NULL);
		g_free(buffer);
		buffer = NULL;
	}
	fseek(whiteboard->scene_file, 0, SEEK_END);

	//init page data from page file
	fseek(whiteboard->page_file, 0, SEEK_SET);
	JANUS_LOG(LOG_WARN, "page file seek: %d\n", ferror(whiteboard->page_file));
	while (janus_whiteboard_read_packet_from_file(&buffer, &pkt_len, whiteboard->page_file) > 0) {
		if (buffer == NULL) {
			JANUS_LOG(LOG_WARN, "parse whiteboard pgae package error: %s\n", whiteboard->filename);
			continue;
		}
		Pb__Page *tmp_page = pb__page__unpack(NULL, pkt_len, (const uint8_t*)buffer);
		if (tmp_page == NULL) {
			JANUS_LOG(LOG_WARN, "parse whiteboard page data error.\n");
			fseek(whiteboard->page_file, 0, SEEK_END);
			g_free(buffer);
			return -1;
		}

		janus_scene *scene_data = janus_whiteboard_get_scene(whiteboard, tmp_page->scene);
		if (scene_data == NULL) {
			JANUS_LOG(LOG_WARN, "whiteboard: page's scene data error.\n");
			fseek(whiteboard->page_file, 0, SEEK_END);
			g_free(buffer);
			return -1;
		}
		janus_page *page_info = (tmp_page->page >= 0) ? scene_data->pages[tmp_page->page] : NULL;
		if (page_info == NULL) {
			page_info = g_malloc0(sizeof(janus_page));
			if (page_info == NULL) {
				JANUS_LOG(LOG_WARN, "whiteboard: init page data error.\n");
				fseek(whiteboard->page_file, 0, SEEK_END);
				g_free(buffer);
				return -1;
			}
			scene_data->pages[tmp_page->page] = page_info;
		}

		page_info->scene = tmp_page->scene;
		page_info->page = tmp_page->page;
		page_info->angle = tmp_page->angle;
		page_info->scale = tmp_page->scale;
		page_info->move_x = tmp_page->move_x;
		page_info->move_y = tmp_page->move_y;
		whiteboard->cur_page = *page_info;

		pb__page__free_unpacked(tmp_page, NULL);
		g_free(buffer);
		buffer = NULL;
	}
	fseek(whiteboard->page_file, 0, SEEK_END);
	JANUS_LOG(LOG_INFO, "whiteboard: --->init scene page data from file success!\n");
	return 0;
}

/*! 初始化header
    @returns 正常初始化返回header_len长度（非负数）， 异常返回 -1 */
int janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard) {

	if(!whiteboard || !whiteboard->header_file) {
		return -1;
	}

	if (janus_whiteboard_init_scene_from_file_l(whiteboard) != 0) {
		JANUS_LOG(LOG_WARN, "init whiteboard scene data error.\n");
		return -1;
	}

	JANUS_LOG(LOG_INFO, "parse header cur_page(scene, page): (%d, %d)\n", whiteboard->cur_page.scene, whiteboard->cur_page.page);
	if (whiteboard->cur_page.scale == 0 &&
		whiteboard->cur_page.scene == 0 &&
		whiteboard->cur_page.page == 0) {
		whiteboard->cur_page.scene = 0;
		whiteboard->cur_page.page = 0;
		whiteboard->cur_page.angle = 0;
		whiteboard->cur_page.scale = 1;
		whiteboard->cur_page.move_x = 0;
		whiteboard->cur_page.move_y = 0;
		whiteboard->cur_page.key_frame = NULL;
	}

	int ret = 0;
	ret = fseek(whiteboard->header_file, 0, SEEK_SET);
	if (ret < 0) {
		JANUS_LOG(LOG_ERR, "seek header file error.\n");
		return -1;
	}

	// 尝试解析数据到 page的key_frame中，如果不成功则执行创建操作
	size_t keyframe_len   = 0;
	char *keyframe_buffer = NULL;
	while(janus_whiteboard_read_packet_from_file(&keyframe_buffer, &keyframe_len, whiteboard->header_file) > 0) {
		if (keyframe_buffer == NULL) {
			continue;
		}
		Pb__KeyFrame *tmp_keyframe = pb__key_frame__unpack(NULL, keyframe_len, (const uint8_t*)keyframe_buffer);
		if (tmp_keyframe != NULL) {
			janus_page *page_info = janus_whiteboard_get_page(whiteboard, tmp_keyframe->scene, tmp_keyframe->page);
			if (page_info == NULL) {
				JANUS_LOG(LOG_ERR, "key frame page info is null.\n");
				g_free(keyframe_buffer);
				keyframe_buffer = NULL;
				pb__key_frame__free_unpacked(tmp_keyframe, NULL);
				continue;
			}

			if (page_info->key_frame != NULL) {
				pb__key_frame__free_unpacked(page_info->key_frame, NULL);
				page_info->key_frame = NULL;
			}
			page_info->key_frame = tmp_keyframe;
		}
		g_free(keyframe_buffer);
		keyframe_buffer = NULL;
	}

	fseek(whiteboard->file, 0, SEEK_END);
	fseek(whiteboard->scene_file, 0, SEEK_END);
	fseek(whiteboard->page_file, 0, SEEK_END);
	JANUS_LOG(LOG_INFO, "whiteboard: --->Parse or create header success:(%d, %d)\n", whiteboard->cur_page.scene, whiteboard->cur_page.page);

	return 0;
}

/*! 创建白板模块，如果创建成功，则尝试从文件里读取历史保留的数据到当前场景(scene) */
janus_whiteboard *janus_whiteboard_create(const char *local_dir, const char *filename) {
	/* Create the recorder */
	janus_whiteboard *whiteboard = janus_whiteboard_create_with_file(local_dir, filename);
	if(whiteboard == NULL) {
		JANUS_LOG(LOG_FATAL, "Out of Memory when init whiteboard!\n");
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
	janus_whiteboard_scene_page_data_l(whiteboard, whiteboard->cur_page.scene, whiteboard->cur_page.page, whiteboard->packages);

	return whiteboard;
}

/*! 本地函数，处理添加新场景 */
int janus_whiteboard_add_scene_l(janus_whiteboard *whiteboard, Pb__Scene *newScene) {
	janus_scene *j_scene = g_malloc0(sizeof(janus_scene));
	j_scene->type        = newScene->type; 
	j_scene->source_url  = g_strdup(newScene->resource);
	j_scene->page_num    = newScene->pagecount;
	j_scene->pages       = g_malloc0(sizeof(Pb__Page*) * j_scene->page_num);
	// j_scene->page_keyframes = g_malloc0(sizeof(Pb__KeyFrame*) * j_scene->page_num);
	// j_scene->page_keyframe_maxnum = 0;
	if (newScene->resourceid != NULL) {
		j_scene->source_id = g_strdup(newScene->resourceid);
	} else {
		j_scene->source_id = NULL;
	}

	int scene_num = g_hash_table_size(whiteboard->scenes);
	if (newScene->index == -1) {
		newScene->index = scene_num;
	}
	j_scene->index = newScene->index;
	
	if (newScene->index > scene_num) {
		JANUS_LOG(LOG_INFO, "whiteboard: janus_whiteboard_add_scene_l: error input %d, expect %d\n", newScene->index, scene_num);
		janus_whiteboard_scene_free(j_scene);
		return -1;
	} else {
		janus_scene *tmp_scene = janus_whiteboard_get_scene(whiteboard, newScene->index);
		if (tmp_scene) {
			JANUS_LOG(LOG_INFO, "whiteboard: source_url: %s, %s\n", tmp_scene->source_url, j_scene->source_url);
			if (strcmp(tmp_scene->source_url, j_scene->source_url) == 0) {
				janus_whiteboard_scene_free(j_scene);
				return -1;
			}
		}
		g_hash_table_insert(whiteboard->scenes, GINT_TO_POINTER(newScene->index), j_scene);
	}
	
	/*! 将 keyframe 保存到文件 */
	size_t length = pb__scene__get_packed_size(newScene);
	if (length == 0) {
		JANUS_LOG(LOG_WARN, "save scene data fail. unable to get packed size of new scene\n");
		return -1;
	}
	void *buffer = g_malloc0(length);
	if (buffer == NULL) {
		JANUS_LOG(LOG_WARN, "save scene data fail. out of memory when allocating memory for size of %zu\n", length);
		return -1;
	}

	pb__scene__pack(newScene, buffer);
	fseek(whiteboard->scene_file, 0, SEEK_END);
	size_t ret = fwrite(&length, sizeof(size_t), 1, whiteboard->scene_file);
	if (ret == 1) {
	    ret = janus_whiteboard_write_packet_to_file_l(buffer, length, whiteboard->scene_file);
	    ret = (ret==0) ? 1 : 0;//由于以上函数封装的关系，此处需要对返回的结果处理下 
	}
	g_free(buffer);
	if (ret == 0) {
		JANUS_LOG(LOG_ERR, "error happens when saving scene packet to basefile: %s\n", whiteboard->filename);
		return -1;
	}
	return 1;
}

/*! 公开函数，处理添加新场景。注意，此函数不能在内部直接调用，因为有锁 */
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

	if (!whiteboard) {
		JANUS_LOG(LOG_ERR, "add scene error, whiteboard is empty\n");
		return result;
	}
	if (page_count <= 0) {
		JANUS_LOG(LOG_WARN, "add scene error, scene page count <= 0, invalid parameter.\n");
		return result;
	}

	// 构造一个局部的白板包，存放场景数据，稍后打包返回
	Pb__Package package;
	pb__package__init(&package);
	package.type      = package_type;
	package.timestamp = janus_whiteboard_get_current_time_l();
	package.newscene  = g_malloc0(sizeof(Pb__Scene));
	if (!package.newscene) {
		JANUS_LOG(LOG_WARN, "add scene error, can not malloc Pb__Scene\n");
		return result;
	}
	pb__scene__init(package.newscene);
	package.newscene->type       = type;
	package.newscene->index      = index;
	package.newscene->pagecount  = page_count;
	package.newscene->resource   = g_strdup(resource);
	package.newscene->resourceid = g_strdup(resource_id);

	// 上锁，因为需要调用本地函数保存添加指令到 scene_file 内
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if (!whiteboard->scene_file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		g_free(package.newscene->resource);
		g_free(package.newscene->resourceid);
		g_free(package.newscene);
		JANUS_LOG(LOG_WARN, "add scene error, whiteboard->scene_file is empty\n");
		return result;
	}
	result.ret = janus_whiteboard_add_scene_l(whiteboard, package.newscene);
	janus_mutex_unlock_nodebug(&whiteboard->mutex);

	if (result.ret <= 0) {
		g_free(package.newscene->resource);
		g_free(package.newscene->resourceid);
		g_free(package.newscene);
		JANUS_LOG(LOG_WARN, "add scene error, something wrong happend in janus_whiteboard_add_scene_l\n");
		return result;
	}

	// 打包数据返回
	JANUS_LOG(LOG_INFO, "add scene success: %s, %d, %d\n", package.newscene->resource, package.newscene->pagecount, package.newscene->index);
	result.ret          = package.newscene->index;
	result.package_type = KLPackageType_AddScene;
	result.command_len  = pb__package__get_packed_size(&package);
	result.command_buf  = g_malloc0(result.command_len);
	pb__package__pack(&package, result.command_buf);

	// 清理临时数据
	g_free(package.newscene->resource);
	g_free(package.newscene->resourceid);
	g_free(package.newscene);
	return result;
}

/*! 将所有的指令集合到一个pkp，再打包为 buffer 二进制数据返回给客户端
    len=0 的时候继续打包，返回一个清屏指令 */
void janus_whiteboard_packed_data_l(GPtrArray *packages, janus_whiteboard_result *result) {
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
		if (package->page_info != NULL) {
			out_pkg.page_info = package->page_info;
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

	janus_page *page_data = janus_whiteboard_get_page(whiteboard, scene, page);
	if (page_data == NULL) {
		JANUS_LOG(LOG_WARN, "whiteboard: scene_page_data_l: invalid scene(%d) or page(%d)", scene, page);
		return -1;
	}

	int package_data_offset = 0;
	if (page_data->key_frame != NULL) {
		package_data_offset = page_data->key_frame->offset;
		JANUS_LOG(LOG_VERB, "Get scene page data offset %d\n", package_data_offset);
	}

	// seek 到文件开头。FIXME:Rison 使用数组存起来 scene--->offset, 就不需要每次从头开始读取了
	fseek(whiteboard->file, package_data_offset, SEEK_SET);
	size_t pkt_len, out_len = 0;
	char *buffer = NULL;
	while(janus_whiteboard_read_packet_from_file(&buffer, &pkt_len, whiteboard->file)) {
		if(buffer == NULL) {
			JANUS_LOG(LOG_VERB, "janus_whiteboard_read_packet_from_file error %zu\n", pkt_len);
			fseek(whiteboard->file, 0, SEEK_END);
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
	if (scene_data && page >= 0) {
		janus_page *page_data = scene_data->pages[page];
		if (page_data && page_data->key_frame) {
			return 1;
		}
	}
	return 0;
}

int janus_whiteboard_on_receive_keyframe_l(janus_whiteboard *whiteboard, Pb__Package *package) {
	if (!whiteboard->header_file || !package)
		return -1;

	janus_page *page_data = janus_whiteboard_get_page(whiteboard, package->scene, package->page);
	if (!page_data) {
		JANUS_LOG(LOG_WARN, "whiteboard: key_frame package match page invalid!\n");
		return -1;
	}

	Pb__KeyFrame *key_frame = g_malloc0(sizeof(Pb__KeyFrame));
	if (key_frame == NULL) {
		JANUS_LOG(LOG_WARN, "Save keyframe fail. Out of memory when allocating memory for new frame\n");
		return -1;
	}
	pb__key_frame__init(key_frame);
	fseek(whiteboard->file, 0, SEEK_END);
	key_frame->offset    = ftell(whiteboard->file);
	key_frame->scene     = package->scene;
	key_frame->page 		= package->page;
	key_frame->timestamp = package->timestamp;

	if (page_data->key_frame != NULL) {
		g_free(page_data->key_frame);
	}
	page_data->key_frame = key_frame;

	/*! 将 keyframe 保存到文件 */
	size_t length = pb__key_frame__get_packed_size(key_frame);
	void *buffer = g_malloc0(length);
	if (length!= 0 && buffer == NULL) {
		JANUS_LOG(LOG_WARN, "Save keyframe fail. Out of memory when allocating memory for tmp file buffer\n");
		return -1;
	}

	pb__key_frame__pack(key_frame, buffer);
	fseek(whiteboard->header_file, 0, SEEK_END);
	size_t ret = fwrite(&length, sizeof(size_t), 1, whiteboard->header_file);
	if (ret == 1) {
	    ret = janus_whiteboard_write_packet_to_file_l(buffer, length, whiteboard->header_file);
	    ret = (ret==0) ? 1 : 0;//由于以上函数封装的关系，此处需要对返回的结果处理下 
	}
	g_free(buffer);

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

	Pb__Page nextPage;
	pb__page__init(&nextPage);
	if (package->page_info != NULL){
		nextPage = *(package->page_info);
	} else {
		janus_page *target_page = janus_whiteboard_get_page(whiteboard, package->scene, package->page);
		nextPage.scene     = package->scene;
		nextPage.page 	   = package->page;
		nextPage.timestamp = package->timestamp;
		nextPage.angle = target_page->angle;
		nextPage.scale = target_page->scale;
		nextPage.move_x = target_page->move_x;
		nextPage.move_y = target_page->move_y;
	}

	size_t length = pb__page__get_packed_size(&nextPage);
	void *buffer = g_malloc0(length);
	if (length != 0 && buffer == NULL) {
		JANUS_LOG(LOG_ERR, "Out of memory when allocating memory for tmp switch scene index buffer\n");
		return -1;
	}
	pb__page__pack(&nextPage, buffer);
	size_t ret = fwrite(&length, sizeof(length), 1, whiteboard->page_file);
	if (ret == 1) {
		ret = janus_whiteboard_write_packet_to_file_l(buffer, length, whiteboard->page_file);
		ret = (ret==0) ? 1 : 0;
	}
	g_free(buffer);

	if (ret == 0) {
		JANUS_LOG(LOG_ERR, "Error happens when saving switch scene index to basefile: %s\n", whiteboard->filename);
		return -1;
	}
	return 0;
}

/*! 核心函数
    本函数尝试以白板数据进行解包，对 场景切换，获取场景数据进行额外处理。其余情况正常保存。
    //FIXME:Rison 使用消息队列异步处理，因为使用锁，会降低服务器性能
    @returns 保存成功返回非负数。如果有数据返回则表示返回数据的长度，*/
janus_whiteboard_result janus_whiteboard_save_package(janus_whiteboard *whiteboard, char *buffer, size_t length) {
	janus_whiteboard_result result = {
		.ret          = -1, /* 大于0时表示发起者发出了指令，将有数据需要返回. */
		.keyframe_len = 0,
		.keyframe_buf = NULL,
		.command_len  = 0,
		.command_buf  = NULL,
		.package_type = KLPackageType_None,
	};

	if (!whiteboard) {
		JANUS_LOG(LOG_ERR, "Error saving frame. Whiteboard is empty\n");
		return result;
	}
	if (!buffer || length <= 0) {
		JANUS_LOG(LOG_WARN, "Error saving frame. Invalid params\n");
		return result;
	}

	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if (!whiteboard->file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_WARN, "Error saving frame. whiteboard->file is empty\n");
		return result;
	}

	Pb__Package *package = pb__package__unpack(NULL, length, (const uint8_t *)buffer);
	if (package == NULL) {
		JANUS_LOG(LOG_WARN, "Error saving frame. Invalid whiteboard packet\n");
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return result;
	}

	JANUS_LOG(LOG_INFO, "whiteboard: package type(%d), scene(%d), page(%d)\n", package->type, package->scene, package->page);
	if (package->type == KLPackageType_None ||
	 package->type == KLPackageType_KeyFrame || 
	 package->type == KLPackageType_EnableUserDraw ||
	 package->type == KLPackageType_DeleteScene ||
	 package->type == KLPackageType_ModifyScene ||
	 package->type == KLPackageType_SceneOrderChange) {
		JANUS_LOG(LOG_WARN, "whiteboard: un-support package type, return now\n");
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return result;
	 }

	package->timestamp = janus_whiteboard_get_current_time_l();
	janus_page *cur_page = &whiteboard->cur_page;
	if (package->type == KLPackageType_AddScene) {
		// 添加一个场景，包含 资源ID，页码数，索引
		result.ret = janus_whiteboard_add_scene_l(whiteboard, package->newscene);
		JANUS_LOG(LOG_INFO, "whiteboard: create a newscene: %s, %d, %d\n", package->newscene->resource, package->newscene->pagecount, package->newscene->index);
		result.command_len = pb__package__get_packed_size(package);
		result.command_buf = g_malloc0(result.command_len);
		pb__package__pack(package, result.command_buf);
		result.package_type = KLPackageType_AddScene;
		pb__package__free_unpacked(package, NULL);
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return result;

	} else if (package->type == KLPackageType_SceneData) {
		// 获取当前在在哪个场景和场景内的第几页
		int scene_num = g_hash_table_size(whiteboard->scenes);
		result.ret = 1;
		if (scene_num > 0) {
			JANUS_LOG(LOG_INFO, "whiteboard: KLPackageType_SceneData\n");
			Pb__Package out_package = *package;
			out_package.page   = whiteboard->cur_page.page;
			out_package.scene  = whiteboard->cur_page.scene;
			result.command_len = pb__package__get_packed_size(&out_package);
			JANUS_LOG(LOG_INFO, "whiteboard: packed_size %d\n", result.command_len);
			result.command_buf = g_malloc0(result.command_len);
			int size = pb__package__pack(&out_package, result.command_buf);
			JANUS_LOG(LOG_INFO, "whiteboard: command_buf_size %d\n", size);
		}
		else {
			result.ret = -1;
		}
		result.package_type = KLPackageType_SceneData;
		pb__package__free_unpacked(package, NULL);
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return result;

	} else if (package->type == KLPackageType_SwitchScenePage) {
		// 切换白板场景
		if (package->scene == cur_page->scene && package->page == cur_page->page) {
			JANUS_LOG(LOG_WARN, "Got a request to switch scene page, but currenttly the whiteboard is on the target %d scene %d page\n", package->scene, package->page);
			result.ret = 0;
			pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
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
		int ret = janus_whiteboard_scene_page_data_l(whiteboard, package->scene, package->page, whiteboard->packages);
		if (ret < 0) {
			JANUS_LOG(LOG_WARN, "Something wrong happens when fetching scene data with reselt %d\n", ret);
		}
		// 这里不立即返回，是因为需要将此命令数据保存
		whiteboard->cur_page = *janus_whiteboard_get_page(whiteboard, package->scene, package->page);

	} else if (package->type == KLPackageType_PageChange) {
		// 页面信息发生更改，如旋转，缩放等
		if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a request to page change, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
			result.ret = -1;
			pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
		if (package->page_info == NULL) {
			JANUS_LOG(LOG_WARN, "Got a request to page change, page info is invalid.\n");
			result.ret = -1;
			pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}

		package->page_info->scene = package->scene;
		package->page_info->page  = package->page;
		janus_whiteboard_on_receive_switch_scene_l(whiteboard, package);
		janus_page *page_info = janus_whiteboard_set_page(whiteboard, package->page_info);
		if (whiteboard->cur_page.scene == page_info->scene && whiteboard->cur_page.page == page_info->page) {
			whiteboard->cur_page = *page_info;
		}

	} else if(package->type == KLPackageType_CleanDraw) {
	    // 清屏
		if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a request get scene page, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
		    result.ret = -1;
		    pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
		janus_scene *scene = janus_whiteboard_get_scene(whiteboard, package->scene);
		if (cur_page->scene == package->scene && cur_page->page == package->page) {
			g_ptr_array_set_size(whiteboard->packages, 0);
			JANUS_LOG(LOG_INFO, "Got a clear screen command, clear local cache data now\n");
		}

	} else if (package->type == KLPackageType_ScenePageData) {
		// 请求指定场景的白板数据
		if (package->scene < 0 || package->page < 0) {
			// -1的情况表示请求当前场景
			package->scene = cur_page->scene;
			package->page = cur_page->page;
		}
		if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a request get scene page, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
			result.ret = -1;
			pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
		JANUS_LOG(LOG_VERB, "Got scene page data(%d, %d)/(%d, %d)\n", package->scene, package->page, cur_page->scene, cur_page->page);
		if (package->scene == cur_page->scene && package->page == cur_page->page) {
			janus_whiteboard_packed_data_l(whiteboard->packages, &result);
		} else {
			// TODO: Rison 改成直接报错。目前的情况是 其他场景，只获取，不切换
			GPtrArray *packages = g_ptr_array_new_full(BASE_PACKET_CAPACITY, (GDestroyNotify)janus_whiteboard_package_free);
			int num = janus_whiteboard_scene_page_data_l(whiteboard, package->scene, package->page, packages);
			janus_whiteboard_packed_data_l(packages, &result);
			g_ptr_array_unref(packages);
		}
		JANUS_LOG(LOG_VERB, "Got scene data with keyframe:%d, command:%d\n", result.keyframe_len, result.command_len);
		pb__package__free_unpacked(package, NULL);
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		result.ret = 1;
		return result;
	}

	// 关键帧保存
	if (package->type == KLPackageType_KeyFrame || package->type == KLPackageType_CleanDraw) {
		// 关键帧标志和清屏动作，可以认为是一个标准的关键帧
		if (!janus_whiteboard_package_check(whiteboard, package)) {
			JANUS_LOG(LOG_WARN, "Got a key frame or clean package, but its scene(%d) or page(%d) index is invalid.\n", package->scene, package->page);
			result.ret = -1;
			pb__package__free_unpacked(package, NULL);
			janus_mutex_unlock_nodebug(&whiteboard->mutex);
			return result;
		}
		janus_whiteboard_on_receive_keyframe_l(whiteboard, package);

	} else if (package->type == KLPackageType_SwitchScenePage) {
		// 切换到新页面或者新场景的数据为0也可以认为是关键帧
		if (whiteboard->packages->len == 0) {
			janus_whiteboard_on_receive_keyframe_l(whiteboard, package);
		}

	} else if (package->page >= 0 && !janus_whiteboard_have_keyframe_l(whiteboard, package->scene, package->page)) {
		// 修复第一个包不是关键帧的问题
		janus_whiteboard_on_receive_keyframe_l(whiteboard, package);
	}

	// 打印非法数据，便与跟踪调试
	if (package->page < 0) {
		JANUS_LOG(LOG_WARN, "Got an un-expect page, page type %d, scene %d, page %d\n", package->type, package->scene, package->page);
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
	
	// 保存数据到当前场景（内存），以便快速处理 KLPackageType_ScenePageData 指令
	if (package->scene == cur_page->scene && package->page == cur_page->page) {
		janus_whiteboard_add_pkt_to_packages_l(whiteboard->packages, package);
	} else {
		pb__package__free_unpacked(package, NULL);
	}

	result.ret = 0;
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
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

	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	return 0;
}

static void janus_whiteboard_scene_free(janus_scene *scene) {
	for (int index = 0; index < scene->page_num; index++) {
		janus_page *page = scene->pages[index];
		if (page != NULL) {
			if (page->key_frame){
				g_free(page->key_frame);
			}
			scene->pages[index] = NULL;
		}
	}
	g_free(scene->pages);
	scene->pages = NULL;

	if (scene->source_id != NULL) {
		g_free(scene->source_id);
	}
	if (scene->source_url != NULL) {
		g_free(scene->source_url);
	}
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
	package.scene = whiteboard->cur_page.scene;
	package.page = whiteboard->cur_page.page;

	result.ret = 0;
	result.command_len = pb__package__get_packed_size(&package);
	result.command_buf = g_malloc0(result.command_len);
	pb__package__pack(&package, result.command_buf);

	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	g_free(package.extension);
	return result;
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
	header.n_pages = 0;
	header.pages   = g_malloc0(sizeof(Pb__Page*) * MAX_PACKET_CAPACITY);
	if (header.pages == NULL) {
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
		Pb__Page *tmp_switchscene = pb__page__unpack(NULL, switchscene_len, (const uint8_t*)buffer);
		if (tmp_switchscene != NULL) {
			header.pages[header.n_pages] = tmp_switchscene;
			header.n_pages ++;
			if (header.n_pages >= MAX_PACKET_CAPACITY) {
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
			for (i=0; i<header.n_pages; i++) {
				if (header.pages[i] != NULL) {
					pb__page__free_unpacked(header.pages[i], NULL);
					header.pages[i] = NULL;
				}
			}
			g_free(header.pages);
			header.n_pages = 0;

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
