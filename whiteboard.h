#ifndef _JANUS_WHITEBOARD_H
#define _JANUS_WHITEBOARD_H

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "protobuf/command.pb-c.h"
#include "mutex.h"

typedef enum
{
    KLDrawCommandType_BeginDraw = 0,
    KLDrawCommandType_Drawing,
    KLDrawCommandType_EndDraw,

    KLDrawCommandType_BeginEraser,
    KLDrawCommandType_Erasing,
    KLDrawCommandType_EndEraser,
} KLDrawCommandType;

typedef enum {
	KLPackageType_DrawCommand = 0,
    KLPackageType_SwitchScene,
    KLPackageType_CleanDraw,
    KLPackageType_SceneData
} KLDataPackageType;


/*! \brief Structure that represents a whiteboard */
typedef struct janus_whiteboard {
	/*! \brief Absolute path to the directory where the whiteboard file is stored */ 
	char *dir;
	/*! \brief Filename of this whiteboard file */ 
	char *filename;
	/*! \brief whiteboard file */
	FILE *file;
	
	int scene;
	Pb__Package **scene_packages;
	int scene_package_num;
	/*! \brief Mutex to lock/unlock this whiteboard instance */ 
	janus_mutex mutex;
} janus_whiteboard;

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
janus_whiteboard *janus_whiteboard_create(const char *dir, const char *filename, int scene);
/*! \brief Save an RTP whiteboard frame in the whiteboard
 * @param[in] whiteboard The janus_whiteboard instance to save the frame to
 * @param[in] buffer The frame data to save
 * @param[in] length The frame data length
 * @returns 0 in case of success, a negative integer otherwise */
int janus_whiteboard_save_package(janus_whiteboard *whiteboard, char *buffer, uint length, uint8_t **out);

uint8_t *janus_whiteboard_current_scene_data(janus_whiteboard *whiteboard, int *size);
/*! \brief Close the whiteboard
 * @param[in] whiteboard The janus_whiteboard instance to close
 * @returns 0 in case of success, a negative integer otherwise */
int janus_whiteboard_close(janus_whiteboard *whiteboard);
/*! \brief Free the whiteboard resources
 * @param[in] whiteboard The janus_whiteboard instance to free
 * @returns 0 in case of success, a negative integer otherwise */
int janus_whiteboard_free(janus_whiteboard *whiteboard);

#endif