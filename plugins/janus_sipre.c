/*! \file   janus_sipre.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus SIPre plugin (libre)
 * \details  This is basically a clone of the SIPre plugin, with the key
 * difference being that it uses \c libre (http://creytiv.com/re.html)
 * instead of Sofia SIP for its internal stack. As such, it provides an
 * alternative for those who don't want to, or can't, use the Sofia-based
 * SIP plugin. The API it exposes is exactly the same, meaning it should
 * be pretty straightforward to switch from one plugin to another on the
 * client side. The configuration file looks exactly the same as well.
 *
 * \section sipapi SIPre Plugin API
 *
 * All requests you can send in the SIPre Plugin API are asynchronous,
 * which means all responses (successes and errors) will be delivered
 * as events with the same transaction.
 *
 * The supported requests are \c register , \c call , \c accept and
 * \c hangup . \c register can be used, as the name suggests, to register
 * a username at a SIPre registrar to call and be called; \c call is used
 * to send an INVITE to a different SIPre URI through the plugin, while
 * \c accept is used to accept the call in case one is invited instead
 * of inviting; finally, \c hangup can be used to terminate the
 * communication at any time, either to hangup (BYE) an ongoing call or
 * to cancel/decline (CANCEL/BYE) a call that hasn't started yet.
 *
 * Actual API docs: TBD.
 *
 * \ingroup plugins
 * \ref plugins
 */

#include "plugin.h"

#include <arpa/inet.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>

#include <jansson.h>

#include <re_types.h>
#include <re_fmt.h>
#include <re_mbuf.h>
#include <re_msg.h>
#include <re_list.h>
#include <re_sa.h>
#include <re_main.h>
#include <re_mem.h>
#include <re_mqueue.h>
#include <re_sdp.h>
#include <re_uri.h>
#include <re_sip.h>
#include <re_sipreg.h>
#include <re_sipsess.h>
#include <re_srtp.h>
#include <re_tmr.h>
#include <re_tls.h>

#include "../debug.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../record.h"
#include "../rtp.h"
#include "../rtcp.h"
#include "../sdp-utils.h"
#include "../utils.h"
#include "../ip-utils.h"


/* Plugin information */
#define JANUS_SIPRE_VERSION			1
#define JANUS_SIPRE_VERSION_STRING	"0.0.1"
#define JANUS_SIPRE_DESCRIPTION		"This is a simple SIP plugin for Janus (based on libre instead of Sofia), allowing WebRTC peers to register at a SIP server and call SIP user agents through the gateway."
#define JANUS_SIPRE_NAME			"JANUS SIPre plugin"
#define JANUS_SIPRE_AUTHOR			"Meetecho s.r.l."
#define JANUS_SIPRE_PACKAGE			"janus.plugin.sipre"

/* Plugin methods */
janus_plugin *create(void);
int janus_sipre_init(janus_callbacks *callback, const char *config_path);
void janus_sipre_destroy(void);
int janus_sipre_get_api_compatibility(void);
int janus_sipre_get_version(void);
const char *janus_sipre_get_version_string(void);
const char *janus_sipre_get_description(void);
const char *janus_sipre_get_name(void);
const char *janus_sipre_get_author(void);
const char *janus_sipre_get_package(void);
void janus_sipre_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_sipre_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
void janus_sipre_setup_media(janus_plugin_session *handle);
void janus_sipre_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_sipre_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_sipre_hangup_media(janus_plugin_session *handle);
void janus_sipre_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_sipre_query_session(janus_plugin_session *handle);

/* Plugin setup */
static janus_plugin janus_sipre_plugin =
	JANUS_PLUGIN_INIT (
		.init = janus_sipre_init,
		.destroy = janus_sipre_destroy,

		.get_api_compatibility = janus_sipre_get_api_compatibility,
		.get_version = janus_sipre_get_version,
		.get_version_string = janus_sipre_get_version_string,
		.get_description = janus_sipre_get_description,
		.get_name = janus_sipre_get_name,
		.get_author = janus_sipre_get_author,
		.get_package = janus_sipre_get_package,

		.create_session = janus_sipre_create_session,
		.handle_message = janus_sipre_handle_message,
		.setup_media = janus_sipre_setup_media,
		.incoming_rtp = janus_sipre_incoming_rtp,
		.incoming_rtcp = janus_sipre_incoming_rtcp,
		.hangup_media = janus_sipre_hangup_media,
		.destroy_session = janus_sipre_destroy_session,
		.query_session = janus_sipre_query_session,
	);

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_SIPRE_NAME);
	return &janus_sipre_plugin;
}

/* Parameter validation */
static struct janus_json_parameter request_parameters[] = {
	{"request", JANUS_JSON_STRING, JANUS_JSON_PARAM_REQUIRED}
};
static struct janus_json_parameter register_parameters[] = {
	{"type", JANUS_JSON_STRING, 0},
	{"send_register", JANUS_JSON_BOOL, 0},
	{"sips", JANUS_JSON_BOOL, 0},
	{"username", JANUS_JSON_STRING, 0},
	{"secret", JANUS_JSON_STRING, 0},
	{"ha1_secret", JANUS_JSON_STRING, 0},
	{"authuser", JANUS_JSON_STRING, 0}
};
static struct janus_json_parameter proxy_parameters[] = {
	{"proxy", JANUS_JSON_STRING, 0}
};
static struct janus_json_parameter call_parameters[] = {
	{"uri", JANUS_JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"autoack", JANUS_JSON_BOOL, 0},
	{"headers", JANUS_JSON_OBJECT, 0},
	{"srtp", JANUS_JSON_STRING, 0}
};
static struct janus_json_parameter accept_parameters[] = {
	{"srtp", JANUS_JSON_STRING, 0}
};
static struct janus_json_parameter recording_parameters[] = {
	{"action", JANUS_JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"audio", JANUS_JSON_BOOL, 0},
	{"video", JANUS_JSON_BOOL, 0},
	{"peer_audio", JANUS_JSON_BOOL, 0},
	{"peer_video", JANUS_JSON_BOOL, 0},
	{"filename", JANUS_JSON_STRING, 0}
};
static struct janus_json_parameter dtmf_info_parameters[] = {
	{"digit", JANUS_JSON_STRING, JANUS_JSON_PARAM_REQUIRED},
	{"duration", JANUS_JSON_INTEGER, JANUS_JSON_PARAM_POSITIVE}
};

/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static gboolean notify_events = TRUE;
static janus_callbacks *gateway = NULL;

static char *local_ip = NULL;
static int keepalive_interval = 120;
static gboolean behind_nat = FALSE;
static char *user_agent;
#define JANUS_DEFAULT_REGISTER_TTL	3600
static int register_ttl = JANUS_DEFAULT_REGISTER_TTL;

static GThread *handler_thread;
static GThread *watchdog;
static void *janus_sipre_handler(void *data);

typedef struct janus_sipre_message {
	janus_plugin_session *handle;
	char *transaction;
	json_t *message;
	json_t *jsep;
} janus_sipre_message;
static GAsyncQueue *messages = NULL;
static janus_sipre_message exit_message;

static void janus_sipre_message_free(janus_sipre_message *msg) {
	if(!msg || msg == &exit_message)
		return;

	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	if(msg->message)
		json_decref(msg->message);
	msg->message = NULL;
	if(msg->jsep)
		json_decref(msg->jsep);
	msg->jsep = NULL;

	g_free(msg);
}

/* libre SIP stack */
static struct sip *sipstack;
static struct tls *tls = NULL;
GThread *sipstack_thread = NULL;

/* Message queue */
typedef enum janus_sipre_mqueue_event {
	janus_sipre_mqueue_event_do_init,
	janus_sipre_mqueue_event_do_register,
	/* TODO Add other events here */
	janus_sipre_mqueue_event_do_exit
} janus_sipre_mqueue_event;
static struct mqueue *mq = NULL;
void janus_sipre_mqueue_handler(int id, void *data, void *arg);

/* Registration info */
typedef enum {
	janus_sipre_registration_status_disabled = -2,
	janus_sipre_registration_status_failed = -1,
	janus_sipre_registration_status_unregistered = 0,
	janus_sipre_registration_status_registering,
	janus_sipre_registration_status_registered,
	janus_sipre_registration_status_unregistering,
} janus_sipre_registration_status;

static const char *janus_sipre_registration_status_string(janus_sipre_registration_status status) {
	switch(status) {
		case janus_sipre_registration_status_disabled:
			return "disabled";
		case janus_sipre_registration_status_failed:
			return "failed";
		case janus_sipre_registration_status_unregistered:
			return "unregistered";
		case janus_sipre_registration_status_registering:
			return "registering";
		case janus_sipre_registration_status_registered:
			return "registered";
		case janus_sipre_registration_status_unregistering:
			return "unregistering";
		default:
			return "unknown";
	}
}


typedef enum {
	janus_sipre_call_status_idle = 0,
	janus_sipre_call_status_inviting,
	janus_sipre_call_status_invited,
	janus_sipre_call_status_incall,
	janus_sipre_call_status_closing,
} janus_sipre_call_status;

static const char *janus_sipre_call_status_string(janus_sipre_call_status status) {
	switch(status) {
		case janus_sipre_call_status_idle:
			return "idle";
		case janus_sipre_call_status_inviting:
			return "inviting";
		case janus_sipre_call_status_invited:
			return "invited";
		case janus_sipre_call_status_incall:
			return "incall";
		case janus_sipre_call_status_closing:
			return "closing";
		default:
			return "unknown";
	}
}


typedef enum {
	janus_sipre_secret_type_plaintext = 1,
	janus_sipre_secret_type_hashed = 2,
	janus_sipre_secret_type_unknown
} janus_sipre_secret_type;

typedef struct janus_sipre_account {
	char *identity;
	char *user_agent;		/* Used to override the general UA string */
	gboolean sips;
	char *username;
	char *display_name;		/* Used for outgoing calls in the From header */
	char *authuser;			/**< username to use for authentication */
	char *secret;
	janus_sipre_secret_type secret_type;
	int sip_port;
	char *proxy;
	janus_sipre_registration_status registration_status;
} janus_sipre_account;

typedef struct janus_sipre_stack {
	struct sipsess *sess;				/* SIP session */
	struct sipsess_sock *sess_sock;		/* SIP session socket */
	struct sipreg *reg;					/* SIP registration */
	struct sdp_session *sdp;			/* SDP session */
	struct sdp_media *sdp_media;		/* SDP media */
	void *session;						/* Opaque pointer to the plugin session */
} janus_sipre_stack;

typedef struct janus_sipre_media {
	char *remote_ip;
	int ready:1;
	gboolean autoack;
	gboolean require_srtp, has_srtp_local, has_srtp_remote;
	int has_audio:1;
	int audio_rtp_fd, audio_rtcp_fd;
	int local_audio_rtp_port, remote_audio_rtp_port;
	int local_audio_rtcp_port, remote_audio_rtcp_port;
	guint32 audio_ssrc, audio_ssrc_peer;
	int audio_pt;
	const char *audio_pt_name;
	srtp_t audio_srtp_in, audio_srtp_out;
	srtp_policy_t audio_remote_policy, audio_local_policy;
	int audio_srtp_suite_in, audio_srtp_suite_out;
	gboolean audio_send;
	int has_video:1;
	int video_rtp_fd, video_rtcp_fd;
	int local_video_rtp_port, remote_video_rtp_port;
	int local_video_rtcp_port, remote_video_rtcp_port;
	guint32 video_ssrc, video_ssrc_peer;
	int video_pt;
	const char *video_pt_name;
	srtp_t video_srtp_in, video_srtp_out;
	srtp_policy_t video_remote_policy, video_local_policy;
	int video_srtp_suite_in, video_srtp_suite_out;
	gboolean video_send;
	janus_rtp_switching_context context;
	int pipefd[2];
	gboolean updated;
} janus_sipre_media;

typedef struct janus_sipre_session {
	janus_plugin_session *handle;
	janus_sipre_stack stack;
	janus_sipre_account account;
	janus_sipre_call_status status;
	janus_sipre_media media;
	char *transaction;
	char *callee;
	char *callid;
	janus_sdp *sdp;				/* The SDP this user sent */
	janus_recorder *arc;		/* The Janus recorder instance for this user's audio, if enabled */
	janus_recorder *arc_peer;	/* The Janus recorder instance for the peer's audio, if enabled */
	janus_recorder *vrc;		/* The Janus recorder instance for this user's video, if enabled */
	janus_recorder *vrc_peer;	/* The Janus recorder instance for the peer's video, if enabled */
	janus_mutex rec_mutex;		/* Mutex to protect the recorders from race conditions */
	volatile gint hangingup;
	gint64 destroyed;	/* Time at which this session was marked as destroyed */
	janus_mutex mutex;
} janus_sipre_session;
static GHashTable *sessions;
static GList *old_sessions;
static GHashTable *identities;
static GHashTable *callids;
static janus_mutex sessions_mutex;


/* SRTP stuff (in case we need SDES) */
static int janus_sipre_srtp_set_local(janus_sipre_session *session, gboolean video, char **crypto) {
	if(session == NULL)
		return -1;
	/* Generate key/salt */
	uint8_t *key = g_malloc0(SRTP_MASTER_LENGTH);
	srtp_crypto_get_random(key, SRTP_MASTER_LENGTH);
	/* Set SRTP policies */
	srtp_policy_t *policy = video ? &session->media.video_local_policy : &session->media.audio_local_policy;
	srtp_crypto_policy_set_rtp_default(&(policy->rtp));
	srtp_crypto_policy_set_rtcp_default(&(policy->rtcp));
	policy->ssrc.type = ssrc_any_inbound;
	policy->key = key;
	policy->next = NULL;
	/* Create SRTP context */
	srtp_err_status_t res = srtp_create(video ? &session->media.video_srtp_out : &session->media.audio_srtp_out, policy);
	if(res != srtp_err_status_ok) {
		/* Something went wrong... */
		JANUS_LOG(LOG_ERR, "Oops, error creating outbound SRTP session: %d (%s)\n", res, janus_srtp_error_str(res));
		g_free(key);
		policy->key = NULL;
		return -2;
	}
	/* Base64 encode the salt */
	*crypto = g_base64_encode(key, SRTP_MASTER_LENGTH);
	if((video && session->media.video_srtp_out) || (!video && session->media.audio_srtp_out)) {
		JANUS_LOG(LOG_VERB, "%s outbound SRTP session created\n", video ? "Video" : "Audio");
	}
	return 0;
}
static int janus_sipre_srtp_set_remote(janus_sipre_session *session, gboolean video, const char *crypto, int suite) {
	if(session == NULL || crypto == NULL)
		return -1;
	/* Base64 decode the crypto string and set it as the remote SRTP context */
	gsize len = 0;
	guchar *decoded = g_base64_decode(crypto, &len);
	if(len < SRTP_MASTER_LENGTH) {
		/* FIXME Can this happen? */
		g_free(decoded);
		return -2;
	}
	/* Set SRTP policies */
	srtp_policy_t *policy = video ? &session->media.video_remote_policy : &session->media.audio_remote_policy;
	srtp_crypto_policy_set_rtp_default(&(policy->rtp));
	srtp_crypto_policy_set_rtcp_default(&(policy->rtcp));
	if(suite == 32) {
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&(policy->rtp));
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_32(&(policy->rtcp));
	} else if(suite == 80) {
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&(policy->rtp));
		srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&(policy->rtcp));
	}
	policy->ssrc.type = ssrc_any_inbound;
	policy->key = decoded;
	policy->next = NULL;
	/* Create SRTP context */
	srtp_err_status_t res = srtp_create(video ? &session->media.video_srtp_in : &session->media.audio_srtp_in, policy);
	if(res != srtp_err_status_ok) {
		/* Something went wrong... */
		JANUS_LOG(LOG_ERR, "Oops, error creating inbound SRTP session: %d (%s)\n", res, janus_srtp_error_str(res));
		g_free(decoded);
		policy->key = NULL;
		return -2;
	}
	if((video && session->media.video_srtp_in) || (!video && session->media.audio_srtp_in)) {
		JANUS_LOG(LOG_VERB, "%s inbound SRTP session created\n", video ? "Video" : "Audio");
	}
	return 0;
}
static void janus_sipre_srtp_cleanup(janus_sipre_session *session) {
	if(session == NULL)
		return;
	session->media.autoack = TRUE;
	session->media.require_srtp = FALSE;
	session->media.has_srtp_local = FALSE;
	session->media.has_srtp_remote = FALSE;
	/* Audio */
	if(session->media.audio_srtp_out)
		srtp_dealloc(session->media.audio_srtp_out);
	session->media.audio_srtp_out = NULL;
	g_free(session->media.audio_local_policy.key);
	session->media.audio_local_policy.key = NULL;
	session->media.audio_srtp_suite_out = 0;
	if(session->media.audio_srtp_in)
		srtp_dealloc(session->media.audio_srtp_in);
	session->media.audio_srtp_in = NULL;
	g_free(session->media.audio_remote_policy.key);
	session->media.audio_remote_policy.key = NULL;
	session->media.audio_srtp_suite_in = 0;
	/* Video */
	if(session->media.video_srtp_out)
		srtp_dealloc(session->media.video_srtp_out);
	session->media.video_srtp_out = NULL;
	g_free(session->media.video_local_policy.key);
	session->media.video_local_policy.key = NULL;
	session->media.video_srtp_suite_out = 0;
	if(session->media.video_srtp_in)
		srtp_dealloc(session->media.video_srtp_in);
	session->media.video_srtp_in = NULL;
	g_free(session->media.video_remote_policy.key);
	session->media.video_remote_policy.key = NULL;
	session->media.video_srtp_suite_in = 0;
}


/* libre event thread */
gpointer janus_sipre_stack_thread(gpointer user_data);
/* libre callbacks */
int janus_sipre_cb_auth(char **user, char **pass, const char *realm, void *arg);
void janus_sipre_cb_register(int err, const struct sip_msg *msg, void *arg);
void janus_sipre_cb_progress(const struct sip_msg *msg, void *arg);
void janus_sipre_cb_incoming(const struct sip_msg *msg, void *arg);
int janus_sipre_cb_offer(struct mbuf **mbp, const struct sip_msg *msg, void *arg);
int janus_sipre_cb_answer(const struct sip_msg *msg, void *arg);
void janus_sipre_cb_established(const struct sip_msg *msg, void *arg);
void janus_sipre_cb_closed(int err, const struct sip_msg *msg, void *arg);
void janus_sipre_cb_exit(void *arg);

/* URI parsing utilities */
static int janus_sipre_parse_uri(const char *uri) {
	if(uri == NULL)
		return -1;
	struct sip_addr addr;
	struct pl pluri;
	pl_set_str(&pluri, uri);
	if(sip_addr_decode(&addr, &pluri) != 0)
		return -1;
	return 0;
}
static char *janus_sipre_get_uri_username(const char *uri) {
	if(uri == NULL)
		return NULL;
	struct sip_addr addr;
	struct pl pluri;
	pl_set_str(&pluri, uri);
	if(sip_addr_decode(&addr, &pluri) != 0)
		return NULL;
	char *at = strchr(addr.uri.user.p, '@');
	if(at != NULL)
		*(at) = '\0';
	char *username = g_strdup(addr.uri.user.p);
	if(at != NULL)
		*(at) = '@';
	return username;
}
static char *janus_sipre_get_uri_host(const char *uri) {
	if(uri == NULL)
		return NULL;
	struct sip_addr addr;
	struct pl pluri;
	pl_set_str(&pluri, uri);
	if(sip_addr_decode(&addr, &pluri) != 0)
		return NULL;
	return g_strdup(addr.uri.host.p);
}
static uint16_t janus_sipre_get_uri_port(const char *uri) {
	if(uri == NULL)
		return 0;
	struct sip_addr addr;
	struct pl pluri;
	pl_set_str(&pluri, uri);
	if(sip_addr_decode(&addr, &pluri) != 0)
		return 0;
	return addr.uri.port;
}


/* SDP parsing and manipulation */
void janus_sipre_sdp_process(janus_sipre_session *session, janus_sdp *sdp, gboolean answer, gboolean update, gboolean *changed);
char *janus_sipre_sdp_manipulate(janus_sipre_session *session, janus_sdp *sdp, gboolean answer);
/* Media */
static int janus_sipre_allocate_local_ports(janus_sipre_session *session);
static void *janus_sipre_relay_thread(void *data);


/* Error codes */
#define JANUS_SIPRE_ERROR_UNKNOWN_ERROR		499
#define JANUS_SIPRE_ERROR_NO_MESSAGE			440
#define JANUS_SIPRE_ERROR_INVALID_JSON		441
#define JANUS_SIPRE_ERROR_INVALID_REQUEST		442
#define JANUS_SIPRE_ERROR_MISSING_ELEMENT		443
#define JANUS_SIPRE_ERROR_INVALID_ELEMENT		444
#define JANUS_SIPRE_ERROR_ALREADY_REGISTERED	445
#define JANUS_SIPRE_ERROR_INVALID_ADDRESS		446
#define JANUS_SIPRE_ERROR_WRONG_STATE			447
#define JANUS_SIPRE_ERROR_MISSING_SDP			448
#define JANUS_SIPRE_ERROR_LIBRE_ERROR		449
#define JANUS_SIPRE_ERROR_IO_ERROR			450
#define JANUS_SIPRE_ERROR_RECORDING_ERROR		451
#define JANUS_SIPRE_ERROR_TOO_STRICT			452


/* SIPre watchdog/garbage collector (sort of) */
static void *janus_sipre_watchdog(void *data) {
	JANUS_LOG(LOG_INFO, "SIPre watchdog started\n");
	gint64 now = 0;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		janus_mutex_lock(&sessions_mutex);
		/* Iterate on all the sessions */
		now = janus_get_monotonic_time();
		if(old_sessions != NULL) {
			GList *sl = old_sessions;
			JANUS_LOG(LOG_HUGE, "Checking %d old SIPre sessions...\n", g_list_length(old_sessions));
			while(sl) {
				janus_sipre_session *session = (janus_sipre_session *)sl->data;
				if(!session) {
					sl = sl->next;
					continue;
				}
				if(now-session->destroyed >= 5*G_USEC_PER_SEC) {
					/* We're lazy and actually get rid of the stuff only after a few seconds */
					JANUS_LOG(LOG_VERB, "Freeing old SIPre session\n");
					GList *rm = sl->next;
					old_sessions = g_list_delete_link(old_sessions, sl);
					sl = rm;
					if(session->account.identity) {
					    g_hash_table_remove(identities, session->account.identity);
					    g_free(session->account.identity);
					    session->account.identity = NULL;
					}
					session->account.sips = TRUE;
					if(session->account.proxy) {
					    g_free(session->account.proxy);
					    session->account.proxy = NULL;
					}
					if(session->account.secret) {
					    g_free(session->account.secret);
					    session->account.secret = NULL;
					}
					if(session->account.username) {
					    g_free(session->account.username);
					    session->account.username = NULL;
					}
					if(session->account.display_name) {
					    g_free(session->account.display_name);
					    session->account.display_name = NULL;
					}
					if(session->account.user_agent) {
					    g_free(session->account.user_agent);
					    session->account.user_agent = NULL;
					}
					if(session->account.authuser) {
					    g_free(session->account.authuser);
					    session->account.authuser = NULL;
					}
					if(session->callee) {
					    g_free(session->callee);
					    session->callee = NULL;
					}
					if(session->callid) {
					    g_hash_table_remove(callids, session->callid);
					    g_free(session->callid);
					    session->callid = NULL;
					}
					if(session->sdp) {
					    janus_sdp_free(session->sdp);
					    session->sdp = NULL;
					}
					if(session->transaction) {
					    g_free(session->transaction);
					    session->transaction = NULL;
					}
					if(session->media.remote_ip) {
					    g_free(session->media.remote_ip);
					    session->media.remote_ip = NULL;
					}
					janus_sipre_srtp_cleanup(session);
					session->handle = NULL;
					g_free(session);
					session = NULL;
					continue;
				}
				sl = sl->next;
			}
		}
		janus_mutex_unlock(&sessions_mutex);
		g_usleep(500000);
	}
	JANUS_LOG(LOG_INFO, "SIPre watchdog stopped\n");
	return NULL;
}


/* Random string helper (for call-ids) */
static char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static void janus_sipre_random_string(int length, char *buffer) {
	if(length > 0 && buffer) {
		int l = (int)(sizeof(charset)-1);
		int i=0;
		for(i=0; i<length; i++) {
			int key = rand() % l;
			buffer[i] = charset[key];
		}
		buffer[length-1] = '\0';
	}
}


/* Plugin implementation */
int janus_sipre_init(janus_callbacks *callback, const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_SIPRE_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config != NULL) {
		janus_config_print(config);

		janus_config_item *item = janus_config_get_item_drilldown(config, "general", "local_ip");
		if(item && item->value) {
			/* Verify that the address is valid */
			struct ifaddrs *ifas = NULL;
			janus_network_address iface;
			janus_network_address_string_buffer ibuf;
			if(getifaddrs(&ifas) || ifas == NULL) {
				JANUS_LOG(LOG_ERR, "Unable to acquire list of network devices/interfaces; some configurations may not work as expected...\n");
			} else {
				if(janus_network_lookup_interface(ifas, item->value, &iface) != 0) {
					JANUS_LOG(LOG_WARN, "Error setting local IP address to %s, falling back to detecting IP address...\n", item->value);
				} else {
					if(janus_network_address_to_string_buffer(&iface, &ibuf) != 0 || janus_network_address_string_buffer_is_null(&ibuf)) {
						JANUS_LOG(LOG_WARN, "Error getting local IP address from %s, falling back to detecting IP address...\n", item->value);
					} else {
						local_ip = g_strdup(janus_network_address_string_from_buffer(&ibuf));
					}
				}
			}
		}

		item = janus_config_get_item_drilldown(config, "general", "keepalive_interval");
		if(item && item->value) {
			keepalive_interval = atoi(item->value);
		}
		JANUS_LOG(LOG_VERB, "SIPre keep-alive interval set to %d seconds\n", keepalive_interval);

		item = janus_config_get_item_drilldown(config, "general", "register_ttl");
		if(item && item->value) {
			register_ttl = atoi(item->value);
		}
		JANUS_LOG(LOG_VERB, "SIPre registration TTL set to %d seconds\n", register_ttl);

		item = janus_config_get_item_drilldown(config, "general", "behind_nat");
		if(item && item->value) {
			behind_nat = janus_is_true(item->value);
		}

		item = janus_config_get_item_drilldown(config, "general", "user_agent");
		if(item && item->value) {
			user_agent = g_strdup(item->value);
		} else {
			user_agent = g_strdup("Janus WebRTC Gateway SIPre Plugin "JANUS_SIPRE_VERSION_STRING);
		}
		JANUS_LOG(LOG_VERB, "SIPre User-Agent set to %s\n", user_agent);

		item = janus_config_get_item_drilldown(config, "general", "events");
		if(item != NULL && item->value != NULL) {
			notify_events = janus_is_true(item->value);
		}
		if(!notify_events && callback->events_is_enabled()) {
			JANUS_LOG(LOG_WARN, "Notification of events to handlers disabled for %s\n", JANUS_SIPRE_NAME);
		}

		janus_config_destroy(config);
	}
	config = NULL;

	if(local_ip == NULL) {
		local_ip = janus_network_detect_local_ip_as_string(janus_network_query_options_any_ip);
		if(local_ip == NULL) {
			JANUS_LOG(LOG_WARN, "Couldn't find any address! using 127.0.0.1 as the local IP... (which is NOT going to work out of your machine)\n");
			local_ip = g_strdup("127.0.0.1");
		}
	}
	JANUS_LOG(LOG_VERB, "Local IP set to %s\n", local_ip);

#ifdef HAVE_SRTP_2
	/* Init randomizer (for randum numbers in SRTP) */
	RAND_poll();
#endif

	/* Setup libre */
	int err = libre_init();
	if(err) {
		JANUS_LOG(LOG_ERR, "libre_init() failed: %d (%s)\n", err, strerror(err));
		return -1;
	}
	err = sip_alloc(&sipstack, NULL, 32, 32, 32, JANUS_SIPRE_NAME, janus_sipre_cb_exit, NULL);
	if(err) {
		JANUS_LOG(LOG_ERR, "Failed to initialize libre SIP stack: %d (%s)\n", err, strerror(err));
		return -1;
	}
	err = mqueue_alloc(&mq, janus_sipre_mqueue_handler, NULL);
	if(err) {
		mem_deref(sipstack);
		JANUS_LOG(LOG_ERR, "Failed to initialize message queue: %d (%s)\n", err, strerror(err));
		return -1;
	}
	/* We initialize in the loop */
	mqueue_push(mq, janus_sipre_mqueue_event_do_init, NULL);

	sessions = g_hash_table_new(NULL, NULL);
	callids = g_hash_table_new(g_str_hash, g_str_equal);
	identities = g_hash_table_new(g_str_hash, g_str_equal);
	janus_mutex_init(&sessions_mutex);
	messages = g_async_queue_new_full((GDestroyNotify) janus_sipre_message_free);
	/* This is the callback we'll need to invoke to contact the gateway */
	gateway = callback;

	g_atomic_int_set(&initialized, 1);

	GError *error = NULL;
	/* Start the sessions watchdog */
	watchdog = g_thread_try_new("sipre watchdog", &janus_sipre_watchdog, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the SIPre watchdog thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	/* Launch the thread that will handle incoming API messages */
	handler_thread = g_thread_try_new("sipre handler", janus_sipre_handler, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the SIPre handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	/* Launch the thread that will handle the libre event loop */
	sipstack_thread = g_thread_try_new("sipre loop", janus_sipre_stack_thread, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the SIPre loop thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_SIPRE_NAME);
	return 0;
}

void janus_sipre_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}

	if(sipstack_thread != NULL) {
		g_thread_join(sipstack_thread);
		sipstack_thread = NULL;
	}
	if(watchdog != NULL) {
		g_thread_join(watchdog);
		watchdog = NULL;
	}
	/* FIXME We should destroy the sessions cleanly */
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	g_hash_table_destroy(callids);
	g_hash_table_destroy(identities);
	sessions = NULL;
	callids = NULL;
	identities = NULL;
	janus_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);

	/* Deinitialize libre */
	libre_close();
	tmr_debug();
	mem_debug();

	g_free(local_ip);

	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_SIPRE_NAME);
}

int janus_sipre_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_sipre_get_version(void) {
	return JANUS_SIPRE_VERSION;
}

const char *janus_sipre_get_version_string(void) {
	return JANUS_SIPRE_VERSION_STRING;
}

const char *janus_sipre_get_description(void) {
	return JANUS_SIPRE_DESCRIPTION;
}

const char *janus_sipre_get_name(void) {
	return JANUS_SIPRE_NAME;
}

const char *janus_sipre_get_author(void) {
	return JANUS_SIPRE_AUTHOR;
}

const char *janus_sipre_get_package(void) {
	return JANUS_SIPRE_PACKAGE;
}

void janus_sipre_create_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_sipre_session *session = g_malloc0(sizeof(janus_sipre_session));
	session->handle = handle;
	session->account.identity = NULL;
	session->account.sips = TRUE;
	session->account.username = NULL;
	session->account.display_name = NULL;
	session->account.user_agent = NULL;
	session->account.authuser = NULL;
	session->account.secret = NULL;
	session->account.secret_type = janus_sipre_secret_type_unknown;
	session->account.sip_port = 0;
	session->account.proxy = NULL;
	session->account.registration_status = janus_sipre_registration_status_unregistered;
	session->status = janus_sipre_call_status_idle;
	memset(&session->stack, 0, sizeof(janus_sipre_stack));
	session->transaction = NULL;
	session->callee = NULL;
	session->callid = NULL;
	session->sdp = NULL;
	session->media.remote_ip = NULL;
	session->media.ready = 0;
	session->media.autoack = TRUE;
	session->media.require_srtp = FALSE;
	session->media.has_srtp_local = FALSE;
	session->media.has_srtp_remote = FALSE;
	session->media.has_audio = 0;
	session->media.audio_rtp_fd = -1;
	session->media.audio_rtcp_fd= -1;
	session->media.local_audio_rtp_port = 0;
	session->media.remote_audio_rtp_port = 0;
	session->media.local_audio_rtcp_port = 0;
	session->media.remote_audio_rtcp_port = 0;
	session->media.audio_ssrc = 0;
	session->media.audio_ssrc_peer = 0;
	session->media.audio_pt = -1;
	session->media.audio_pt_name = NULL;
	session->media.audio_srtp_suite_in = 0;
	session->media.audio_srtp_suite_out = 0;
	session->media.audio_send = TRUE;
	session->media.has_video = 0;
	session->media.video_rtp_fd = -1;
	session->media.video_rtcp_fd= -1;
	session->media.local_video_rtp_port = 0;
	session->media.remote_video_rtp_port = 0;
	session->media.local_video_rtcp_port = 0;
	session->media.remote_video_rtcp_port = 0;
	session->media.video_ssrc = 0;
	session->media.video_ssrc_peer = 0;
	session->media.video_pt = -1;
	session->media.video_pt_name = NULL;
	session->media.video_srtp_suite_in = 0;
	session->media.video_srtp_suite_out = 0;
	session->media.video_send = TRUE;
	/* Initialize the RTP context */
	janus_rtp_switching_context_reset(&session->media.context);
	session->media.pipefd[0] = -1;
	session->media.pipefd[1] = -1;
	session->media.updated = FALSE;
	janus_mutex_init(&session->rec_mutex);
	session->destroyed = 0;
	g_atomic_int_set(&session->hangingup, 0);
	janus_mutex_init(&session->mutex);
	handle->plugin_handle = session;

	int err = sipsess_listen(&session->stack.sess_sock, sipstack, 32, janus_sipre_cb_established, session);
	if(err < 0) {
		/* TODO Anything we should do? */
		JANUS_LOG(LOG_ERR, "Error listening: %d (%s)\n", err, strerror(err));
	}

	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
	janus_mutex_unlock(&sessions_mutex);


	return;
}

void janus_sipre_destroy_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}
	janus_sipre_session *session = (janus_sipre_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No SIPre session associated with this handle...\n");
		*error = -2;
		return;
	}
	janus_mutex_lock(&sessions_mutex);
	if(!session->destroyed) {
		g_hash_table_remove(sessions, handle);
		janus_sipre_hangup_media(handle);
		session->destroyed = janus_get_monotonic_time();
		JANUS_LOG(LOG_VERB, "Destroying SIPre session (%s)...\n", session->account.username ? session->account.username : "unregistered user");
		/* TODO Destroy re-related stuff for this SIP session */

		/* Cleaning up and removing the session is done in a lazy way */
		old_sessions = g_list_append(old_sessions, session);
	}
	janus_mutex_unlock(&sessions_mutex);
	return;
}

json_t *janus_sipre_query_session(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}
	janus_sipre_session *session = (janus_sipre_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	/* Provide some generic info, e.g., if we're in a call and with whom */
	json_t *info = json_object();
	json_object_set_new(info, "username", session->account.username ? json_string(session->account.username) : NULL);
	json_object_set_new(info, "display_name", session->account.display_name ? json_string(session->account.display_name) : NULL);
	json_object_set_new(info, "user_agent", session->account.user_agent ? json_string(session->account.user_agent) : NULL);
	json_object_set_new(info, "identity", session->account.identity ? json_string(session->account.identity) : NULL);
	json_object_set_new(info, "registration_status", json_string(janus_sipre_registration_status_string(session->account.registration_status)));
	json_object_set_new(info, "call_status", json_string(janus_sipre_call_status_string(session->status)));
	if(session->callee) {
		json_object_set_new(info, "callee", json_string(session->callee ? session->callee : "??"));
		json_object_set_new(info, "auto-ack", json_string(session->media.autoack ? "yes" : "no"));
		json_object_set_new(info, "srtp-required", json_string(session->media.require_srtp ? "yes" : "no"));
		json_object_set_new(info, "sdes-local", json_string(session->media.has_srtp_local ? "yes" : "no"));
		json_object_set_new(info, "sdes-remote", json_string(session->media.has_srtp_remote ? "yes" : "no"));
	}
	if(session->arc || session->vrc || session->arc_peer || session->vrc_peer) {
		json_t *recording = json_object();
		if(session->arc && session->arc->filename)
			json_object_set_new(recording, "audio", json_string(session->arc->filename));
		if(session->vrc && session->vrc->filename)
			json_object_set_new(recording, "video", json_string(session->vrc->filename));
		if(session->arc_peer && session->arc_peer->filename)
			json_object_set_new(recording, "audio-peer", json_string(session->arc_peer->filename));
		if(session->vrc_peer && session->vrc_peer->filename)
			json_object_set_new(recording, "video-peer", json_string(session->vrc_peer->filename));
		json_object_set_new(info, "recording", recording);
	}
	json_object_set_new(info, "destroyed", json_integer(session->destroyed));
	return info;
}

struct janus_plugin_result *janus_sipre_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);
	janus_sipre_message *msg = g_malloc0(sizeof(janus_sipre_message));
	msg->handle = handle;
	msg->transaction = transaction;
	msg->message = message;
	msg->jsep = jsep;
	g_async_queue_push(messages, msg);

	/* All the requests to this plugin are handled asynchronously */
	return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, NULL, NULL);
}

void janus_sipre_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "WebRTC media is now available\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_sipre_session *session = (janus_sipre_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(session->destroyed)
		return;
	g_atomic_int_set(&session->hangingup, 0);
	/* TODO Only relay RTP/RTCP when we get this event */
}

void janus_sipre_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if(gateway) {
		/* Honour the audio/video active flags */
		janus_sipre_session *session = (janus_sipre_session *)handle->plugin_handle;
		if(!session || session->destroyed) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		/* Forward to our SIPre peer */
		if((video && !session->media.video_send) || (!video && !session->media.audio_send)) {
			/* Dropping packet, peer doesn't want to receive it */
			return;
		}
		if((video && session->media.video_ssrc == 0) || (!video && session->media.audio_ssrc == 0)) {
			rtp_header *header = (rtp_header *)buf;
			if(video) {
				session->media.video_ssrc = ntohl(header->ssrc);
			} else {
				session->media.audio_ssrc = ntohl(header->ssrc);
			}
			JANUS_LOG(LOG_VERB, "[SIPre-%s] Got SIPre %s SSRC: %"SCNu32"\n",
				session->account.username ? session->account.username : "unknown",
				video ? "video" : "audio",
				video ? session->media.video_ssrc : session->media.audio_ssrc);
		}
		if((video && session->media.has_video && session->media.video_rtp_fd) ||
				(!video && session->media.has_audio && session->media.audio_rtp_fd)) {
			/* Save the frame if we're recording */
			janus_recorder_save_frame(video ? session->vrc : session->arc, buf, len);
			/* Is SRTP involved? */
			if(session->media.has_srtp_local) {
				char sbuf[2048];
				memcpy(&sbuf, buf, len);
				int protected = len;
				int res = srtp_protect(
					(video ? session->media.video_srtp_out : session->media.audio_srtp_out),
					&sbuf, &protected);
				if(res != srtp_err_status_ok) {
					rtp_header *header = (rtp_header *)&sbuf;
					guint32 timestamp = ntohl(header->timestamp);
					guint16 seq = ntohs(header->seq_number);
					JANUS_LOG(LOG_ERR, "[SIPre-%s] %s SRTP protect error... %s (len=%d-->%d, ts=%"SCNu32", seq=%"SCNu16")...\n",
						session->account.username ? session->account.username : "unknown",
						video ? "Video" : "Audio", janus_srtp_error_str(res), len, protected, timestamp, seq);
				} else {
					/* Forward the frame to the peer */
					send((video ? session->media.video_rtp_fd : session->media.audio_rtp_fd), sbuf, protected, 0);
				}
			} else {
				/* Forward the frame to the peer */
				send((video ? session->media.video_rtp_fd : session->media.audio_rtp_fd), buf, len, 0);
			}
		}
	}
}

void janus_sipre_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	if(gateway) {
		janus_sipre_session *session = (janus_sipre_session *)handle->plugin_handle;
		if(!session || session->destroyed) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		/* Forward to our SIPre peer */
		if((video && session->media.has_video && session->media.video_rtcp_fd) ||
				(!video && session->media.has_audio && session->media.audio_rtcp_fd)) {
			/* Fix SSRCs as the gateway does */
			JANUS_LOG(LOG_HUGE, "[SIPre-%s] Fixing %s SSRCs (local %u, peer %u)\n",
				session->account.username ? session->account.username : "unknown",
				video ? "video" : "audio",
				(video ? session->media.video_ssrc : session->media.audio_ssrc),
				(video ? session->media.video_ssrc_peer : session->media.audio_ssrc_peer));
			janus_rtcp_fix_ssrc(NULL, (char *)buf, len, video,
				(video ? session->media.video_ssrc : session->media.audio_ssrc),
				(video ? session->media.video_ssrc_peer : session->media.audio_ssrc_peer));
			/* Is SRTP involved? */
			if(session->media.has_srtp_local) {
				char sbuf[2048];
				memcpy(&sbuf, buf, len);
				int protected = len;
				int res = srtp_protect_rtcp(
					(video ? session->media.video_srtp_out : session->media.audio_srtp_out),
					&sbuf, &protected);
				if(res != srtp_err_status_ok) {
					JANUS_LOG(LOG_ERR, "[SIPre-%s] %s SRTCP protect error... %s (len=%d-->%d)...\n",
						session->account.username ? session->account.username : "unknown",
						video ? "Video" : "Audio",
						janus_srtp_error_str(res), len, protected);
				} else {
					/* Forward the message to the peer */
					send((video ? session->media.video_rtcp_fd : session->media.audio_rtcp_fd), sbuf, protected, 0);
				}
			} else {
				/* Forward the message to the peer */
				send((video ? session->media.video_rtcp_fd : session->media.audio_rtcp_fd), buf, len, 0);
			}
		}
	}
}

void janus_sipre_hangup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "No WebRTC media anymore\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_sipre_session *session = (janus_sipre_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(session->destroyed)
		return;
	if(g_atomic_int_add(&session->hangingup, 1))
		return;
	if(!(session->status == janus_sipre_call_status_inviting ||
		 session->status == janus_sipre_call_status_invited ||
		 session->status == janus_sipre_call_status_incall))
		return;
	/* Get rid of the recorders, if available */
	janus_mutex_lock(&session->rec_mutex);
	if(session->arc) {
		janus_recorder_close(session->arc);
		JANUS_LOG(LOG_INFO, "Closed user's audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
		janus_recorder_free(session->arc);
	}
	session->arc = NULL;
	if(session->arc_peer) {
		janus_recorder_close(session->arc_peer);
		JANUS_LOG(LOG_INFO, "Closed peer's audio recording %s\n", session->arc_peer->filename ? session->arc_peer->filename : "??");
		janus_recorder_free(session->arc_peer);
	}
	session->arc_peer = NULL;
	if(session->vrc) {
		janus_recorder_close(session->vrc);
		JANUS_LOG(LOG_INFO, "Closed user's video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
		janus_recorder_free(session->vrc);
	}
	session->vrc = NULL;
	if(session->vrc_peer) {
		janus_recorder_close(session->vrc_peer);
		JANUS_LOG(LOG_INFO, "Closed peer's video recording %s\n", session->vrc_peer->filename ? session->vrc_peer->filename : "??");
		janus_recorder_free(session->vrc_peer);
	}
	session->vrc_peer = NULL;
	janus_mutex_unlock(&session->rec_mutex);
	/* FIXME Simulate a "hangup" coming from the browser */
	janus_sipre_message *msg = g_malloc0(sizeof(janus_sipre_message));
	msg->handle = handle;
	msg->message = json_pack("{ss}", "request", "hangup");
	msg->transaction = NULL;
	msg->jsep = NULL;
	g_async_queue_push(messages, msg);
}

/* Thread to handle incoming messages */
static void *janus_sipre_handler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining SIPre handler thread\n");
	janus_sipre_message *msg = NULL;
	int error_code = 0;
	char error_cause[512];
	json_t *root = NULL;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);
		if(msg == NULL)
			continue;
		if(msg == &exit_message)
			break;
		if(msg->handle == NULL) {
			janus_sipre_message_free(msg);
			continue;
		}
		janus_sipre_session *session = NULL;
		janus_mutex_lock(&sessions_mutex);
		if(g_hash_table_lookup(sessions, msg->handle) != NULL ) {
			session = (janus_sipre_session *)msg->handle->plugin_handle;
		}
		janus_mutex_unlock(&sessions_mutex);
		if(!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			janus_sipre_message_free(msg);
			continue;
		}
		if(session->destroyed) {
			janus_sipre_message_free(msg);
			continue;
		}
		/* Handle request */
		error_code = 0;
		root = msg->message;
		if(msg->message == NULL) {
			JANUS_LOG(LOG_ERR, "No message??\n");
			error_code = JANUS_SIPRE_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s", "No message??");
			goto error;
		}
		if(!json_is_object(root)) {
			JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
			error_code = JANUS_SIPRE_ERROR_INVALID_JSON;
			g_snprintf(error_cause, 512, "JSON error: not an object");
			goto error;
		}
		JANUS_VALIDATE_JSON_OBJECT(root, request_parameters,
			error_code, error_cause, TRUE,
			JANUS_SIPRE_ERROR_MISSING_ELEMENT, JANUS_SIPRE_ERROR_INVALID_ELEMENT);
		if(error_code != 0)
			goto error;
		json_t *request = json_object_get(root, "request");
		const char *request_text = json_string_value(request);
		json_t *result = NULL;

		if(!strcasecmp(request_text, "register")) {
			/* Send a REGISTER */
			if(session->account.registration_status > janus_sipre_registration_status_unregistered) {
				JANUS_LOG(LOG_ERR, "Already registered (%s)\n", session->account.username);
				error_code = JANUS_SIPRE_ERROR_ALREADY_REGISTERED;
				g_snprintf(error_cause, 512, "Already registered (%s)", session->account.username);
				goto error;
			}

			/* Cleanup old values */
			if(session->account.identity != NULL) {
				g_hash_table_remove(identities, session->account.identity);
				g_free(session->account.identity);
			}
			session->account.identity = NULL;
			session->account.sips = TRUE;
			if(session->account.username != NULL)
				g_free(session->account.username);
			session->account.username = NULL;
			if(session->account.display_name != NULL)
				g_free(session->account.display_name);
			session->account.display_name = NULL;
			if(session->account.authuser != NULL)
				g_free(session->account.authuser);
			session->account.authuser = NULL;
			if(session->account.secret != NULL)
				g_free(session->account.secret);
			session->account.secret = NULL;
			session->account.secret_type = janus_sipre_secret_type_unknown;
			if(session->account.proxy != NULL)
				g_free(session->account.proxy);
			session->account.proxy = NULL;
			if(session->account.user_agent != NULL)
				g_free(session->account.user_agent);
			session->account.user_agent = NULL;
			session->account.registration_status = janus_sipre_registration_status_unregistered;

			gboolean guest = FALSE;
			JANUS_VALIDATE_JSON_OBJECT(root, register_parameters,
				error_code, error_cause, TRUE,
				JANUS_SIPRE_ERROR_MISSING_ELEMENT, JANUS_SIPRE_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			json_t *type = json_object_get(root, "type");
			if(type != NULL) {
				const char *type_text = json_string_value(type);
				if(!strcmp(type_text, "guest")) {
					JANUS_LOG(LOG_INFO, "Registering as a guest\n");
					guest = TRUE;
				} else {
					JANUS_LOG(LOG_WARN, "Unknown type '%s', ignoring...\n", type_text);
				}
			}

			gboolean send_register = TRUE;
			json_t *do_register = json_object_get(root, "send_register");
			if(do_register != NULL) {
				if(guest) {
					JANUS_LOG(LOG_ERR, "Conflicting elements: send_register cannot be true if guest is true\n");
					error_code = JANUS_SIPRE_ERROR_INVALID_ELEMENT;
					g_snprintf(error_cause, 512, "Conflicting elements: send_register cannot be true if guest is true");
					goto error;
				}
				send_register = json_is_true(do_register);
			}

			gboolean sips = TRUE;
			json_t *do_sipres = json_object_get(root, "sips");
			if(do_sipres != NULL) {
				sips = json_is_true(do_sipres);
			}

			/* Parse address */
			json_t *proxy = json_object_get(root, "proxy");
			const char *proxy_text = NULL;
			if(proxy && !json_is_null(proxy)) {
				/* Has to be validated separately because it could be null */
				JANUS_VALIDATE_JSON_OBJECT(root, proxy_parameters,
					error_code, error_cause, TRUE,
					JANUS_SIPRE_ERROR_MISSING_ELEMENT, JANUS_SIPRE_ERROR_INVALID_ELEMENT);
				if(error_code != 0)
					goto error;
				proxy_text = json_string_value(proxy);
				if(janus_sipre_parse_uri(proxy_text) < 0) {
					JANUS_LOG(LOG_ERR, "Invalid proxy address %s\n", proxy_text);
					error_code = JANUS_SIPRE_ERROR_INVALID_ADDRESS;
					g_snprintf(error_cause, 512, "Invalid proxy address %s\n", proxy_text);
					goto error;
				}
			}

			/* Parse register TTL */
			int ttl = register_ttl;
			json_t *reg_ttl = json_object_get(root, "register_ttl");
			if(reg_ttl && json_is_integer(reg_ttl))
				ttl = json_integer_value(reg_ttl);
			if(ttl <= 0)
				ttl = JANUS_DEFAULT_REGISTER_TTL;

			/* Parse display name */
			const char* display_name_text = NULL;
			json_t *display_name = json_object_get(root, "display_name");
			if(display_name && json_is_string(display_name))
				display_name_text = json_string_value(display_name);

			/* Parse user agent */
			const char* user_agent_text = NULL;
			json_t *user_agent = json_object_get(root, "user_agent");
			if(user_agent && json_is_string(user_agent))
				user_agent_text = json_string_value(user_agent);

			/* Now the user part, if needed */
			json_t *username = json_object_get(root, "username");
			if(!guest && !username) {
				/* The username is mandatory if we're not registering as guests */
				JANUS_LOG(LOG_ERR, "Missing element (username)\n");
				error_code = JANUS_SIPRE_ERROR_MISSING_ELEMENT;
				g_snprintf(error_cause, 512, "Missing element (username)");
				goto error;
			}
			const char *username_text = NULL;
			char *user_id = NULL, *user_host = NULL;
			guint16 user_port = 0;
			if(username) {
				/* Parse address */
				username_text = json_string_value(username);
				if(janus_sipre_parse_uri(username_text) < 0) {
					JANUS_LOG(LOG_ERR, "Invalid user address %s\n", username_text);
					error_code = JANUS_SIPRE_ERROR_INVALID_ADDRESS;
					g_snprintf(error_cause, 512, "Invalid user address %s\n", username_text);
					goto error;
				}
				user_id = janus_sipre_get_uri_username(username_text);
				user_host = janus_sipre_get_uri_host(username_text);
				user_port = janus_sipre_get_uri_port(username_text);
			}
			if(guest) {
				/* Not needed, we can stop here: just pick a random username if it wasn't provided and say we're registered */
				if(!username)
					g_snprintf(user_id, 255, "janus-sipre-%"SCNu32"", janus_random_uint32());
				JANUS_LOG(LOG_INFO, "Guest will have username %s\n", user_id);
				send_register = FALSE;
			} else {
				json_t *secret = json_object_get(root, "secret");
				json_t *ha1_secret = json_object_get(root, "ha1_secret");
				json_t *authuser = json_object_get(root, "authuser");
				if(!secret && !ha1_secret) {
					g_free(user_id);
					g_free(user_host);
					JANUS_LOG(LOG_ERR, "Missing element (secret or ha1_secret)\n");
					error_code = JANUS_SIPRE_ERROR_MISSING_ELEMENT;
					g_snprintf(error_cause, 512, "Missing element (secret or ha1_secret)");
					goto error;
				}
				if(secret && ha1_secret) {
					g_free(user_id);
					g_free(user_host);
					JANUS_LOG(LOG_ERR, "Conflicting elements specified (secret and ha1_secret)\n");
					error_code = JANUS_SIPRE_ERROR_INVALID_ELEMENT;
					g_snprintf(error_cause, 512, "Conflicting elements specified (secret and ha1_secret)");
					goto error;
				}
				const char *secret_text;
				if(secret) {
					secret_text = json_string_value(secret);
					session->account.secret = g_strdup(secret_text);
					session->account.secret_type = janus_sipre_secret_type_plaintext;
				} else {
					secret_text = json_string_value(ha1_secret);
					session->account.secret = g_strdup(secret_text);
					session->account.secret_type = janus_sipre_secret_type_hashed;
				}
				if(authuser) {
					const char *authuser_text;
					authuser_text = json_string_value(authuser);
					session->account.authuser = g_strdup(authuser_text);
				} else {
					session->account.authuser = g_strdup(user_id);
				}
				/* Got the values, try registering now */
				JANUS_LOG(LOG_VERB, "Registering user %s (secret %s) @ %s through %s\n",
					user_id, secret_text, user_host, proxy_text != NULL ? proxy_text : "(null)");
			}

			session->account.identity = g_strdup(username_text);
			g_hash_table_insert(identities, session->account.identity, session);
			session->account.sips = sips;
			session->account.username = g_strdup(user_id);
			if(display_name_text) {
				session->account.display_name = g_strdup(display_name_text);
			}
			if(user_agent_text) {
				session->account.user_agent = g_strdup(user_agent_text);
			}
			if(proxy_text) {
				session->account.proxy = g_strdup(proxy_text);
			} else {
				/* Build one from the user's identity */
				char uri[256];
				g_snprintf(uri, sizeof(uri), "sip:%s:%"SCNu16, user_host, (user_port ? user_port : 5060));
				session->account.proxy = g_strdup(uri);
			}
			g_free(user_host);
			g_free(user_id);

			session->account.registration_status = janus_sipre_registration_status_registering;
			if(send_register) {
				char ttl_text[20];
				g_snprintf(ttl_text, sizeof(ttl_text), "%d", ttl);
					/* TODO Any way to specify TTL in sipreg_register? */
				/* We enqueue this REGISTER attempt, to be sure it's done in the re_main loop thread
				 * FIXME Maybe passing a key to the session is better than passing the session object
				 * itself? it may be gone when it gets handled... won't be an issue with the
				 * reference counter branch but needs to be taken into account until then */
				mqueue_push(mq, janus_sipre_mqueue_event_do_register, session);
				result = json_object();
				json_object_set_new(result, "event", json_string("registering"));
			} else {
				JANUS_LOG(LOG_VERB, "Not sending a SIPre REGISTER: either send_register was set to false or guest mode was enabled\n");
				session->account.registration_status = janus_sipre_registration_status_disabled;
				result = json_object();
				json_object_set_new(result, "event", json_string("registered"));
				json_object_set_new(result, "username", json_string(session->account.username));
				json_object_set_new(result, "register_sent", json_false());
				/* Also notify event handlers */
				if(notify_events && gateway->events_is_enabled()) {
					json_t *info = json_object();
					json_object_set_new(info, "event", json_string("registered"));
					json_object_set_new(info, "identity", json_string(session->account.identity));
					json_object_set_new(info, "type", json_string("guest"));
					gateway->notify_event(&janus_sipre_plugin, session->handle, info);
				}
			}
		} else if(!strcasecmp(request_text, "call")) {
			/* Call another peer */
			//~ if(session->stack == NULL) {
				//~ JANUS_LOG(LOG_ERR, "Wrong state (register first)\n");
				//~ error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				//~ g_snprintf(error_cause, 512, "Wrong state (register first)");
				//~ goto error;
			//~ }
			if(session->status >= janus_sipre_call_status_inviting) {
				JANUS_LOG(LOG_ERR, "Wrong state (already in a call? status=%s)\n", janus_sipre_call_status_string(session->status));
				error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Wrong state (already in a call? status=%s)", janus_sipre_call_status_string(session->status));
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, call_parameters,
				error_code, error_cause, TRUE,
				JANUS_SIPRE_ERROR_MISSING_ELEMENT, JANUS_SIPRE_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			json_t *uri = json_object_get(root, "uri");
			/* Check if we need to ACK manually (e.g., for the Record-Route hack) */
			json_t *autoack = json_object_get(root, "autoack");
			gboolean do_autoack = autoack ? json_is_true(autoack) : TRUE;
			/* Check if the INVITE needs to be enriched with custom headers */
			char custom_headers[2048];
			custom_headers[0] = '\0';
			json_t *headers = json_object_get(root, "headers");
			if(headers) {
				if(json_object_size(headers) > 0) {
					/* Parse custom headers */
					const char *key = NULL;
					json_t *value = NULL;
					void *iter = json_object_iter(headers);
					while(iter != NULL) {
						key = json_object_iter_key(iter);
						value = json_object_get(headers, key);
						if(value == NULL || !json_is_string(value)) {
							JANUS_LOG(LOG_WARN, "Skipping header '%s': value is not a string\n", key);
							iter = json_object_iter_next(headers, iter);
							continue;
						}
						char h[255];
						g_snprintf(h, 255, "%s: %s\r\n", key, json_string_value(value));
						JANUS_LOG(LOG_VERB, "Adding custom header, %s", h);
						g_strlcat(custom_headers, h, 2048);
						iter = json_object_iter_next(headers, iter);
					}
				}
			}
			/* SDES-SRTP is disabled by default, let's see if we need to enable it */
			gboolean offer_srtp = FALSE, require_srtp = FALSE;
			json_t *srtp = json_object_get(root, "srtp");
			if(srtp) {
				const char *srtp_text = json_string_value(srtp);
				if(!strcasecmp(srtp_text, "sdes_optional")) {
					/* Negotiate SDES, but make it optional */
					offer_srtp = TRUE;
				} else if(!strcasecmp(srtp_text, "sdes_mandatory")) {
					/* Negotiate SDES, and require it */
					offer_srtp = TRUE;
					require_srtp = TRUE;
				} else {
					JANUS_LOG(LOG_ERR, "Invalid element (srtp can only be sdes_optional or sdes_mandatory)\n");
					error_code = JANUS_SIPRE_ERROR_INVALID_ELEMENT;
					g_snprintf(error_cause, 512, "Invalid element (srtp can only be sdes_optional or sdes_mandatory)");
					goto error;
				}
			}
			/* Parse address */
			const char *uri_text = json_string_value(uri);
			if(janus_sipre_parse_uri(uri_text) < 0) {
				JANUS_LOG(LOG_ERR, "Invalid user address %s\n", uri_text);
				error_code = JANUS_SIPRE_ERROR_INVALID_ADDRESS;
				g_snprintf(error_cause, 512, "Invalid user address %s\n", uri_text);
				goto error;
			}
			/* Any SDP to handle? if not, something's wrong */
			const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
			const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
			if(!msg_sdp) {
				JANUS_LOG(LOG_ERR, "Missing SDP\n");
				error_code = JANUS_SIPRE_ERROR_MISSING_SDP;
				g_snprintf(error_cause, 512, "Missing SDP");
				goto error;
			}
			if(strstr(msg_sdp, "m=application")) {
				JANUS_LOG(LOG_ERR, "The SIPre plugin does not support DataChannels\n");
				error_code = JANUS_SIPRE_ERROR_MISSING_SDP;
				g_snprintf(error_cause, 512, "The SIPre plugin does not support DataChannels");
				goto error;
			}
			JANUS_LOG(LOG_VERB, "%s is calling %s\n", session->account.username, uri_text);
			JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg_sdp_type, msg_sdp);
			/* Clean up SRTP stuff from before first, in case it's still needed */
			janus_sipre_srtp_cleanup(session);
			session->media.require_srtp = require_srtp;
			session->media.has_srtp_local = offer_srtp;
			if(offer_srtp) {
				JANUS_LOG(LOG_VERB, "Going to negotiate SDES-SRTP (%s)...\n", require_srtp ? "mandatory" : "optional");
			}
			/* Parse the SDP we got, manipulate some things, and generate a new one */
			char sdperror[100];
			janus_sdp *parsed_sdp = janus_sdp_parse(msg_sdp, sdperror, sizeof(sdperror));
			if(!parsed_sdp) {
				JANUS_LOG(LOG_ERR, "Error parsing SDP: %s\n", sdperror);
				error_code = JANUS_SIPRE_ERROR_MISSING_SDP;
				g_snprintf(error_cause, 512, "Error parsing SDP: %s", sdperror);
				goto error;
			}
			/* Allocate RTP ports and merge them with the anonymized SDP */
			if(strstr(msg_sdp, "m=audio") && !strstr(msg_sdp, "m=audio 0")) {
				JANUS_LOG(LOG_VERB, "Going to negotiate audio...\n");
				session->media.has_audio = 1;	/* FIXME Maybe we need a better way to signal this */
			}
			if(strstr(msg_sdp, "m=video") && !strstr(msg_sdp, "m=video 0")) {
				JANUS_LOG(LOG_VERB, "Going to negotiate video...\n");
				session->media.has_video = 1;	/* FIXME Maybe we need a better way to signal this */
			}
			if(janus_sipre_allocate_local_ports(session) < 0) {
				JANUS_LOG(LOG_ERR, "Could not allocate RTP/RTCP ports\n");
				janus_sdp_free(parsed_sdp);
				error_code = JANUS_SIPRE_ERROR_IO_ERROR;
				g_snprintf(error_cause, 512, "Could not allocate RTP/RTCP ports");
				goto error;
			}
			char *sdp = janus_sipre_sdp_manipulate(session, parsed_sdp, FALSE);
			if(sdp == NULL) {
				JANUS_LOG(LOG_ERR, "Could not allocate RTP/RTCP ports\n");
				janus_sdp_free(parsed_sdp);
				error_code = JANUS_SIPRE_ERROR_IO_ERROR;
				g_snprintf(error_cause, 512, "Could not allocate RTP/RTCP ports");
				goto error;
			}
			/* Take note of the SDP (may be useful for UPDATEs or re-INVITEs) */
			janus_sdp_free(session->sdp);
			session->sdp = parsed_sdp;
			JANUS_LOG(LOG_VERB, "Prepared SDP for INVITE:\n%s", sdp);
			/* Prepare the From header */
			char from_hdr[1024];
			if(session->account.display_name) {
				g_snprintf(from_hdr, sizeof(from_hdr), "\"%s\" <%s>", session->account.display_name, session->account.identity);
			} else {
				g_snprintf(from_hdr, sizeof(from_hdr), "%s", session->account.identity);
			}
			/* Prepare the stack */
				/* TODO */
			g_atomic_int_set(&session->hangingup, 0);
			session->status = janus_sipre_call_status_inviting;
			/* Create a random call-id */
			char callid[24];
			janus_sipre_random_string(24, (char *)&callid);
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("calling"));
				json_object_set_new(info, "callee", json_string(uri_text));
				json_object_set_new(info, "call-id", json_string(callid));
				json_object_set_new(info, "sdp", json_string(sdp));
				gateway->notify_event(&janus_sipre_plugin, session->handle, info);
			}
			/* Send INVITE */
			session->callee = g_strdup(uri_text);
			session->callid = g_strdup(callid);
			g_hash_table_insert(callids, session->callid, session);
			session->media.autoack = do_autoack;
				/* TODO Use re to send INVITE */
			g_free(sdp);
			if(session->transaction)
				g_free(session->transaction);
			session->transaction = msg->transaction ? g_strdup(msg->transaction) : NULL;
			/* Send an ack back */
			result = json_object();
			json_object_set_new(result, "event", json_string("calling"));
		} else if(!strcasecmp(request_text, "accept")) {
			if(session->status != janus_sipre_call_status_invited) {
				JANUS_LOG(LOG_ERR, "Wrong state (not invited? status=%s)\n", janus_sipre_call_status_string(session->status));
				error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Wrong state (not invited? status=%s)", janus_sipre_call_status_string(session->status));
				goto error;
			}
			if(session->callee == NULL) {
				JANUS_LOG(LOG_ERR, "Wrong state (no caller?)\n");
				error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Wrong state (no caller?)");
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, accept_parameters,
				error_code, error_cause, TRUE,
				JANUS_SIPRE_ERROR_MISSING_ELEMENT, JANUS_SIPRE_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			json_t *srtp = json_object_get(root, "srtp");
			gboolean answer_srtp = FALSE;
			if(srtp) {
				const char *srtp_text = json_string_value(srtp);
				if(!strcasecmp(srtp_text, "sdes_optional")) {
					/* Negotiate SDES, but make it optional */
					answer_srtp = TRUE;
				} else if(!strcasecmp(srtp_text, "sdes_mandatory")) {
					/* Negotiate SDES, and require it */
					answer_srtp = TRUE;
					session->media.require_srtp = TRUE;
				} else {
					JANUS_LOG(LOG_ERR, "Invalid element (srtp can only be sdes_optional or sdes_mandatory)\n");
					error_code = JANUS_SIPRE_ERROR_INVALID_ELEMENT;
					g_snprintf(error_cause, 512, "Invalid element (srtp can only be sdes_optional or sdes_mandatory)");
					goto error;
				}
			}
			if(session->media.require_srtp && !session->media.has_srtp_remote) {
				JANUS_LOG(LOG_ERR, "Can't accept the call: SDES-SRTP required, but caller didn't offer it\n");
				error_code = JANUS_SIPRE_ERROR_TOO_STRICT;
				g_snprintf(error_cause, 512, "Can't accept the call: SDES-SRTP required, but caller didn't offer it");
				goto error;
			}
			answer_srtp = answer_srtp || session->media.has_srtp_remote;
			/* Any SDP to handle? if not, something's wrong */
			const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
			const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
			if(!msg_sdp) {
				JANUS_LOG(LOG_ERR, "Missing SDP\n");
				error_code = JANUS_SIPRE_ERROR_MISSING_SDP;
				g_snprintf(error_cause, 512, "Missing SDP");
				goto error;
			}
			/* Accept a call from another peer */
			JANUS_LOG(LOG_VERB, "We're accepting the call from %s\n", session->callee);
			JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg_sdp_type, msg_sdp);
			session->media.has_srtp_local = answer_srtp;
			if(answer_srtp) {
				JANUS_LOG(LOG_VERB, "Going to negotiate SDES-SRTP (%s)...\n", session->media.require_srtp ? "mandatory" : "optional");
			}
			/* Parse the SDP we got, manipulate some things, and generate a new one */
			char sdperror[100];
			janus_sdp *parsed_sdp = janus_sdp_parse(msg_sdp, sdperror, sizeof(sdperror));
			if(!parsed_sdp) {
				JANUS_LOG(LOG_ERR, "Error parsing SDP: %s\n", sdperror);
				error_code = JANUS_SIPRE_ERROR_MISSING_SDP;
				g_snprintf(error_cause, 512, "Error parsing SDP: %s", sdperror);
				goto error;
			}
			/* Allocate RTP ports and merge them with the anonymized SDP */
			if(strstr(msg_sdp, "m=audio") && !strstr(msg_sdp, "m=audio 0")) {
				JANUS_LOG(LOG_VERB, "Going to negotiate audio...\n");
				session->media.has_audio = 1;	/* FIXME Maybe we need a better way to signal this */
			}
			if(strstr(msg_sdp, "m=video") && !strstr(msg_sdp, "m=video 0")) {
				JANUS_LOG(LOG_VERB, "Going to negotiate video...\n");
				session->media.has_video = 1;	/* FIXME Maybe we need a better way to signal this */
			}
			if(janus_sipre_allocate_local_ports(session) < 0) {
				JANUS_LOG(LOG_ERR, "Could not allocate RTP/RTCP ports\n");
				janus_sdp_free(parsed_sdp);
				error_code = JANUS_SIPRE_ERROR_IO_ERROR;
				g_snprintf(error_cause, 512, "Could not allocate RTP/RTCP ports");
				goto error;
			}
			char *sdp = janus_sipre_sdp_manipulate(session, parsed_sdp, TRUE);
			if(sdp == NULL) {
				JANUS_LOG(LOG_ERR, "Could not allocate RTP/RTCP ports\n");
				janus_sdp_free(parsed_sdp);
				error_code = JANUS_SIPRE_ERROR_IO_ERROR;
				g_snprintf(error_cause, 512, "Could not allocate RTP/RTCP ports");
				goto error;
			}
			if(session->media.audio_pt > -1) {
				session->media.audio_pt_name = janus_get_codec_from_pt(sdp, session->media.audio_pt);
				JANUS_LOG(LOG_VERB, "Detected audio codec: %d (%s)\n", session->media.audio_pt, session->media.audio_pt_name);
			}
			if(session->media.video_pt > -1) {
				session->media.video_pt_name = janus_get_codec_from_pt(sdp, session->media.video_pt);
				JANUS_LOG(LOG_VERB, "Detected video codec: %d (%s)\n", session->media.video_pt, session->media.video_pt_name);
			}
			/* Take note of the SDP (may be useful for UPDATEs or re-INVITEs) */
			janus_sdp_free(session->sdp);
			session->sdp = parsed_sdp;
			JANUS_LOG(LOG_VERB, "Prepared SDP for 200 OK:\n%s", sdp);
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("accepted"));
				if(session->callid)
					json_object_set_new(info, "call-id", json_string(session->callid));
				gateway->notify_event(&janus_sipre_plugin, session->handle, info);
			}
			/* Send 200 OK */
			g_atomic_int_set(&session->hangingup, 0);
			session->status = janus_sipre_call_status_incall;
				/* TODO Use re to send 200 OK */
			g_free(sdp);
			/* Send an ack back */
			result = json_object();
			json_object_set_new(result, "event", json_string("accepted"));
			/* Start the media */
			session->media.ready = 1;	/* FIXME Maybe we need a better way to signal this */
			GError *error = NULL;
			char tname[16];
			g_snprintf(tname, sizeof(tname), "siprtp %s", session->account.username);
			g_thread_try_new(tname, janus_sipre_relay_thread, session, &error);
			if(error != NULL) {
				JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the RTP/RTCP thread...\n", error->code, error->message ? error->message : "??");
			}
		} else if(!strcasecmp(request_text, "decline")) {
			/* Reject an incoming call */
			if(session->status != janus_sipre_call_status_invited) {
				JANUS_LOG(LOG_ERR, "Wrong state (not invited? status=%s)\n", janus_sipre_call_status_string(session->status));
				/* Ignore */
				janus_sipre_message_free(msg);
				continue;
				//~ g_snprintf(error_cause, 512, "Wrong state (not in a call?)");
				//~ goto error;
			}
			if(session->callee == NULL) {
				JANUS_LOG(LOG_ERR, "Wrong state (no callee?)\n");
				error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Wrong state (no callee?)");
				goto error;
			}
			session->status = janus_sipre_call_status_closing;
			/* Prepare response */
			int response_code = 486;
			json_t *code_json = json_object_get(root, "code");
			if(code_json && json_is_integer(code_json))
				response_code = json_integer_value(code_json);
			if(response_code <= 399) {
				JANUS_LOG(LOG_WARN, "Invalid SIPre response code specified, using 486 to decline call\n");
				response_code = 486;
			}
				/* TODO Use re to send error */
			/* Also notify event handlers */
			if(notify_events && gateway->events_is_enabled()) {
				json_t *info = json_object();
				json_object_set_new(info, "event", json_string("declined"));
				json_object_set_new(info, "callee", json_string(session->callee));
				if(session->callid)
					json_object_set_new(info, "call-id", json_string(session->callid));
				json_object_set_new(info, "code", json_integer(response_code));
				gateway->notify_event(&janus_sipre_plugin, session->handle, info);
			}
			g_free(session->callee);
			session->callee = NULL;
			/* Notify the operation */
			result = json_object();
			json_object_set_new(result, "event", json_string("declining"));
			json_object_set_new(result, "code", json_integer(response_code));
		} else if(!strcasecmp(request_text, "hangup")) {
			/* Hangup an ongoing call */
			if(!(session->status == janus_sipre_call_status_inviting || session->status == janus_sipre_call_status_incall)) {
				JANUS_LOG(LOG_ERR, "Wrong state (not in a call? status=%s)\n", janus_sipre_call_status_string(session->status));
				/* Ignore */
				janus_sipre_message_free(msg);
				continue;
				//~ g_snprintf(error_cause, 512, "Wrong state (not in a call?)");
				//~ goto error;
			}
			if(session->callee == NULL) {
				JANUS_LOG(LOG_ERR, "Wrong state (no callee?)\n");
				error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Wrong state (no callee?)");
				goto error;
			}
			session->status = janus_sipre_call_status_closing;
				/* TODO Use re to send BYE */
			g_free(session->callee);
			session->callee = NULL;
			/* Notify the operation */
			result = json_object();
			json_object_set_new(result, "event", json_string("hangingup"));
		} else if(!strcasecmp(request_text, "recording")) {
			/* Start or stop recording */
			if(!(session->status == janus_sipre_call_status_inviting || session->status == janus_sipre_call_status_incall)) {
				JANUS_LOG(LOG_ERR, "Wrong state (not in a call? status=%s)\n", janus_sipre_call_status_string(session->status));
				g_snprintf(error_cause, 512, "Wrong state (not in a call?)");
				goto error;
			}
			if(session->callee == NULL) {
				JANUS_LOG(LOG_ERR, "Wrong state (no callee?)\n");
				error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Wrong state (no callee?)");
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, recording_parameters,
				error_code, error_cause, TRUE,
				JANUS_SIPRE_ERROR_MISSING_ELEMENT, JANUS_SIPRE_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			json_t *action = json_object_get(root, "action");
			const char *action_text = json_string_value(action);
			if(strcasecmp(action_text, "start") && strcasecmp(action_text, "stop")) {
				JANUS_LOG(LOG_ERR, "Invalid action (should be start|stop)\n");
				error_code = JANUS_SIPRE_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid action (should be start|stop)");
				goto error;
			}
			gboolean record_audio = FALSE, record_video = FALSE,	/* No media is recorded by default */
				record_peer_audio = FALSE, record_peer_video = FALSE;
			json_t *audio = json_object_get(root, "audio");
			record_audio = audio ? json_is_true(audio) : FALSE;
			json_t *video = json_object_get(root, "video");
			record_video = video ? json_is_true(video) : FALSE;
			json_t *peer_audio = json_object_get(root, "peer_audio");
			record_peer_audio = peer_audio ? json_is_true(peer_audio) : FALSE;
			json_t *peer_video = json_object_get(root, "peer_video");
			record_peer_video = peer_video ? json_is_true(peer_video) : FALSE;
			if(!record_audio && !record_video && !record_peer_audio && !record_peer_video) {
				JANUS_LOG(LOG_ERR, "Invalid request (at least one of audio, video, peer_audio and peer_video should be true)\n");
				error_code = JANUS_SIPRE_ERROR_RECORDING_ERROR;
				g_snprintf(error_cause, 512, "Invalid request (at least one of audio, video, peer_audio and peer_video should be true)");
				goto error;
			}
			json_t *recfile = json_object_get(root, "filename");
			const char *recording_base = json_string_value(recfile);
			janus_mutex_lock(&session->rec_mutex);
			if(!strcasecmp(action_text, "start")) {
				/* Start recording something */
				char filename[255];
				gint64 now = janus_get_real_time();
				if(record_peer_audio || record_peer_video) {
					JANUS_LOG(LOG_INFO, "Starting recording of peer's %s (user %s, call %s)\n",
						(record_peer_audio && record_peer_video ? "audio and video" : (record_peer_audio ? "audio" : "video")),
						session->account.username, session->transaction);
					/* Start recording this peer's audio and/or video */
					if(record_peer_audio) {
						memset(filename, 0, 255);
						if(recording_base) {
							/* Use the filename and path we have been provided */
							g_snprintf(filename, 255, "%s-peer-audio", recording_base);
							/* FIXME This only works if offer/answer happened */
							session->arc_peer = janus_recorder_create(NULL, session->media.audio_pt_name, filename);
							if(session->arc_peer == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this peer!\n");
							}
						} else {
							/* Build a filename */
							g_snprintf(filename, 255, "sip-%s-%s-%"SCNi64"-peer-audio",
								session->account.username ? session->account.username : "unknown",
								session->transaction ? session->transaction : "unknown",
								now);
							/* FIXME This only works if offer/answer happened */
							session->arc_peer = janus_recorder_create(NULL, session->media.audio_pt_name, filename);
							if(session->arc_peer == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this peer!\n");
							}
						}
					}
					if(record_peer_video) {
						memset(filename, 0, 255);
						if(recording_base) {
							/* Use the filename and path we have been provided */
							g_snprintf(filename, 255, "%s-peer-video", recording_base);
							/* FIXME This only works if offer/answer happened */
							session->vrc_peer = janus_recorder_create(NULL, session->media.video_pt_name, filename);
							if(session->vrc_peer == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this peer!\n");
							}
						} else {
							/* Build a filename */
							g_snprintf(filename, 255, "sip-%s-%s-%"SCNi64"-peer-video",
								session->account.username ? session->account.username : "unknown",
								session->transaction ? session->transaction : "unknown",
								now);
							/* FIXME This only works if offer/answer happened */
							session->vrc_peer = janus_recorder_create(NULL, session->media.video_pt_name, filename);
							if(session->vrc_peer == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this peer!\n");
							}
						}
						/* TODO We should send a FIR/PLI to this peer... */
					}
				}
				if(record_audio || record_video) {
					/* Start recording the user's audio and/or video */
					JANUS_LOG(LOG_INFO, "Starting recording of user's %s (user %s, call %s)\n",
						(record_audio && record_video ? "audio and video" : (record_audio ? "audio" : "video")),
						session->account.username, session->transaction);
					if(record_audio) {
						memset(filename, 0, 255);
						if(recording_base) {
							/* Use the filename and path we have been provided */
							g_snprintf(filename, 255, "%s-user-audio", recording_base);
							/* FIXME This only works if offer/answer happened */
							session->arc = janus_recorder_create(NULL, session->media.audio_pt_name, filename);
							if(session->arc == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this peer!\n");
							}
						} else {
							/* Build a filename */
							g_snprintf(filename, 255, "sip-%s-%s-%"SCNi64"-own-audio",
								session->account.username ? session->account.username : "unknown",
								session->transaction ? session->transaction : "unknown",
								now);
							/* FIXME This only works if offer/answer happened */
							session->arc = janus_recorder_create(NULL, session->media.audio_pt_name, filename);
							if(session->arc == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this peer!\n");
							}
						}
					}
					if(record_video) {
						memset(filename, 0, 255);
						if(recording_base) {
							/* Use the filename and path we have been provided */
							g_snprintf(filename, 255, "%s-user-video", recording_base);
							/* FIXME This only works if offer/answer happened */
							session->vrc = janus_recorder_create(NULL, session->media.video_pt_name, filename);
							if(session->vrc == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this user!\n");
							}
						} else {
							/* Build a filename */
							g_snprintf(filename, 255, "sip-%s-%s-%"SCNi64"-own-video",
								session->account.username ? session->account.username : "unknown",
								session->transaction ? session->transaction : "unknown",
								now);
							/* FIXME This only works if offer/answer happened */
							session->vrc = janus_recorder_create(NULL, session->media.video_pt_name, filename);
							if(session->vrc == NULL) {
								/* FIXME We should notify the fact the recorder could not be created */
								JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this user!\n");
							}
						}
						/* Send a PLI */
						JANUS_LOG(LOG_VERB, "Recording video, sending a PLI to kickstart it\n");
						char buf[12];
						memset(buf, 0, 12);
						janus_rtcp_pli((char *)&buf, 12);
						gateway->relay_rtcp(session->handle, 1, buf, 12);
					}
				}
			} else {
				/* Stop recording something: notice that this never returns an error, even when we were not recording anything */
				if(record_audio) {
					if(session->arc) {
						janus_recorder_close(session->arc);
						JANUS_LOG(LOG_INFO, "Closed user's audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
						janus_recorder_free(session->arc);
					}
					session->arc = NULL;
				}
				if(record_video) {
					if(session->vrc) {
						janus_recorder_close(session->vrc);
						JANUS_LOG(LOG_INFO, "Closed user's video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
						janus_recorder_free(session->vrc);
					}
					session->vrc = NULL;
				}
				if(record_peer_audio) {
					if(session->arc_peer) {
						janus_recorder_close(session->arc_peer);
						JANUS_LOG(LOG_INFO, "Closed peer's audio recording %s\n", session->arc_peer->filename ? session->arc_peer->filename : "??");
						janus_recorder_free(session->arc_peer);
					}
					session->arc_peer = NULL;
				}
				if(record_peer_video) {
					if(session->vrc_peer) {
						janus_recorder_close(session->vrc_peer);
						JANUS_LOG(LOG_INFO, "Closed peer's video recording %s\n", session->vrc_peer->filename ? session->vrc_peer->filename : "??");
						janus_recorder_free(session->vrc_peer);
					}
					session->vrc_peer = NULL;
				}
			}
			janus_mutex_unlock(&session->rec_mutex);
			/* Notify the result */
			result = json_object();
			json_object_set_new(result, "event", json_string("recordingupdated"));
		} else if(!strcasecmp(request_text, "dtmf_info")) {
			/* Send DMTF tones using SIPre INFO
			 * (https://tools.ietf.org/html/draft-kaplan-dispatch-info-dtmf-package-00)
			 */
			if(!(session->status == janus_sipre_call_status_inviting || session->status == janus_sipre_call_status_incall)) {
				JANUS_LOG(LOG_ERR, "Wrong state (not in a call? status=%s)\n", janus_sipre_call_status_string(session->status));
				g_snprintf(error_cause, 512, "Wrong state (not in a call?)");
				goto error;
			}
			if(session->callee == NULL) {
				JANUS_LOG(LOG_ERR, "Wrong state (no callee?)\n");
				error_code = JANUS_SIPRE_ERROR_WRONG_STATE;
				g_snprintf(error_cause, 512, "Wrong state (no callee?)");
				goto error;
			}
			JANUS_VALIDATE_JSON_OBJECT(root, dtmf_info_parameters,
				error_code, error_cause, TRUE,
				JANUS_SIPRE_ERROR_MISSING_ELEMENT, JANUS_SIPRE_ERROR_INVALID_ELEMENT);
			if(error_code != 0)
				goto error;
			json_t *digit = json_object_get(root, "digit");
			const char *digit_text = json_string_value(digit);
			if(strlen(digit_text) != 1) {
				JANUS_LOG(LOG_ERR, "Invalid element (digit should be one character))\n");
				error_code = JANUS_SIPRE_ERROR_INVALID_ELEMENT;
				g_snprintf(error_cause, 512, "Invalid element (digit should be one character)");
				goto error;
			}
			int duration_ms = 0;
			json_t *duration = json_object_get(root, "duration");
			duration_ms = duration ? json_integer_value(duration) : 0;
			if(duration_ms <= 0 || duration_ms > 5000) {
				duration_ms = 160; /* default value */
			}

			char payload[64];
			g_snprintf(payload, sizeof(payload), "Signal=%s\r\nDuration=%d", digit_text, duration_ms);
			/* TODO Send "application/dtmf-relay" SIP INFO */
		} else {
			JANUS_LOG(LOG_ERR, "Unknown request (%s)\n", request_text);
			error_code = JANUS_SIPRE_ERROR_INVALID_REQUEST;
			g_snprintf(error_cause, 512, "Unknown request (%s)", request_text);
			goto error;
		}

		/* Prepare JSON event */
		json_t *event = json_object();
		json_object_set_new(event, "sip", json_string("event"));
		if(result != NULL)
			json_object_set_new(event, "result", result);
		int ret = gateway->push_event(msg->handle, &janus_sipre_plugin, msg->transaction, event, NULL);
		JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
		json_decref(event);
		janus_sipre_message_free(msg);
		continue;

error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "sip", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			int ret = gateway->push_event(msg->handle, &janus_sipre_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);
			janus_sipre_message_free(msg);
		}
	}
	JANUS_LOG(LOG_VERB, "Leaving SIPre handler thread\n");
	return NULL;
}


/* Process an incoming SDP */
void janus_sipre_sdp_process(janus_sipre_session *session, janus_sdp *sdp, gboolean answer, gboolean update, gboolean *changed) {
	if(!session || !sdp)
		return;
	/* c= */
	if(sdp->c_addr) {
		if(update && strcmp(sdp->c_addr, session->media.remote_ip)) {
			/* This is an update and an address changed */
			if(changed)
				*changed = TRUE;
		}
		g_free(session->media.remote_ip);
		session->media.remote_ip = g_strdup(sdp->c_addr);
	}
	GList *temp = sdp->m_lines;
	while(temp) {
		janus_sdp_mline *m = (janus_sdp_mline *)temp->data;
		session->media.require_srtp = session->media.require_srtp || (m->proto && !strcasecmp(m->proto, "RTP/SAVP"));
		if(m->type == JANUS_SDP_AUDIO) {
			if(m->port) {
				if(m->port != session->media.remote_audio_rtp_port) {
					/* This is an update and an address changed */
					if(changed)
						*changed = TRUE;
				}
				session->media.has_audio = 1;
				session->media.remote_audio_rtp_port = m->port;
				session->media.remote_audio_rtcp_port = m->port+1;	/* FIXME We're assuming RTCP is on the next port */
				if(m->direction == JANUS_SDP_SENDONLY || m->direction == JANUS_SDP_INACTIVE)
					session->media.audio_send = FALSE;
				else
					session->media.audio_send = TRUE;
			} else {
				session->media.audio_send = FALSE;
			}
		} else if(m->type == JANUS_SDP_VIDEO) {
			if(m->port) {
				if(m->port != session->media.remote_video_rtp_port) {
					/* This is an update and an address changed */
					if(changed)
						*changed = TRUE;
				}
				session->media.has_video = 1;
				session->media.remote_video_rtp_port = m->port;
				session->media.remote_video_rtcp_port = m->port+1;	/* FIXME We're assuming RTCP is on the next port */
				if(m->direction == JANUS_SDP_SENDONLY || m->direction == JANUS_SDP_INACTIVE)
					session->media.video_send = FALSE;
				else
					session->media.video_send = TRUE;
			} else {
				session->media.video_send = FALSE;
			}
		} else {
			JANUS_LOG(LOG_WARN, "Unsupported media line (not audio/video)\n");
			temp = temp->next;
			continue;
		}
		if(m->c_addr) {
			if(update && strcmp(m->c_addr, session->media.remote_ip)) {
				/* This is an update and an address changed */
				if(changed)
					*changed = TRUE;
			}
			g_free(session->media.remote_ip);
			session->media.remote_ip = g_strdup(m->c_addr);
		}
		if(update) {
			/* FIXME This is a session update, we only accept changes in IP/ports */
			temp = temp->next;
			continue;
		}
		GList *tempA = m->attributes;
		while(tempA) {
			janus_sdp_attribute *a = (janus_sdp_attribute *)tempA->data;
			if(a->name) {
				if(!strcasecmp(a->name, "crypto")) {
					if(m->type == JANUS_SDP_AUDIO || m->type == JANUS_SDP_VIDEO) {
						gint32 tag = 0;
						int suite;
						char crypto[81];
						/* FIXME inline can be more complex than that, and we're currently only offering SHA1_80 */
						int res = sscanf(a->value, "%"SCNi32" AES_CM_128_HMAC_SHA1_%2d inline:%80s",
							&tag, &suite, crypto);
						if(res != 3) {
							JANUS_LOG(LOG_WARN, "Failed to parse crypto line, ignoring... %s\n", a->value);
						} else {
							gboolean video = (m->type == JANUS_SDP_VIDEO);
							int current_suite = video ? session->media.video_srtp_suite_in : session->media.audio_srtp_suite_in;
							if(current_suite == 0) {
								if(video)
									session->media.video_srtp_suite_in = suite;
								else
									session->media.audio_srtp_suite_in = suite;
								janus_sipre_srtp_set_remote(session, video, crypto, suite);
								session->media.has_srtp_remote = TRUE;
							} else {
								JANUS_LOG(LOG_WARN, "We already configured a %s crypto context (AES_CM_128_HMAC_SHA1_%d), skipping additional crypto line\n",
									video ? "video" : "audio", current_suite);
							}
						}
					}
				}
			}
			tempA = tempA->next;
		}
		if(answer && (m->type == JANUS_SDP_AUDIO || m->type == JANUS_SDP_VIDEO)) {
			/* Check which codec was negotiated eventually */
			int pt = -1;
			if(m->ptypes)
				pt = GPOINTER_TO_INT(m->ptypes->data);
			if(pt > -1) {
				if(m->type == JANUS_SDP_AUDIO) {
					session->media.audio_pt = pt;
				} else {
					session->media.video_pt = pt;
				}
			}
		}
		temp = temp->next;
	}
	if(update && changed && *changed) {
		/* Something changed: mark this on the session, so that the thread can update the sockets */
		session->media.updated = TRUE;
		if(session->media.pipefd[1] > 0) {
			int code = 1;
			ssize_t res = 0;
			do {
				res = write(session->media.pipefd[1], &code, sizeof(int));
			} while(res == -1 && errno == EINTR);
		}
	}
}

char *janus_sipre_sdp_manipulate(janus_sipre_session *session, janus_sdp *sdp, gboolean answer) {
	if(!session || !sdp)
		return NULL;
	/* Start replacing stuff */
	JANUS_LOG(LOG_VERB, "Setting protocol to %s\n", session->media.require_srtp ? "RTP/SAVP" : "RTP/AVP");
	GList *temp = sdp->m_lines;
	while(temp) {
		janus_sdp_mline *m = (janus_sdp_mline *)temp->data;
		g_free(m->proto);
		m->proto = g_strdup(session->media.require_srtp ? "RTP/SAVP" : "RTP/AVP");
		if(m->type == JANUS_SDP_AUDIO) {
			m->port = session->media.local_audio_rtp_port;
			if(session->media.has_srtp_local) {
				char *crypto = NULL;
				session->media.audio_srtp_suite_out = 80;
				janus_sipre_srtp_set_local(session, FALSE, &crypto);
				/* FIXME 32? 80? Both? */
				janus_sdp_attribute *a = janus_sdp_attribute_create("crypto", "1 AES_CM_128_HMAC_SHA1_80 inline:%s", crypto);
				g_free(crypto);
				m->attributes = g_list_append(m->attributes, a);
			}
		} else if(m->type == JANUS_SDP_VIDEO) {
			m->port = session->media.local_video_rtp_port;
			if(session->media.has_srtp_local) {
				char *crypto = NULL;
				session->media.audio_srtp_suite_out = 80;
				janus_sipre_srtp_set_local(session, TRUE, &crypto);
				/* FIXME 32? 80? Both? */
				janus_sdp_attribute *a = janus_sdp_attribute_create("crypto", "1 AES_CM_128_HMAC_SHA1_80 inline:%s", crypto);
				g_free(crypto);
				m->attributes = g_list_append(m->attributes, a);
			}
		}
		g_free(m->c_addr);
		m->c_addr = g_strdup(local_ip);
		if(answer && (m->type == JANUS_SDP_AUDIO || m->type == JANUS_SDP_VIDEO)) {
			/* Check which codec was negotiated eventually */
			int pt = -1;
			if(m->ptypes)
				pt = GPOINTER_TO_INT(m->ptypes->data);
			if(pt > -1) {
				if(m->type == JANUS_SDP_AUDIO) {
					session->media.audio_pt = pt;
				} else {
					session->media.video_pt = pt;
				}
			}
		}
		temp = temp->next;
	}
	/* Generate a SDP string out of our changes */
	return janus_sdp_write(sdp);
}

/* Bind local RTP/RTCP sockets */
static int janus_sipre_allocate_local_ports(janus_sipre_session *session) {
	if(session == NULL) {
		JANUS_LOG(LOG_ERR, "Invalid session\n");
		return -1;
	}
	/* Reset status */
	if(session->media.audio_rtp_fd != -1) {
		close(session->media.audio_rtp_fd);
		session->media.audio_rtp_fd = -1;
	}
	if(session->media.audio_rtcp_fd != -1) {
		close(session->media.audio_rtcp_fd);
		session->media.audio_rtcp_fd = -1;
	}
	session->media.local_audio_rtp_port = 0;
	session->media.local_audio_rtcp_port = 0;
	session->media.audio_ssrc = 0;
	if(session->media.video_rtp_fd != -1) {
		close(session->media.video_rtp_fd);
		session->media.video_rtp_fd = -1;
	}
	if(session->media.video_rtcp_fd != -1) {
		close(session->media.video_rtcp_fd);
		session->media.video_rtcp_fd = -1;
	}
	session->media.local_video_rtp_port = 0;
	session->media.local_video_rtcp_port = 0;
	session->media.video_ssrc = 0;
	if(session->media.pipefd[0] > 0) {
		close(session->media.pipefd[0]);
		session->media.pipefd[0] = -1;
	}
	if(session->media.pipefd[1] > 0) {
		close(session->media.pipefd[1]);
		session->media.pipefd[1] = -1;
	}
	/* Start */
	int attempts = 100;	/* FIXME Don't retry forever */
	if(session->media.has_audio) {
		JANUS_LOG(LOG_VERB, "Allocating audio ports:\n");
		struct sockaddr_in audio_rtp_address, audio_rtcp_address;
		while(session->media.local_audio_rtp_port == 0 || session->media.local_audio_rtcp_port == 0) {
			if(attempts == 0)	/* Too many failures */
				return -1;
			if(session->media.audio_rtp_fd == -1) {
				session->media.audio_rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
			}
			if(session->media.audio_rtcp_fd == -1) {
				session->media.audio_rtcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
			}
			int rtp_port = g_random_int_range(10000, 60000);	/* FIXME Should this be configurable? */
			if(rtp_port % 2)
				rtp_port++;	/* Pick an even port for RTP */
			audio_rtp_address.sin_family = AF_INET;
			audio_rtp_address.sin_port = htons(rtp_port);
			inet_pton(AF_INET, local_ip, &audio_rtp_address.sin_addr.s_addr);
			if(bind(session->media.audio_rtp_fd, (struct sockaddr *)(&audio_rtp_address), sizeof(struct sockaddr)) < 0) {
				JANUS_LOG(LOG_ERR, "Bind failed for audio RTP (port %d), trying a different one...\n", rtp_port);
				attempts--;
				continue;
			}
			JANUS_LOG(LOG_VERB, "Audio RTP listener bound to port %d\n", rtp_port);
			int rtcp_port = rtp_port+1;
			audio_rtcp_address.sin_family = AF_INET;
			audio_rtcp_address.sin_port = htons(rtcp_port);
			inet_pton(AF_INET, local_ip, &audio_rtcp_address.sin_addr.s_addr);
			if(bind(session->media.audio_rtcp_fd, (struct sockaddr *)(&audio_rtcp_address), sizeof(struct sockaddr)) < 0) {
				JANUS_LOG(LOG_ERR, "Bind failed for audio RTCP (port %d), trying a different one...\n", rtcp_port);
				/* RTP socket is not valid anymore, reset it */
				close(session->media.audio_rtp_fd);
				session->media.audio_rtp_fd = -1;
				attempts--;
				continue;
			}
			JANUS_LOG(LOG_VERB, "Audio RTCP listener bound to port %d\n", rtcp_port);
			session->media.local_audio_rtp_port = rtp_port;
			session->media.local_audio_rtcp_port = rtcp_port;
		}
	}
	if(session->media.has_video) {
		JANUS_LOG(LOG_VERB, "Allocating video ports:\n");
		struct sockaddr_in video_rtp_address, video_rtcp_address;
		while(session->media.local_video_rtp_port == 0 || session->media.local_video_rtcp_port == 0) {
			if(attempts == 0)	/* Too many failures */
				return -1;
			if(session->media.video_rtp_fd == -1) {
				session->media.video_rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
			}
			if(session->media.video_rtcp_fd == -1) {
				session->media.video_rtcp_fd = socket(AF_INET, SOCK_DGRAM, 0);
			}
			int rtp_port = g_random_int_range(10000, 60000);	/* FIXME Should this be configurable? */
			if(rtp_port % 2)
				rtp_port++;	/* Pick an even port for RTP */
			video_rtp_address.sin_family = AF_INET;
			video_rtp_address.sin_port = htons(rtp_port);
			inet_pton(AF_INET, local_ip, &video_rtp_address.sin_addr.s_addr);
			if(bind(session->media.video_rtp_fd, (struct sockaddr *)(&video_rtp_address), sizeof(struct sockaddr)) < 0) {
				JANUS_LOG(LOG_ERR, "Bind failed for video RTP (port %d), trying a different one...\n", rtp_port);
				attempts--;
				continue;
			}
			JANUS_LOG(LOG_VERB, "Video RTP listener bound to port %d\n", rtp_port);
			int rtcp_port = rtp_port+1;
			video_rtcp_address.sin_family = AF_INET;
			video_rtcp_address.sin_port = htons(rtcp_port);
			inet_pton(AF_INET, local_ip, &video_rtcp_address.sin_addr.s_addr);
			if(bind(session->media.video_rtcp_fd, (struct sockaddr *)(&video_rtcp_address), sizeof(struct sockaddr)) < 0) {
				JANUS_LOG(LOG_ERR, "Bind failed for video RTCP (port %d), trying a different one...\n", rtcp_port);
				/* RTP socket is not valid anymore, reset it */
				close(session->media.video_rtp_fd);
				session->media.video_rtp_fd = -1;
				attempts--;
				continue;
			}
			JANUS_LOG(LOG_VERB, "Video RTCP listener bound to port %d\n", rtcp_port);
			session->media.local_video_rtp_port = rtp_port;
			session->media.local_video_rtcp_port = rtcp_port;
		}
	}
	/* We need this to quickly interrupt the poll when it's time to update a session or wrap up */
	pipe(session->media.pipefd);
	return 0;
}

/* Helper method to (re)connect RTP/RTCP sockets */
static void janus_sipre_connect_sockets(janus_sipre_session *session, struct sockaddr_in *server_addr) {
	if(!session || !server_addr)
		return;

	if(session->media.updated) {
		JANUS_LOG(LOG_VERB, "Updating session sockets\n");
	}

	/* Connect peers (FIXME This pretty much sucks right now) */
	if(session->media.remote_audio_rtp_port) {
		server_addr->sin_port = htons(session->media.remote_audio_rtp_port);
		if(connect(session->media.audio_rtp_fd, (struct sockaddr *)server_addr, sizeof(struct sockaddr)) == -1) {
			JANUS_LOG(LOG_ERR, "[SIPre-%s] Couldn't connect audio RTP? (%s:%d)\n", session->account.username, session->media.remote_ip, session->media.remote_audio_rtp_port);
			JANUS_LOG(LOG_ERR, "[SIPre-%s]   -- %d (%s)\n", session->account.username, errno, strerror(errno));
		}
	}
	if(session->media.remote_audio_rtcp_port) {
		server_addr->sin_port = htons(session->media.remote_audio_rtcp_port);
		if(connect(session->media.audio_rtcp_fd, (struct sockaddr *)server_addr, sizeof(struct sockaddr)) == -1) {
			JANUS_LOG(LOG_ERR, "[SIPre-%s] Couldn't connect audio RTCP? (%s:%d)\n", session->account.username, session->media.remote_ip, session->media.remote_audio_rtcp_port);
			JANUS_LOG(LOG_ERR, "[SIPre-%s]   -- %d (%s)\n", session->account.username, errno, strerror(errno));
		}
	}
	if(session->media.remote_video_rtp_port) {
		server_addr->sin_port = htons(session->media.remote_video_rtp_port);
		if(connect(session->media.video_rtp_fd, (struct sockaddr *)server_addr, sizeof(struct sockaddr)) == -1) {
			JANUS_LOG(LOG_ERR, "[SIPre-%s] Couldn't connect video RTP? (%s:%d)\n", session->account.username, session->media.remote_ip, session->media.remote_video_rtp_port);
			JANUS_LOG(LOG_ERR, "[SIPre-%s]   -- %d (%s)\n", session->account.username, errno, strerror(errno));
		}
	}
	if(session->media.remote_video_rtcp_port) {
		server_addr->sin_port = htons(session->media.remote_video_rtcp_port);
		if(connect(session->media.video_rtcp_fd, (struct sockaddr *)server_addr, sizeof(struct sockaddr)) == -1) {
			JANUS_LOG(LOG_ERR, "[SIPre-%s] Couldn't connect video RTCP? (%s:%d)\n", session->account.username, session->media.remote_ip, session->media.remote_video_rtcp_port);
			JANUS_LOG(LOG_ERR, "[SIPre-%s]   -- %d (%s)\n", session->account.username, errno, strerror(errno));
		}
	}

}

/* Thread to relay RTP/RTCP frames coming from the SIPre peer */
static void *janus_sipre_relay_thread(void *data) {
	janus_sipre_session *session = (janus_sipre_session *)data;
	if(!session || !session->account.username || !session->callee) {
		g_thread_unref(g_thread_self());
		return NULL;
	}
	JANUS_LOG(LOG_VERB, "Starting relay thread (%s <--> %s)\n", session->account.username, session->callee);

	gboolean have_server_ip = TRUE;
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	if((inet_aton(session->media.remote_ip, &server_addr.sin_addr)) <= 0) {	/* Not a numeric IP... */
		struct hostent *host = gethostbyname(session->media.remote_ip);	/* ...resolve name */
		if(!host) {
			JANUS_LOG(LOG_ERR, "[SIPre-%s] Couldn't get host (%s)\n", session->account.username, session->media.remote_ip);
			have_server_ip = FALSE;
		} else {
			server_addr.sin_addr = *(struct in_addr *)host->h_addr_list;
		}
	}
	if(have_server_ip)
		janus_sipre_connect_sockets(session, &server_addr);

	if(!session->callee) {
		JANUS_LOG(LOG_VERB, "[SIPre-%s] Leaving thread, no callee...\n", session->account.username);
		g_thread_unref(g_thread_self());
		return NULL;
	}
	/* File descriptors */
	socklen_t addrlen;
	struct sockaddr_in remote;
	int resfd = 0, bytes = 0;
	struct pollfd fds[5];
	int pipe_fd = session->media.pipefd[0];
	char buffer[1500];
	memset(buffer, 0, 1500);
	/* Loop */
	int num = 0;
	gboolean goon = TRUE;
	int astep = 0, vstep = 0;
	guint32 ats = 0, vts = 0;
	while(goon && session != NULL && !session->destroyed &&
			session->status > janus_sipre_call_status_idle &&
			session->status < janus_sipre_call_status_closing) {	/* FIXME We need a per-call watchdog as well */

		if(session->media.updated) {
			/* Apparently there was a session update */
			if(have_server_ip && (inet_aton(session->media.remote_ip, &server_addr.sin_addr)) <= 0) {
				janus_sipre_connect_sockets(session, &server_addr);
			} else {
				JANUS_LOG(LOG_ERR, "[SIPre-%s] Couldn't update session details (missing or invalid remote IP address)\n", session->account.username);
			}
			session->media.updated = FALSE;
		}

		/* Prepare poll */
		num = 0;
		if(session->media.audio_rtp_fd != -1) {
			fds[num].fd = session->media.audio_rtp_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(session->media.audio_rtcp_fd != -1) {
			fds[num].fd = session->media.audio_rtcp_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(session->media.video_rtp_fd != -1) {
			fds[num].fd = session->media.video_rtp_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(session->media.video_rtcp_fd != -1) {
			fds[num].fd = session->media.video_rtcp_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		if(pipe_fd != -1) {
			fds[num].fd = pipe_fd;
			fds[num].events = POLLIN;
			fds[num].revents = 0;
			num++;
		}
		/* Wait for some data */
		resfd = poll(fds, num, 1000);
		if(resfd < 0) {
			JANUS_LOG(LOG_ERR, "[SIPre-%s] Error polling...\n", session->account.username);
			JANUS_LOG(LOG_ERR, "[SIPre-%s]   -- %d (%s)\n", session->account.username, errno, strerror(errno));
			break;
		} else if(resfd == 0) {
			/* No data, keep going */
			continue;
		}
		if(session == NULL || session->destroyed ||
				session->status <= janus_sipre_call_status_idle ||
				session->status >= janus_sipre_call_status_closing)
			break;
		int i = 0;
		for(i=0; i<num; i++) {
			if(fds[i].revents & (POLLERR | POLLHUP)) {
				/* Socket error? */
				JANUS_LOG(LOG_ERR, "[SIPre-%s] Error polling: %s...\n", session->account.username,
					fds[i].revents & POLLERR ? "POLLERR" : "POLLHUP");
				JANUS_LOG(LOG_ERR, "[SIPre-%s]   -- %d (%s)\n", session->account.username, errno, strerror(errno));
				if(session->media.updated)
					break;
				goon = FALSE;	/* Can we assume it's pretty much over, after a POLLERR? */
				/* FIXME Simulate a "hangup" coming from the browser */
				janus_sipre_message *msg = g_malloc0(sizeof(janus_sipre_message));
				msg->handle = session->handle;
				msg->message = json_pack("{ss}", "request", "hangup");
				msg->transaction = NULL;
				msg->jsep = NULL;
				g_async_queue_push(messages, msg);
				break;
			} else if(fds[i].revents & POLLIN) {
				if(pipe_fd != -1 && fds[i].fd == pipe_fd) {
					/* Poll interrupted for a reason, go on */
					int code = 0;
					bytes = read(pipe_fd, &code, sizeof(int));
					break;
				}
				/* Got an RTP/RTCP packet */
				addrlen = sizeof(remote);
				bytes = recvfrom(fds[i].fd, buffer, 1500, 0, (struct sockaddr*)&remote, &addrlen);
				/* Let's check what this is */
				gboolean video = fds[i].fd == session->media.video_rtp_fd || fds[i].fd == session->media.video_rtcp_fd;
				gboolean rtcp = fds[i].fd == session->media.audio_rtcp_fd || fds[i].fd == session->media.video_rtcp_fd;
				if(!rtcp) {
					/* Audio or Video RTP */
					rtp_header *header = (rtp_header *)buffer;
					if((video && session->media.video_ssrc_peer != ntohl(header->ssrc)) ||
							(!video && session->media.audio_ssrc_peer != ntohl(header->ssrc))) {
						if(video) {
							session->media.video_ssrc_peer = ntohl(header->ssrc);
						} else {
							session->media.audio_ssrc_peer = ntohl(header->ssrc);
						}
						JANUS_LOG(LOG_VERB, "[SIPre-%s] Got SIP peer %s SSRC: %"SCNu32"\n",
							session->account.username ? session->account.username : "unknown",
							video ? "video" : "audio", session->media.audio_ssrc_peer);
					}
					/* Is this SRTP? */
					if(session->media.has_srtp_remote) {
						int buflen = bytes;
						srtp_err_status_t res = srtp_unprotect(
							(video ? session->media.video_srtp_in : session->media.audio_srtp_in),
							buffer, &buflen);
						if(res != srtp_err_status_ok && res != srtp_err_status_replay_fail && res != srtp_err_status_replay_old) {
							guint32 timestamp = ntohl(header->timestamp);
							guint16 seq = ntohs(header->seq_number);
							JANUS_LOG(LOG_ERR, "[SIPre-%s] %s SRTP unprotect error: %s (len=%d-->%d, ts=%"SCNu32", seq=%"SCNu16")\n",
								session->account.username ? session->account.username : "unknown",
								video ? "Video" : "Audio", janus_srtp_error_str(res), bytes, buflen, timestamp, seq);
							continue;
						}
						bytes = buflen;
					}
					/* Check if the SSRC changed (e.g., after a re-INVITE or UPDATE) */
					guint32 timestamp = ntohl(header->timestamp);
					janus_rtp_header_update(header, &session->media.context, video,
						(video ? (vstep ? vstep : 4500) : (astep ? astep : 960)));
					if(video) {
						if(vts == 0) {
							vts = timestamp;
						} else if(vstep == 0) {
							vstep = timestamp-vts;
							if(vstep < 0) {
								vstep = 0;
							}
						}
					} else {
						if(ats == 0) {
							ats = timestamp;
						} else if(astep == 0) {
							astep = timestamp-ats;
							if(astep < 0) {
								astep = 0;
							}
						}
					}
					/* Save the frame if we're recording */
					janus_recorder_save_frame(video ? session->vrc_peer : session->arc_peer, buffer, bytes);
					/* Relay to browser */
					gateway->relay_rtp(session->handle, video, buffer, bytes);
					continue;
				} else {
					/* Audio or Video RTCP */
					if(session->media.has_srtp_remote) {
						int buflen = bytes;
						srtp_err_status_t res = srtp_unprotect_rtcp(
							(video ? session->media.video_srtp_in : session->media.audio_srtp_in),
							buffer, &buflen);
						if(res != srtp_err_status_ok && res != srtp_err_status_replay_fail && res != srtp_err_status_replay_old) {
							JANUS_LOG(LOG_ERR, "[SIPre-%s] %s SRTCP unprotect error: %s (len=%d-->%d)\n",
								session->account.username ? session->account.username : "unknown",
								video ? "Video" : "Audio", janus_srtp_error_str(res), bytes, buflen);
							continue;
						}
						bytes = buflen;
					}
					/* Relay to browser */
					gateway->relay_rtcp(session->handle, video, buffer, bytes);
					continue;
				}
			}
		}
	}
	if(session->media.audio_rtp_fd != -1) {
		close(session->media.audio_rtp_fd);
		session->media.audio_rtp_fd = -1;
	}
	if(session->media.audio_rtcp_fd != -1) {
		close(session->media.audio_rtcp_fd);
		session->media.audio_rtcp_fd = -1;
	}
	session->media.local_audio_rtp_port = 0;
	session->media.local_audio_rtcp_port = 0;
	session->media.audio_ssrc = 0;
	if(session->media.video_rtp_fd != -1) {
		close(session->media.video_rtp_fd);
		session->media.video_rtp_fd = -1;
	}
	if(session->media.video_rtcp_fd != -1) {
		close(session->media.video_rtcp_fd);
		session->media.video_rtcp_fd = -1;
	}
	session->media.local_video_rtp_port = 0;
	session->media.local_video_rtcp_port = 0;
	session->media.video_ssrc = 0;
	if(session->media.pipefd[0] > 0) {
		close(session->media.pipefd[0]);
		session->media.pipefd[0] = -1;
	}
	if(session->media.pipefd[1] > 0) {
		close(session->media.pipefd[1]);
		session->media.pipefd[1] = -1;
	}
	/* Clean up SRTP stuff, if needed */
	janus_sipre_srtp_cleanup(session);
	/* Done */
	JANUS_LOG(LOG_VERB, "Leaving SIPre relay thread\n");
	g_thread_unref(g_thread_self());
	return NULL;
}


/* libre loop thread */
gpointer janus_sipre_stack_thread(gpointer user_data) {
	JANUS_LOG(LOG_INFO, "Joining libre loop thread...\n");

	/* Initialize this thread as a worker */
	int err = 0;
	err = re_thread_init();
	if(err != 0) {
		printf("re_thread_init failed: %d (%s)\n", err, strerror(err));
		g_thread_unref(g_thread_self());
		return NULL;
	}
	/* Enter loop */
	err = re_main(NULL);
	if(err != 0) {
		JANUS_LOG(LOG_ERR, "re_main() failed: %d (%s)\n", err, strerror(err));
	}

	/* Done here */
	JANUS_LOG(LOG_WARN, "Leaving libre loop thread...\n");
	re_thread_close();

	g_thread_unref(g_thread_self());
	return NULL;
}

/* Called when challenged for credentials */
int janus_sipre_cb_auth(char **user, char **pass, const char *realm, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;
	JANUS_LOG(LOG_INFO, "[SIPre-%s] janus_sipre_cb_auth (realm=%s)\n", session->account.username, realm);
	/* TODO How do we handle hashed secrets? */
	int err = 0;
	err |= str_dup(user, session->account.authuser);
	err |= str_dup(pass, session->account.secret);
	JANUS_LOG(LOG_INFO, "[SIPre-%s]   -- %s / %s\n", session->account.username, *user, *pass);
	return err;
}

/* Called when REGISTER responses are received */
void janus_sipre_cb_register(int err, const struct sip_msg *msg, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;
	JANUS_LOG(LOG_INFO, "[SIPre-%s] janus_sipre_cb_register\n", session->account.username);
	if(err) {
		JANUS_LOG(LOG_ERR, "[SIPre-%s] REGISTER error: %s\n", session->account.username, strerror(err));
	} else {
		JANUS_LOG(LOG_INFO, "[SIPre-%s] REGISTER reply: %u %s\n", session->account.username, msg->scode, (char *)&msg->reason.p);
	}
	/* TODO Send result back to user */
}

/* Called when SIP progress (e.g., 180 Ringing) responses are received */
void janus_sipre_cb_progress(const struct sip_msg *msg, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;
	JANUS_LOG(LOG_INFO, "[SIPre-%s] session progress: %u %s\n", session->account.username, msg->scode, (char *)&msg->reason.p);

	/* TODO Handle */
}

/* Called upon incoming INVITEs */
void janus_sipre_cb_incoming(const struct sip_msg *msg, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;
	JANUS_LOG(LOG_INFO, "[SIPre-%s] janus_sipre_cb_incoming\n", session->account.username);

	/* TODO Handle */
}

/* Called when an SDP offer is received (got offer: true) or being sent (got_offer: false) */
int janus_sipre_cb_offer(struct mbuf **mbp, const struct sip_msg *msg, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;
	JANUS_LOG(LOG_INFO, "[SIPre-%s] janus_sipre_cb_offer\n", session->account.username);

	struct sdp_session *sdp = NULL;
	const bool got_offer = mbuf_get_left(msg->mb);
	if(got_offer) {
		int err = sdp_decode(sdp, msg->mb, true);
		if(err) {
			JANUS_LOG(LOG_ERR, "unable to decode SDP offer: %s\n", strerror(err));
			return err;
		}
		JANUS_LOG(LOG_INFO, "SDP offer received\n");
		/* TODO Handle */
	} else {
		JANUS_LOG(LOG_INFO, "sending SDP offer\n");
	}

	return sdp_encode(mbp, sdp, !got_offer);
}


/* called when an SDP answer is received */
int janus_sipre_cb_answer(const struct sip_msg *msg, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;
	JANUS_LOG(LOG_INFO, "[SIPre-%s] janus_sipre_cb_answer\n", session->account.username);

	JANUS_LOG(LOG_INFO, "SDP answer received\n");

	struct sdp_session *sdp = NULL;
	int err = sdp_decode(sdp, msg->mb, false);
	if(err) {
		JANUS_LOG(LOG_ERR, "unable to decode SDP answer: %s\n", strerror(err));
		return err;
	}

	/* TODO Handle */
	return 0;
}

/* called when the session is established */
void janus_sipre_cb_established(const struct sip_msg *msg, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;
	JANUS_LOG(LOG_INFO, "[SIPre-%s] janus_sipre_cb_established\n", session->account.username);

	/* TODO Handle */
}

/* Called when the session fails to connect or is terminated by the peer */
void janus_sipre_cb_closed(int err, const struct sip_msg *msg, void *arg) {
	janus_sipre_session *session = (janus_sipre_session *)arg;

	if(err) {
		JANUS_LOG(LOG_ERR, "[SIPre-%s] janus_sipre_cb_closed: %s\n", session->account.username, strerror(err));
	} else {
		JANUS_LOG(LOG_INFO, "[SIPre-%s] janus_sipre_cb_closed: %u %s\n", session->account.username, msg->scode, (char *)&msg->reason.p);
	}

	/* TODO Handle */
}

/* Called when all SIP transactions are completed */
void janus_sipre_cb_exit(void *arg) {
	/* Stop libre main loop */
	re_cancel();
}

/* Callback to implement SIP requests in the re_main loop thread */
void janus_sipre_mqueue_handler(int id, void *data, void *arg) {
	JANUS_LOG(LOG_FATAL, "janus_sipre_mqueue_handler: %d\n", id);
	switch((janus_sipre_mqueue_event)id) {
		case janus_sipre_mqueue_event_do_init: {
			JANUS_LOG(LOG_INFO, "Initializing SIP transports\n");
			struct sa laddr, laddrs;
			sa_set_str(&laddr, local_ip, 0);
			sa_set_str(&laddrs, local_ip, 0);
			int err = 0;
			err |= sip_transp_add(sipstack, SIP_TRANSP_UDP, &laddr);
			err |= sip_transp_add(sipstack, SIP_TRANSP_TCP, &laddr);
			if(err) {
				JANUS_LOG(LOG_ERR, "Failed to initialize libre SIP transports: %d (%s)\n", err, strerror(err));
				return;
			}
			err = tls_alloc(&tls, TLS_METHOD_SSLV23, NULL, NULL);
			err |= sip_transp_add(sipstack, SIP_TRANSP_TLS, &laddrs, tls);
			if(err) {
				mem_deref(sipstack);
				mem_deref(tls);
				JANUS_LOG(LOG_ERR, "Failed to initialize libre SIPS transports: %d (%s)\n", err, strerror(err));
				return;
			}
			mem_deref(tls);
			break;
		}
		case janus_sipre_mqueue_event_do_register: {
			janus_sipre_session *session = (janus_sipre_session *)data;
			JANUS_LOG(LOG_INFO, "[SIPre-%s] Sending REGISTER\n", session->account.username);
			int err = sipreg_register(&session->stack.reg, sipstack,
				session->account.proxy,
				session->account.identity, session->account.identity, 3600,
				session->account.display_name ? session->account.display_name : session->account.username, NULL, 0, 0,
				janus_sipre_cb_auth, session, FALSE,
				janus_sipre_cb_register, session, NULL, NULL);
			if(err < 0) {
				/* TODO Send event and handle accordingly */
				JANUS_LOG(LOG_ERR, "Error attempting to REGISTER...\n");
			}
			break;
		}
		case janus_sipre_mqueue_event_do_exit:
			/* We're done, here, break the loop */
			re_cancel();
			break;
		default:
			/* Shouldn't happen */
			break;
	}
}