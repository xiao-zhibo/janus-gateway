#include "whiteboard.h"
#include <sys/stat.h>
#include "debug.h"
#include "utils.h"

/* Info header in the structured recording */
static const char *header = "MJR00001";

int janus_whiteboard_scene_data(janus_whiteboard *whiteboard, int scene, Pb__Package** packages) {
	if (whiteboard == NULL || whiteboard->file == NULL)
		return -1;
	int packageLength = 0;
	fseek(whiteboard->file, strlen(header), SEEK_SET);
	int packLen;

	while(fread(&packLen, sizeof(int), 1, whiteboard->file) != 1) {
		char *buffer = g_malloc0(packLen);
		int temp = 0, tot = packLen;
		while(tot > 0) {
			temp = fread(buffer+packLen-tot, sizeof(char), tot, whiteboard->file);
			if(temp <= 0) {
				JANUS_LOG(LOG_ERR, "Error saving frame...\n");
				fseek(whiteboard->file, 0, SEEK_END);
				return -1;
			}
			tot -= temp;
		}
		Pb__Package *package = pb__package__unpack(NULL, packLen, buffer);
		if (package == NULL)
		{
			JANUS_LOG(LOG_WARN, "parse whiteboard data error.");
			fseek(whiteboard->file, 0, SEEK_END);
			return -1;
		}
		if (package->scene == scene) {
			if (package->type == KLPackageType_CleanDraw) {
				packageLength = 0;
			} else {
				packages[packageLength] = package;
				packageLength ++;
			}
		}
		g_free(buffer);
	}
	fseek(whiteboard->file, 0, SEEK_END);
	return packageLength;
}

uint8_t *janus_whiteboard_packed_data(Pb__Package **packages, int len, int *out_len) {
	if (packages == NULL || len <= 0) 
		return -1;

	int cmd_num = 0;
	for (int i = 0; i < len; i ++) {
		cmd_num += packages[i]->n_cmd;
	}
	Pb__Package package;
	package = *packages[0];
	package.n_cmd = cmd_num;
	package.cmd = g_malloc0(sizeof(Pb__Command) * cmd_num);
	int k = 0;
	for (int i = 0; i < len; i ++) {
		Pb__Command **cmd = packages[i]->cmd;
		for (int j = 0; j < packages[i]->n_cmd; j ++) {
			package.cmd[k++] = cmd[j];
		}
	}
	*out_len = pb__package__get_packed_size(&package);
	uint8_t *buffer = g_malloc0(*out_len);
	pb__package__pack(&package, buffer);
	g_free(package.cmd);
	return buffer;
}

janus_whiteboard *janus_whiteboard_create(const char *dir, const char *filename, int scene) {
	/* Create the recorder */
	janus_whiteboard *whiteboard = g_malloc0(sizeof(janus_whiteboard));
	if(whiteboard == NULL) {
		JANUS_LOG(LOG_FATAL, "Memory error!\n");
		return NULL;
	}
	whiteboard->dir = NULL;
	whiteboard->filename = NULL;
	whiteboard->file = NULL;
	
	if(dir != NULL) {
		/* Check if this directory exists, and create it if needed */
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
	char newname[1024];
	memset(newname, 0, 1024);
	g_snprintf(newname, 1024, "%s.mjr", filename);

	/* Try opening the file now */
	if(dir == NULL) {
		whiteboard->file = fopen(newname, "ab+");
	} else {
		char path[1024];
		memset(path, 0, 1024);
		g_snprintf(path, 1024, "%s/%s", dir, newname);
		whiteboard->file = fopen(path, "ab+");
	}
	if(whiteboard->file == NULL) {
		JANUS_LOG(LOG_ERR, "fopen error: %d\n", errno);
		return NULL;
	}
	if(dir)
		whiteboard->dir = g_strdup(dir);
	whiteboard->filename = g_strdup(newname);

	if (ftell(whiteboard->file) == 0) {
		fwrite(header, sizeof(char), strlen(header), whiteboard->file);
	} else {
		whiteboard->scene_packages = g_malloc0(sizeof(Pb__Package*) * 10000);
		whiteboard->scene_package_num = janus_whiteboard_scene_data(whiteboard, whiteboard->scene, whiteboard->scene_packages);
		return NULL;
	}
	
	janus_mutex_init(&whiteboard->mutex);
	return whiteboard;
}

int janus_whiteboard_save_package(janus_whiteboard *whiteboard, char *buffer, uint length, uint8_t **out) {
	if(!whiteboard)
		return -1;
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	if(!buffer || length < 1) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return -2;
	}
	if(!whiteboard->file) {
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return -3;
	}
	Pb__Package *package = pb__package__unpack(NULL, length, buffer);
	if (package == NULL)
	{
		JANUS_LOG(LOG_WARN, "parse whiteboard data error.");
		return -4;
	}
	if (package->type == KLPackageType_SwitchScene && package->scene != whiteboard->scene) {
		whiteboard->scene = package->scene;
		for (int i = 0; i < whiteboard->scene_package_num; i ++) {
			pb__package__free_unpacked(whiteboard->scene_packages[i], NULL);
			whiteboard->scene_packages[i] = NULL;
		}
		whiteboard->scene_package_num = janus_whiteboard_scene_data(whiteboard, whiteboard->scene, whiteboard->scene_packages);
		if (whiteboard->scene_package_num <= 0)
			JANUS_LOG(LOG_WARN, "save whiteboard package num : %d", whiteboard->scene_package_num);
	} else if (package->type == KLPackageType_SceneData) {
		int size = 0;
		uint8_t *buf;
		if (package->scene == whiteboard->scene) {
			*out = janus_whiteboard_packed_data(whiteboard->scene_packages, whiteboard->scene_package_num, &size);
		} else {
			Pb__Package **packages = g_malloc0(sizeof(Pb__Package*) * 10000);
			int num = janus_whiteboard_scene_data(whiteboard, whiteboard->scene, packages);
			*out = janus_whiteboard_packed_data(packages, num, &size);
			for (int i = 0; i < num; i ++) {
				pb__package__free_unpacked(packages[i], NULL);
			}
			g_free(packages);
		}
		janus_mutex_unlock_nodebug(&whiteboard->mutex);
		return size;
	} 

	if (package->type != KLPackageType_SceneData) {
		/* Save packet on file */
		fwrite(&length, sizeof(uint), 1, whiteboard->file);
		int temp = 0, tot = length;
		while(tot > 0) {
			temp = fwrite(buffer+length-tot, sizeof(char), tot, whiteboard->file);
			if(temp <= 0) {
				JANUS_LOG(LOG_WARN, "Error saving frame...\n");
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

uint8_t *janus_whiteboard_current_scene_data(janus_whiteboard *whiteboard, int *size) {
	uint8_t *out = janus_whiteboard_packed_data(whiteboard->scene_packages, whiteboard->scene_package_num, size);
	return out;
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

int janus_whiteboard_free(janus_whiteboard *whiteboard) {
	if(!whiteboard)
		return -1;
	janus_whiteboard_close(whiteboard);
	janus_mutex_lock_nodebug(&whiteboard->mutex);
	g_free(whiteboard->dir);
	whiteboard->dir = NULL;
	g_free(whiteboard->filename);
	whiteboard->filename = NULL;
	fclose(whiteboard->file);
	whiteboard->file = NULL;
	janus_mutex_unlock_nodebug(&whiteboard->mutex);
	g_free(whiteboard);
	return 0;
}