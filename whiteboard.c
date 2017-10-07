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
int      janus_whiteboard_remove_packets_l(Pb__Package** packages, int start_index, int len);
int      janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard);
void     janus_whiteboard_add_pkt_to_packages_l(Pb__Package** packages, size_t* packages_len, Pb__Package* dst_pkg);
void     janus_whiteboard_packed_data_l(Pb__Package **packages, int len, janus_whiteboard_result* result);
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

/*! 初始化header
    @returns 正常初始化返回header_len长度（非负数）， 异常返回 -1 */
int janus_whiteboard_parse_or_create_header_l(janus_whiteboard *whiteboard) {

	if(!whiteboard || !whiteboard->header_file)
		return -1;

	whiteboard->scene_keyframes    = g_malloc0(sizeof(Pb__KeyFrame*) * MAX_PACKET_CAPACITY);
	whiteboard->scene_keyframes[0] = g_malloc0(sizeof(Pb__KeyFrame));
	pb__key_frame__init(whiteboard->scene_keyframes[0]);
	whiteboard->scene_keyframes[0]->offset = 0;
	whiteboard->scene_keyframes[0]->timestamp = 0;
	whiteboard->scene_keyframe_maxnum = 1;

	/*int keyframe_len = 0;
	fseek(whiteboard->header_file, 0, SEEK_SET);

	// 尝试解析数据到whiteboard->header，如果不成功则执行创建操作
	while(fread(&keyframe_len, sizeof(size_t), 1, whiteboard->header_file) == 1) {
		char *buffer = g_malloc0(keyframe_len);
		if (janus_whiteboard_read_packet_from_file_l(buffer, keyframe_len, whiteboard->header_file) < 0) {
			JANUS_LOG(LOG_ERR, "Error happens when reading keyframe index packet from basefile: %s\n", whiteboard->filename);
			g_free(buffer);
			return -1;
		}
		Pb__KeyFrame *tmp_keyframe = pb__key_frame__unpack(NULL, keyframe_len, (const uint8_t*)buffer);
		if (tmp_keyframe != NULL) {
			//TODO seek to target and read the key frame.
			Pb__KeyFrame *out = g_malloc0(sizeof(Pb__KeyFrame));
			pb__key_frame__init(out);
			out->offset    = tmp_keyframe->offset;
			out->timestamp = tmp_keyframe->timestamp;

			pb__key_frame__free_unpacked(tmp_keyframe, NULL);
		}
		g_free(buffer);
	}*/
	JANUS_LOG(LOG_HUGE, "Parse or create header success\n");

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

    if (janus_whiteboard_parse_or_create_header_l(whiteboard) < 0) {
    	    JANUS_LOG(LOG_ERR, "Parse or create header error.\n");
    	    return NULL;
    }
	janus_mutex_init(&whiteboard->mutex);

	whiteboard->scene_packages = g_malloc0(sizeof(Pb__Package*) * MAX_PACKET_CAPACITY);
	if (whiteboard->scene_packages) {
	    whiteboard->scene_package_num = janus_whiteboard_scene_data_l(whiteboard, whiteboard->scene, whiteboard->scene_packages);
	} else {
		whiteboard->scene_package_num = 0;
		JANUS_LOG(LOG_ERR, "Out of Memory when alloc memory for %d struct scene_packages!\n", MAX_PACKET_CAPACITY);
	}

	return whiteboard;
}

/*! 将所有的指令集合到一个pkp，再打包为 buffer 二进制数据返回给客户端
    len=0的时候继续打包，返回一个清屏指令 */
void janus_whiteboard_packed_data_l(Pb__Package **packages, int num, janus_whiteboard_result* result) {
	if (packages == NULL || num < 0 || result == NULL) {
		result->ret = -1;
		return;
	}

	// 打包 command 数据
	int total_cmd_num = 0;
	for (int i = 0; i < num; i ++) {
		total_cmd_num += packages[i]->n_cmd;
	}

	Pb__Package out_pkg;
	if (num == 0) {
		pb__package__init(&out_pkg);
		out_pkg.scene = 0;
		out_pkg.type  = KLPackageType_CleanDraw;
	} else {
		pb__package__init(&out_pkg);
		out_pkg.scene = packages[0]->scene;
		out_pkg.type  = KLPackageType_DrawCommand;
		out_pkg.n_cmd = total_cmd_num;
		out_pkg.cmd   = g_malloc0(sizeof(Pb__Command) * total_cmd_num);
	}

	size_t cmd_index = 0;
	for (int i = 0; i < num; i ++) {
		Pb__Command **cmd = packages[i]->cmd;
		if (packages[i]->n_cmd == 0)
			continue;
		for (size_t j = 0; j < packages[i]->n_cmd; j ++) {
			out_pkg.cmd[cmd_index++] = cmd[j];
		}
	}

	result->command_len = pb__package__get_packed_size(&out_pkg);
	result->command_buf = g_malloc0(result->command_len);
	pb__package__pack(&out_pkg, result->command_buf);
	g_free(out_pkg.cmd);

	// 打包 keyframe 数据
	if (num > 0 && packages != NULL) {
		Pb__Package *first_package = packages[0];
		if (first_package->type == KLPackageType_KeyFrame) {
			result->keyframe_len = pb__package__get_packed_size(first_package);
			result->keyframe_buf = g_malloc0(result->keyframe_len);
			pb__package__pack(first_package, result->keyframe_buf);
		}
	}
}

void janus_whiteboard_add_pkt_to_packages_l(Pb__Package** packages, size_t* packages_len, Pb__Package* dst_pkg) {
	if (packages == NULL || packages_len == NULL || dst_pkg == NULL)
		return;

	if (dst_pkg->type == KLPackageType_CleanDraw) {
	    // 清屏指令，移除已经存在的包.
	    janus_whiteboard_remove_packets_l(packages, 0, *packages_len);
	    *packages_len = 0;
    } else if (dst_pkg->type == KLPackageType_KeyFrame) {
		// 遇到关键帧，移除已经存在的包.
		janus_whiteboard_remove_packets_l(packages, 0, *packages_len);
		packages[0] = dst_pkg;
		*packages_len = 1;
	} else if (dst_pkg->type != KLPackageType_SceneData
		    && dst_pkg->type != KLPackageType_SwitchScene) {
		// FIXME:Rison 有新的指令过来需要考虑这里. 过滤掉特殊指令
		packages[*packages_len] = dst_pkg;
		(*packages_len) ++;
	}
}

/*! 从指定的场景获取白板笔迹。先移到当前场景最接近keyframe附近开始查找，需要对clean以及keyframe做额外处理
    @returns 成功获取返回 pkt 的数目， 获取失败返回 -1 */
int janus_whiteboard_scene_data_l(janus_whiteboard *whiteboard, int scene, Pb__Package** packages) {
	if (whiteboard == NULL || whiteboard->file == NULL)
		return -1;

	int package_data_offset = 0;
	if (scene < whiteboard->scene_keyframe_maxnum) {
		Pb__KeyFrame *target_keyframe = whiteboard->scene_keyframes[scene];
		if (target_keyframe != NULL) {
			package_data_offset = target_keyframe->offset;
			JANUS_LOG(LOG_VERB, "Get scene data offset %d\n", package_data_offset);
		}
	}
	// seek 到文件开头。FIXME:Rison 使用数组存起来 scene--->offset, 就不需要每次从头开始读取了
	fseek(whiteboard->file, package_data_offset, SEEK_SET);
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
			janus_whiteboard_add_pkt_to_packages_l(packages, &out_len, package);
		} else {
			pb__package__free_unpacked(package, NULL);
		}

		g_free(buffer);
	}

	fseek(whiteboard->file, 0, SEEK_END);
	return out_len;
}

int janus_whiteboard_on_receive_keyframe_l(janus_whiteboard *whiteboard, Pb__Package *package) {
	if (!whiteboard->file || !package)
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
	next->timestamp = package->timestamp;

	int scene_index = package->scene;
	Pb__KeyFrame **target_keyframe = &whiteboard->scene_keyframes[scene_index];
	if (*target_keyframe != NULL) {
		g_free(*target_keyframe);
	}
	*target_keyframe = next;
	if (scene_index > whiteboard->scene_keyframe_maxnum) {
	    whiteboard->scene_keyframe_maxnum = scene_index + 1;//scene 从 0 开始
	}

	/*! 将 keyframe 保存到文件 */
	size_t length = pb__key_frame__get_packed_size(*target_keyframe);
	void *buffer = g_malloc0(length);
	if (buffer == NULL) {
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

	if (package->type == KLPackageType_SwitchScene) {
	// 切换白板场景
		if (package->scene == whiteboard->scene) {
			JANUS_LOG(LOG_WARN, "Get a request to switch scene, but currenttly the whiteboard is on the target %d scene\n", package->scene);
		    janus_mutex_unlock_nodebug(&whiteboard->mutex);
		    result.ret = 0;
			return result;
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
		if ( package->scene == whiteboard->scene || package->scene == -1 ) {
			// 当前场景
			janus_whiteboard_packed_data_l(whiteboard->scene_packages, whiteboard->scene_package_num, &result);
		} else if (package->scene >= 0) {
			// 其他场景
			Pb__Package **packages = g_malloc0(sizeof(Pb__Package*) * MAX_PACKET_CAPACITY);
			int num = janus_whiteboard_scene_data_l(whiteboard, package->scene, packages);
			janus_whiteboard_packed_data_l(packages, num, &(result.command_len));
			for (int i = 0; i < num; i ++) {
				pb__package__free_unpacked(packages[i], NULL);
			}
			g_free(packages);
		}
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		JANUS_LOG(LOG_VERB, "Get scene data with keyframe:%d, command:%d\n", result.keyframe_len, result.command_len);
		result.ret = 1;
		return result;
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
	
	// 保存数据到当前场景（内存），以便快速处理KLPackageType_SceneData指令
	if (package->scene == whiteboard->scene) {
		janus_whiteboard_add_pkt_to_packages_l(whiteboard->scene_packages, &(whiteboard->scene_package_num), package);
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
	/*! 清理用于快速定位的关键帧索引 */
	for (int start_index = 0; start_index < whiteboard->scene_keyframe_maxnum; start_index++) {
		if (whiteboard->scene_keyframes[start_index] != NULL) {
			g_free(whiteboard->scene_keyframes[start_index]);
			whiteboard->scene_keyframes[start_index] = NULL;
		}
	}
	g_free(whiteboard->scene_keyframes);
	whiteboard->scene_keyframes = NULL;
	
	/*! 清理当前场景缓存的白板数据 */
	janus_whiteboard_remove_packets_l(whiteboard->scene_packages, 0, whiteboard->scene_package_num);
	g_free(whiteboard->scene_packages);
	whiteboard->scene_packages = NULL;
	whiteboard->scene_package_num = 0;

	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	g_free(whiteboard);
	return 0;
}