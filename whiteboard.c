#include "whiteboard.h"
#include <sys/stat.h>
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
int      janus_whiteboard_read_packet_from_file_l(void* dst, size_t len, FILE *src_file);
int      janus_whiteboard_remove_packets_l(Pb__Package** packages, int stat_index, int len);
int      janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard);
uint8_t *janus_whiteboard_packed_data_l(Pb__Package **packages, int len, int *out_len);
int      janus_whiteboard_scene_data_l(janus_whiteboard *whiteboard, int scene, Pb__Package** packages);

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

/*! 由于比较多地方需要 free packets，独立成一个函数比较好管理. 需要对packets长度进行判断防止崩溃 */
int janus_whiteboard_remove_packets_l(Pb__Package** packages, int stat_index, int len) {
	int end_index = stat_index + len;
	for(int index = stat_index; index < end_index; index++) {
		Pb__Package **tmp_package = &packages[index];
		if (*tmp_package) {
			pb__package__free_unpacked(*tmp_package, NULL);
		}
		*tmp_package = NULL;
	}
	return 0;
}

/*! 初始化header
    @returns 正常初始化返回header_len长度（非负数）， 异常返回 -1 */
int janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard) {

	if(!whiteboard || !whiteboard->header_file)
		return -1;

	int header_len = 0;
	fseek(whiteboard->header_file, 0, SEEK_SET);

	// 尝试解析数据到whiteboard->header，如果不成功则执行创建操作
	if(fread(&header_len, sizeof(int), 1, whiteboard->header_file) == 1) {
		char *buffer = g_malloc0(header_len);
		if (janus_whiteboard_read_packet_from_file_l(buffer, header_len, whiteboard->header_file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading header packet from basefile: %s\n", whiteboard->filename);
			g_free(buffer);
			return -1;
		}
		whiteboard->header = pb__header__unpack(NULL, header_len, (const uint8_t *)buffer);
		g_free(buffer);
	}

	if (whiteboard->header)
		return header_len;

	whiteboard->header = g_malloc0(sizeof(Pb__Header));
	memset(whiteboard->header, 0, sizeof(Pb__Header));
	// FIXME: 添加一个变量capacity，代表容量，可能更好
	whiteboard->header->keyframes = g_malloc0(sizeof(Pb__KeyFrame*) * MAX_PACKET_CAPACITY);
	whiteboard->header->n_keyframes = 1;
	whiteboard->header->keyframes[0] = g_malloc0(sizeof(Pb__KeyFrame));
	whiteboard->header->keyframes[0]->offset = 0;
	whiteboard->header->keyframes[0]->timestamp = 0;

	return 0;
}

/*! 创建白板模块，如果创建成功，则尝试从文件里读取历史保留的数据到当前场景(scene) */
janus_whiteboard *janus_whiteboard_create(const char *dir, const char *filename, int scene) {
	/* Create the recorder */
	janus_whiteboard *whiteboard = g_malloc0(sizeof(janus_whiteboard));
	if(whiteboard == NULL) {
		JANUS_LOG(LOG_FATAL, "Out of Memory when alloc memory for struct whiteboard!\n");
		return NULL;
	}
	whiteboard->dir = NULL;
	whiteboard->filename = NULL;
	whiteboard->header_file = NULL;
	whiteboard->file = NULL;
	whiteboard->package_data_offset = 0;
	
	/* Check if this directory exists, and create it if needed */
	if(dir != NULL) {
		struct stat s;
		int err = stat(dir, &s);
		if(err == -1) {
			if(ENOENT == errno) {
				/* Directory does not exist, try creating it */
				if(janus_mkdir(dir, 0755) < 0) {
					JANUS_LOG(LOG_ERR, "mkdir error: %d\n", errno);
					return NULL;
				}
			} else {
				JANUS_LOG(LOG_ERR, "stat error: %d\n", errno);
				return NULL;
			}
		} else {
			if(S_ISDIR(s.st_mode)) {
				/* Directory exists */
				JANUS_LOG(LOG_VERB, "Directory exists: %s\n", dir);
			} else {
				/* File exists but it's not a directory, try creating it */
				if(janus_mkdir(dir, 0755) < 0) {
					JANUS_LOG(LOG_ERR, "mkdir error: %d\n", errno);
					return NULL;
				}
				return NULL;
			}
		}
	}

	const size_t name_length_l = 1024;
	/* generate filename */
	char data_file_name[name_length_l], header_file_name[name_length_l];
	memset(data_file_name,   0, name_length_l);
	memset(header_file_name, 0, name_length_l);
	g_snprintf(data_file_name,   name_length_l, "%s.data", filename);
	g_snprintf(header_file_name, name_length_l, "%s.head", filename);
	/* generate path name */
	char dir_local[name_length_l], data_path[name_length_l], header_path[name_length_l];
	memset(dir_local,   0, name_length_l);
	memset(data_path,   0, name_length_l);
	memset(header_path, 0, name_length_l);
	g_snprintf(dir_local,   name_length_l, "%s", dir == NULL ? "" : dir);
	g_snprintf(data_path,   name_length_l, "%s/%s", dir_local, data_file_name);
	g_snprintf(header_path, name_length_l, "%s/%s", dir_local, header_file_name);

	/* Try opening the data file */
	whiteboard->file = fopen(data_path, "ab+");
	if(whiteboard->file == NULL) {
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", data_path, errno);
		return NULL;
	}
	/* Try opening the header file */
	whiteboard->header_file = fopen(header_path, "ab+");
	if(whiteboard->header_file == NULL) {
		/* avoid memory leak */
		fclose(whiteboard->file);
		JANUS_LOG(LOG_ERR, "fopen %s error: %d\n", header_path, errno);
		return NULL;
	}
	if(dir)
		whiteboard->dir = g_strdup(dir);
	whiteboard->filename = g_strdup(filename);

	janus_mutex_init(&whiteboard->mutex);
    janus_whiteboard_parse_or_create_header_l(whiteboard);

	whiteboard->scene_packages = g_malloc0(sizeof(Pb__Package*) * MAX_PACKET_CAPACITY);
	if (whiteboard->scene_packages) {
	    whiteboard->scene_package_num = janus_whiteboard_scene_data_l(whiteboard, whiteboard->scene, whiteboard->scene_packages);
	} else {
		whiteboard->scene_package_num = 0;
		JANUS_LOG(LOG_ERR, "Out of Memory when alloc memory for %d struct scene_packages!\n", MAX_PACKET_CAPACITY);
	}

	return whiteboard;
}

/*! 将所有的指令集合到一个pkp，再打包为 buffer 二进制数据返回给客户端 */
uint8_t *janus_whiteboard_packed_data_l(Pb__Package **packages, int len, int *out_len) {
	if (packages == NULL || len <= 0 || out_len == NULL) 
		return NULL;

	int total_cmd_num = 0;
	for (int i = 0; i < len; i ++) {
		total_cmd_num += packages[i]->n_cmd;
	}

	Pb__Package out_pkg;
	out_pkg       = *packages[0];
	out_pkg.n_cmd = total_cmd_num;
	out_pkg.cmd   = g_malloc0(sizeof(Pb__Command) * total_cmd_num);

	size_t cmd_index = 0;
	for (int i = 0; i < len; i ++) {
		Pb__Command **cmd = packages[i]->cmd;
		if (packages[i]->n_cmd == 0)
			continue;
		for (size_t j = 0; j < packages[i]->n_cmd; j ++) {
			out_pkg.cmd[cmd_index++] = cmd[j];
		}
	}

	*out_len = pb__package__get_packed_size(&out_pkg);
	uint8_t *out_buf = g_malloc0(*out_len);
	pb__package__pack(&out_pkg, out_buf);

	g_free(out_pkg.cmd);
	return out_buf;
}

/*! 用于返回当前场景的白板数据。由于页面切换时，可能比较多客户端调用获取场景数据接口，
    如果调用 janus_whiteboard_scene_data_l 读取文本文件会造成效率问题。*/
uint8_t *janus_whiteboard_current_scene_data(janus_whiteboard *whiteboard, int *size) {
	uint8_t *out_buf = janus_whiteboard_packed_data_l(whiteboard->scene_packages, whiteboard->scene_package_num, size);
	return out_buf;
}

/*! 从指定的场景获取白板笔迹。先移到当前场景最接近keyframe附近开始查找，需要对clean以及keyframe做额外处理
    @returns 成功获取返回 0， 获取失败返回 -1 */
int janus_whiteboard_scene_data_l(janus_whiteboard *whiteboard, int scene, Pb__Package** packages) {
	if (whiteboard == NULL || whiteboard->file == NULL)
		return -1;

	// seek 到文件开头。FIXME:Rison 使用数组存起来 scene--->offset, 就不需要每次从头开始读取了
	fseek(whiteboard->file, 0, SEEK_SET);
	int pkt_len, out_len = 0;

	while(fread(&pkt_len, sizeof(int), 1, whiteboard->file) != 1) {
		char *buffer = g_malloc0(pkt_len);
		if (janus_whiteboard_read_packet_from_file_l(buffer, pkt_len, whiteboard->file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading scene data packet from basefile: %s\n", whiteboard->filename);
			fseek(whiteboard->file, 0, SEEK_END);
			g_free(buffer);
			return -1;
		}

		Pb__Package *package = pb__package__unpack(NULL, pkt_len, (const uint8_t*)buffer);
		if (package == NULL) {
			JANUS_LOG(LOG_WARN, "Parse whiteboard scene data error.");
			fseek(whiteboard->file, 0, SEEK_END);
			g_free(buffer);
			return -1; //FIXME:Rison: break or continue?
		}

		if (package->scene == scene) {
			if (package->type == KLPackageType_CleanDraw) {
				// 清屏指令，移除已经存在的包.
				janus_whiteboard_remove_packets_l(packages, 0, out_len);
				out_len = 0;
			} else if (package->type == KLPackageType_KeyFrame) {
				// 遇到关键帧，移除已经存在的包.
				janus_whiteboard_remove_packets_l(packages, 0, out_len);
				packages[0] = package;
				out_len = 1;
			} else if (package->type == KLPackageType_SceneData) {
				// 是否过滤掉特殊指令
				packages[out_len] = package;
				out_len ++;
			}
		}

		g_free(buffer);
	}

	fseek(whiteboard->file, 0, SEEK_END);
	return out_len;
}

int janus_whiteboard_add_keyframe(janus_whiteboard *whiteboard) {
	return 0;
}

int janus_whiteboard_save_package(janus_whiteboard *whiteboard, char *buffer, uint16_t length, uint8_t **out) {
	if(!whiteboard)
		return -1;
		JANUS_LOG(LOG_WARN, "Error saving frame. -1\n");
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if(!buffer || length < 1) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_WARN, "Error saving frame. -2\n");
		return -2;
	}
	if(!whiteboard->file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_WARN, "Error saving frame. -3\n");
		return -3;
	}
	Pb__Package *package = pb__package__unpack(NULL, length, buffer);
	if (package == NULL)
	{
		JANUS_LOG(LOG_WARN, "parse whiteboard data error. -4");
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return -4;
	}

	if (package->type == KLPackageType_SwitchScene && package->scene != whiteboard->scene) {
		whiteboard->scene = package->scene;
		for (int i = 0; i < whiteboard->scene_package_num; i ++) {
			pb__package__free_unpacked(whiteboard->scene_packages[i], NULL);
			whiteboard->scene_packages[i] = NULL;
		}
		whiteboard->scene_package_num = janus_whiteboard_scene_data_l(whiteboard, whiteboard->scene, whiteboard->scene_packages);
		if (whiteboard->scene_package_num <= 0)
			JANUS_LOG(LOG_WARN, "save whiteboard package num : %d", whiteboard->scene_package_num);
	} else if (package->type == KLPackageType_SceneData) {
		int size = 0;
		uint8_t *buf;
		if ( package->scene == whiteboard->scene || package->scene == -1 ) {
			*out = janus_whiteboard_packed_data_l(whiteboard->scene_packages, whiteboard->scene_package_num, &size);
		} else if (package->scene >= 0) {
			Pb__Package **packages = g_malloc0(sizeof(Pb__Package*) * 10000);
			int num = janus_whiteboard_scene_data_l(whiteboard, whiteboard->scene, packages);
			*out = janus_whiteboard_packed_data_l(packages, num, &size);
			for (int i = 0; i < num; i ++) {
				pb__package__free_unpacked(packages[i], NULL);
			}
			g_free(packages);
		}
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_WARN, "get frame: %d\n", size);
		return size;
	}

	if (package->type != KLPackageType_SceneData) {
		/* Save packet on file */
		fwrite(&length, sizeof(uint), 1, whiteboard->file);
		int temp = 0, tot = length;
		while(tot > 0) {
			temp = fwrite(buffer+length-tot, sizeof(char), tot, whiteboard->file);
			if(temp <= 0) {
				JANUS_LOG(LOG_WARN, "Error saving frame: -5\n");
				janus_mutex_unlock_nodebug(&whiteboard->mutex);
				return -5;
			}
			tot -= temp;
		}
	}
	/* Done */
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	return 0;
}

int janus_whiteboard_close(janus_whiteboard *whiteboard) {
	if(!whiteboard || !whiteboard->file)
		return -1;
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if(whiteboard->file) {
		fseek(whiteboard->file, 0L, SEEK_END);
		size_t fsize = ftell(whiteboard->file);
		fseek(whiteboard->file, 0L, SEEK_SET);
		JANUS_LOG(LOG_WARN, "File is %zu bytes: %s\n", fsize, whiteboard->filename);
	}
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	return 0;
}

int janus_whiteboard_write_with_header(janus_whiteboard *whiteboard) {
	if(!whiteboard || !whiteboard->file)
		return -1;

	char path[1024];
	char tmp_path[1024];
	memset(path, 0, 1024);
	if (whiteboard->dir != NULL) {
		g_snprintf(path, 1024, "%s/%s", whiteboard->dir, whiteboard->filename);
	} else {
		g_snprintf(path, 1024, "%s", whiteboard->filename);
	}
	g_snprintf(tmp_path, 1024, "%s.tmp", path);//可以去掉
	FILE *file = fopen(path, "wb");

	int header_len = pb__header__get_packed_size(whiteboard->header);
	uint8_t *header_buf = g_malloc0(header_len);
	fwrite(&header_len, sizeof(uint), 1, whiteboard->file);
	int temp = 0, tot = header_len;
	while(tot > 0) {
		temp = fwrite(header_buf+header_len-tot, sizeof(char), tot, whiteboard->file);
		if(temp <= 0) {
			JANUS_LOG(LOG_WARN, "Error saving frame...\n");
			g_free(header_buf);
			fclose(file);
			return -2;
		}
		tot -= temp;
	}
	g_free(header_buf);

	fseek(whiteboard->file, whiteboard->package_data_offset, SEEK_SET);
	int len;
	while(fread(&len, sizeof(int), 1, whiteboard->file) == 1) {
		uint8_t *buf = g_malloc0(len);

		temp = 0, tot = len;
		while(tot > 0) {
			temp = fread(buf+len-tot, sizeof(char), tot, whiteboard->file);
			if(temp <= 0) {
				JANUS_LOG(LOG_WARN, "Error reading frame...\n");
				g_free(buf);
				fclose(file);
				return -3;
			}
			tot -= temp;
		}

		fwrite(&len, sizeof(int), 1, file);
		temp = 0, tot = len;
		while(tot > 0) {
			temp = fwrite(buf+len-tot, sizeof(char), tot, file);
			if(temp <= 0) {
				JANUS_LOG(LOG_WARN, "Error saving frame...\n");
				g_free(buf);
				fclose(file);
				return -4;
			}
			tot -= temp;
		}

		g_free(buf);
	}
	fclose(file);
	fclose(whiteboard->file);
	rename(tmp_path, path);// can be remove
	return 0;
}

int janus_whiteboard_free(janus_whiteboard *whiteboard) {
	if(!whiteboard)
		return -1;
	janus_whiteboard_close(whiteboard);
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	g_free(whiteboard->dir);
	whiteboard->dir = NULL;
	g_free(whiteboard->filename);
	whiteboard->filename = NULL;
	janus_whiteboard_write_with_header(whiteboard);
	// fclose(whiteboard->file);
	whiteboard->file = NULL;
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	g_free(whiteboard);
	return 0;
}