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
int      janus_whiteboard_write_packet_to_file_l(void* src, size_t len, FILE *dst_file);
int      janus_whiteboard_remove_packets_l(Pb__Package** packages, int stat_index, int len);
int      janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard);
uint8_t *janus_whiteboard_packed_data_l(Pb__Package **packages, int len, int *out_len);
int      janus_whiteboard_scene_data_l(janus_whiteboard *whiteboard, int scene, Pb__Package** packages);
int      janus_whiteboard_on_receive_keyframe_l(janus_whiteboard *whiteboard, Pb__Package *package);

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

/*! 写入len字节到目标文件 
    @returns 正常写入返回0， 异常返回 -1 */
int janus_whiteboard_write_packet_to_file_l(void* src, size_t len, FILE *dst_file) {
	size_t ret = 0, total = len;
	while (total > 0) {
		ret = fwrite(src + (len-total), sizeof(unsigned char), total, dst_file);
		if (ret >= total) {
			return 0;
		} else if (ret <= 0) {
			JANUS_LOG(LOG_ERR, "Error saving packet...\n");//应该表明写入了多少
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

	Pb__Package out_pkg = *packages[0];
	out_pkg.type  = KLPackageType_DrawCommand;
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
    @returns 成功获取返回 pkt 的数目， 获取失败返回 -1 */
int janus_whiteboard_scene_data_l(janus_whiteboard *whiteboard, int scene, Pb__Package** packages) {
	if (whiteboard == NULL || whiteboard->file == NULL)
		return -1;

	// seek 到文件开头。FIXME:Rison 使用数组存起来 scene--->offset, 就不需要每次从头开始读取了
	fseek(whiteboard->file, 0, SEEK_SET);
	size_t pkt_len, out_len = 0;

	while(fread(&pkt_len, sizeof(size_t), 1, whiteboard->file) == 1) {
		char *buffer = g_malloc0(pkt_len);
		if (janus_whiteboard_read_packet_from_file_l(buffer, pkt_len, whiteboard->file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading scene data packet from basefile: %s\n", whiteboard->filename);
			fseek(whiteboard->file, 0, SEEK_END);
			g_free(buffer);
			return -1;
		}

		Pb__Package *package = pb__package__unpack(NULL, pkt_len, (const uint8_t*)buffer);
		if (package == NULL) {
			JANUS_LOG(LOG_WARN, "Parse whiteboard scene data error.\n");
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
			} else if (package->type != KLPackageType_SceneData) {
				// 过滤掉特殊指令
				packages[out_len] = package;
				out_len ++;
			}
		}

		g_free(buffer);
	}

	fseek(whiteboard->file, 0, SEEK_END);
	return out_len;
}

int janus_whiteboard_on_receive_keyframe_l(janus_whiteboard *whiteboard, Pb__Package *package) {
	if (!whiteboard->file)
		return -1;

	if (whiteboard->header->n_keyframes >= MAX_PACKET_CAPACITY) {
		JANUS_LOG(LOG_WARN, "Save keyframe fail. MAX_PACKET_CAPACITY is %d\n", MAX_PACKET_CAPACITY);
		return -1;
	}

	Pb__KeyFrame *next = g_malloc0(sizeof(Pb__KeyFrame));
	if (next == NULL) {
		JANUS_LOG(LOG_WARN, "Save keyframe fail. Out of memory when allocating memory for new frame\n");
		return -1;
	}
	fseek(whiteboard->file, 0, SEEK_END);
	next->offset    = ftell(whiteboard->file);
	next->timestamp = package->timestamp;
	whiteboard->header->keyframes[whiteboard->header->n_keyframes] = next;
	whiteboard->header->n_keyframes ++;

	// FIXME:Rison 定期保存头部，考虑关键帧的情况，以便加速查找？
	return 0;
}

/*! 核心函数
    本函数尝试以白板数据进行解包，对 场景切换，获取场景数据进行额外处理。其余情况正常保存。
    //FIXME:Rison 是否需要保存场景切换这些包，因为回访也需要用到?
    @returns 保存成功返回非负数。如果有数据返回则表示返回数据的长度，*/
int janus_whiteboard_save_package(janus_whiteboard *whiteboard, char *buffer, size_t length, uint8_t **out) {
	if(!whiteboard) {
		JANUS_LOG(LOG_ERR, "Error saving frame. Whiteboard is empty\n");
		return -1;
	}
	if(!buffer || length < 1 || !out) {
		JANUS_LOG(LOG_WARN, "Error saving frame. Invalid params\n");
		return -1;
	}

	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if(!whiteboard->file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_WARN, "Error saving frame. whiteboard->file is empty\n");
		return -1;
	}

	Pb__Package *package = pb__package__unpack(NULL, length, (const uint8_t*)buffer);
	if (package == NULL) {
		JANUS_LOG(LOG_WARN, "Error saving frame. Invalid whiteboard packet\n");
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return -1;
	}

	if (package->type == KLPackageType_SwitchScene && package->scene != whiteboard->scene) {
	// 切换白板场景
		if (package->scene == whiteboard->scene) {
			JANUS_LOG(LOG_WARN, "Get a request to switch scene, but currenttly the whiteboard is on the target %d scene\n", package->scene);
			return 0;
		}
		whiteboard->scene = package->scene;
		janus_whiteboard_remove_packets_l(whiteboard->scene_packages, 0, whiteboard->scene_package_num);
		whiteboard->scene_package_num = janus_whiteboard_scene_data_l(whiteboard, whiteboard->scene, whiteboard->scene_packages);
		if (whiteboard->scene_package_num < 0) {
			JANUS_LOG(LOG_WARN, "Something wrong happens when fetching scene data with reselt %d\n", whiteboard->scene_package_num);
			whiteboard->scene_package_num = 0;
		}
	} else if (package->type == KLPackageType_SceneData) {
	// 请求指定场景的白板数据
		int out_size = 0;
		if ( package->scene == whiteboard->scene || package->scene == -1 ) {
			// 当前场景
			*out = janus_whiteboard_packed_data_l(whiteboard->scene_packages, whiteboard->scene_package_num, &out_size);
		} else if (package->scene >= 0) {
			// 其他场景
			Pb__Package **packages = g_malloc0(sizeof(Pb__Package*) * MAX_PACKET_CAPACITY);
			int num = janus_whiteboard_scene_data_l(whiteboard, package->scene, packages);
			*out = janus_whiteboard_packed_data_l(packages, num, &out_size);
			for (int i = 0; i < num; i ++) {
				pb__package__free_unpacked(packages[i], NULL);
			}
			g_free(packages);
		}
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_VERB, "Get scene data with length: %d\n", out_size);
		return out_size;
	}

	//if (package->type != KLPackageType_SceneData)// 此处无需考虑非scene data的情况，因为这种情况已在前面处理并返回

	if (package->type == KLPackageType_KeyFrame || package->type == KLPackageType_CleanDraw) {
	// 额外处理关键帧
		janus_whiteboard_on_receive_keyframe_l(whiteboard, package);
	}

	// 写入到文件记录保存
	fseek(whiteboard->file, 0, SEEK_END);
	size_t ret = fwrite(&length, sizeof(size_t), 1, whiteboard->file);
	if (ret == 1) {
	    ret = janus_whiteboard_write_packet_to_file_l((void*)buffer, length, whiteboard->file);
	    ret = (ret==0) ? 1 : 0;//由于以上函数封装的关系，此处需要对返回的结果处理下 
	}
	if (ret == 0) {
		JANUS_LOG(LOG_ERR, "Error happens when saving scene data packet to basefile: %s\n", whiteboard->filename);
	}
	
	pb__package__free_unpacked(package, NULL);
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
		JANUS_LOG(LOG_WARN, "File is %zu bytes: %s\n", fsize, whiteboard->filename);
	}
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	return 0;
}

/*! 清理内部变量 */
int janus_whiteboard_free(janus_whiteboard *whiteboard) {
	if(!whiteboard)
		return -1;
	janus_whiteboard_close(whiteboard);
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	g_free(whiteboard->dir);
	whiteboard->dir = NULL;
	g_free(whiteboard->filename);
	whiteboard->filename = NULL;
	fclose(whiteboard->header_file);
	whiteboard->header_file = NULL;
	fclose(whiteboard->file);
	whiteboard->file = NULL;
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	g_free(whiteboard);
	return 0;
}