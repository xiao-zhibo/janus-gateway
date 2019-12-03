#ifndef _JANUS_WHITEBOARD_H
#define _JANUS_WHITEBOARD_H

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include "protobuf/command.pb-c.h"
#include "mutex.h"

#include "io/io.h"

typedef enum
{
    KLDrawCommandType_BeginDraw = 0,
    KLDrawCommandType_Drawing,
    KLDrawCommandType_EndDraw,

    KLDrawCommandType_BeginEraser,
    KLDrawCommandType_Erasing,
    KLDrawCommandType_EndEraser,
    KLDrawCommandType_ErasePath,
} KLDrawCommandType;

typedef enum {
	KLPackageType_None = -1,			 //非法
	KLPackageType_DrawCommand = 0,
	KLPackageType_SwitchScenePage = 1,
	KLPackageType_CleanDraw = 2,
	KLPackageType_ScenePageData = 3,
	KLPackageType_KeyFrame = 4,			 //已废弃，关键帧，用于快速切换场景页面
	KLPackageType_AddScene = 5,
	KLPackageType_SceneData = 6,		 //废弃？
	KLPackageType_EnableUserDraw = 7,	 //已废弃，开启/关闭学生画图
	KLPackageType_DeleteScene = 8,		 //暂不支持，删除场景
	KLPackageType_ModifyScene = 9,		 //暂不支持，修改场景
	KLPackageType_SceneOrderChange = 10, //暂不支持，更改场景顺序
	KLPackageType_PageChange = 11,
} KLDataPackageType;

#define MAX_PACKET_CAPACITY 100000
#define BASE_PACKET_CAPACITY 100

typedef struct janus_page {
	int scene;
	int page;
	int angle;
	float scale;
	float move_x;
	float move_y;
	Pb__KeyFrame *key_frame;
} janus_page;

typedef struct janus_scene {
	char *source_id;
	char *source_url;
	int type;
	int index;
	janus_page **pages;
	int page_num;

	/*! 坐标存page, 指针指向相应的keyframe。用于快速定位筛选出符合的场景数据给回前端 */
	// Pb__KeyFrame **page_keyframes;
	// int page_keyframe_maxnum;
} janus_scene;

/*! \brief Structure that represents a whiteboard */
typedef struct janus_whiteboard {
	/*! \brief Absolute path to the directory where the whiteboard file is stored */ 
	char *dir;
	/*! \brief Filename of this whiteboard file */ 
	char *filename;

	/*! \brief whiteboard header file */
	FILE *header_file;
	/*! \brief whiteboard scene data */
	FILE *scene_file;
	/*! \brief whiteboard scitch page index */
	FILE *page_file;
	/*! \brief whiteboard data file */
	FILE *file;
	
	//! 坐标存scene, 指针指向相应的keyframe。用于快速定位筛选出符合的场景数据给回前端 
	GHashTable *scenes;

	janus_page cur_page;
	GPtrArray *packages;
	// int scene;
	// int page;

	int64_t start_timestamp;
	
	/*! \brief Mutex to lock/unlock this whiteboard instance */ 
	janus_mutex mutex;
} janus_whiteboard;

/*! \brief 用于在将处理结果返回给调用者。 */
typedef struct janus_whiteboard_result {
	int   ret;

	/*! \brief 前段获取场景数据的时候，要求有关键帧和指令包，不能合并到一个包. */
	int   keyframe_len;
	void* keyframe_buf;
	int   command_len;
	void* command_buf;
	KLDataPackageType package_type;
} janus_whiteboard_result;

/*! \brief Initialize the whiteboard code
 * @param[in] tempnames Whether the filenames should have a temporary extension, while saving, or not
 * @param[in] extension Extension to add in case tempnames is true */
// void janus_whiteboard_init(gboolean tempnames, const char *extension);
/*! \brief De-initialize the whiteboard code */
// void janus_whiteboard_deinit(void);

/*! \brief Create a new whiteboard
 * \note If no target directory is provided, the current directory will be used. If no filename
 * is passed, a random filename will be used.
 * @param[in] dir Path of the directory to save the recording into (will try to create it if it doesn't exist)
 * @param[in] filename Filename to use for the recording
 * @returns A valid janus_whiteboard instance in case of success, NULL otherwise */
janus_whiteboard *janus_whiteboard_create(const char *local_dir, const char *filename);
/*! \brief Save an RTP whiteboard frame in the whiteboard
 * @param[in] whiteboard The janus_whiteboard instance to save the frame to
 * @param[in] buffer The frame data to save
 * @param[in] length The frame data length
 * @returns 0 in case of success, a negative integer otherwise */
janus_whiteboard_result janus_whiteboard_save_package(janus_whiteboard *whiteboard, char *buffer, size_t length);

janus_whiteboard_result janus_whiteboard_add_scene(janus_whiteboard *whiteboard, int package_type, char *resource_id, char *resource, int page_count, int type, int index);

janus_whiteboard_result janus_whiteboard_delete_scene(janus_whiteboard *whiteboard, int index);

janus_whiteboard_result janus_whiteboard_change_scene_order(janus_whiteboard *whiteboard, int index, int position);

janus_whiteboard_result janus_whiteboard_packet_extension(janus_whiteboard *whiteboard, int package_type, char *extension);

/*! \brief Close the whiteboard
 * @param[in] whiteboard The janus_whiteboard instance to close
 * @returns 0 in case of success, a negative integer otherwise */
int janus_whiteboard_close(janus_whiteboard *whiteboard);
/*! \brief Free the whiteboard resources
 * @param[in] whiteboard The janus_whiteboard instance to free
 * @returns 0 in case of success, a negative integer otherwise */
int janus_whiteboard_free(janus_whiteboard *whiteboard);

#endif