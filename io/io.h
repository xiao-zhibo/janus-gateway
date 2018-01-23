#ifndef _JANUS_TRANSPORT_H
#define _JANUS_TRANSPORT_H

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <inttypes.h>

#include <jansson.h>


/*! \brief Version of the API, to match the one transport plugins were compiled against */
#define JANUS_IO_API_VERSION	1

/*! \brief Initialization of all transport plugin properties to NULL
 * 
 * \note All transport plugins MUST add this as the FIRST line when initializing
 * their transport plugin structure, e.g.:
 * 
\verbatim
static janus_transport janus_http_transport plugin =
	{
		JANUS_TRANSPORT_INIT,
		
		.init = janus_http_init,
		[..]
\endverbatim
 * */
#define JANUS_IO_INIT(...) {		\
		.init = NULL,					\
		.destroy = NULL,				\
		.get_api_compatibility = NULL,	\
		.io_info_create = NULL,			\
		.io_info_close = NULL,			\
		.write_data = NULL,		\
		.read_data = NULL,			\
		.read_data_range = NULL,		\
		## __VA_ARGS__ }


/*! \brief The transport plugin session and callbacks interface */
typedef struct janus_io janus_io;


typedef struct janus_io_info {
	/*! \brief Opaque pointer to the io */
	void *io_handle;
	
	char *path;
} janus_io_info;

/*! \brief The io plugin session and callbacks interface */
struct janus_io {
	/*! \brief io plugin initialization/constructor
	 * @param[in] callback The callback instance the io plugin can use to contact the gateway
	 * @param[in] config_path Path of the folder where the configuration for this io plugin can be found
	 * @returns 0 in case of success, a negative integer in case of error */
	int (* const init)(const char *config_path);
	/*! \brief io plugin deinitialization/destructor */
	void (* const destroy)(void);

	int (* const get_api_compatibility)(void);

	int (* const io_info_create)(janus_io_info *info);
	int (* const io_info_close) (janus_io_info *info);

	int (* const write_data)(janus_io_info *io, char *buf, size_t len);
	int (* const read_data)(janus_io_info *io, char *buf);
	int (* const read_data_range)(janus_io_info *io, char *buf, size_t start, size_t end);

};

janus_io_info *janus_io_info_new(const char *path);

void janus_io_info_free(const janus_io_info *io_info);

/*! \brief The hook that transport plugins need to implement to be created from the gateway */
typedef janus_io* create_i(void);

#endif
