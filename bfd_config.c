/*********************************************************************
 * Copyright 2017-2018 Network Device Education Foundation, Inc. ("NetDEF")
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * bfd_config.c: implements the BFD daemon configuration handling.
 *
 * Authors
 * -------
 * Rafael Zalamena <rzalamena@opensourcerouting.org>
 */

#include <json-c/json.h>

#include <string.h>

#include "bfd.h"

/*
 * Definitions
 */
#define JSON_FOREACH(jo, joi, join)                                            \
	/* struct json_object *jo; */                                          \
	/* struct json_object_iterator joi; */                                 \
	/* struct json_object_iterator join; */                                \
	for ((joi) = json_object_iter_begin((jo)),                             \
	    (join) = json_object_iter_end((jo));                               \
	     json_object_iter_equal(&(joi), &(join)) == 0;                     \
	     json_object_iter_next(&(joi)))

enum peer_list_type {
	PLT_IPV4,
	PLT_IPV6,
	PLT_LABEL,
};


/*
 * Prototypes
 */
int parse_config_json(struct json_object *jo, bpc_handle h, void *arg);
int parse_list(struct json_object *jo, enum peer_list_type plt, bpc_handle h, void *arg);
int parse_peer_config(struct json_object *jo, struct bfd_peer_cfg *bpc);
int parse_peer_label_config(struct json_object *jo, struct bfd_peer_cfg *bpc);

int config_add(struct bfd_peer_cfg *bpc, void *arg);
int config_del(struct bfd_peer_cfg *bpc, void *arg);

int json_object_add_string(struct json_object *jo, const char *key,
			   const char *str);
int json_object_add_bool(struct json_object *jo, const char *key, bool boolean);
int json_object_add_int(struct json_object *jo, const char *key, int64_t value);
int json_object_add_float(struct json_object *jo, const char *key, float value);
int json_object_add_peer(struct json_object *jo, bfd_session *bs);

void pl_free(struct peer_label *pl);


/*
 * Implementation
 */
int config_add(struct bfd_peer_cfg *bpc, void *arg __attribute__((unused)))
{
	return ptm_bfd_sess_new(bpc) == NULL;
}

int config_del(struct bfd_peer_cfg *bpc, void *arg __attribute__((unused)))
{
	return ptm_bfd_ses_del(bpc) != 0;
}

int parse_config_json(struct json_object *jo, bpc_handle h, void *arg)
{
	const char *key, *sval;
	struct json_object *jo_val;
	struct json_object_iterator joi, join;
	int error = 0;

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strcmp(key, "ipv4") == 0) {
			error += parse_list(jo_val, PLT_IPV4, h, arg);
		} else if (strcmp(key, "ipv6") == 0) {
			error += parse_list(jo_val, PLT_IPV6, h, arg);
		} else if (strcmp(key, "label") == 0) {
			error += parse_list(jo_val, PLT_LABEL, h, arg);
		} else {
			sval = json_object_get_string(jo_val);
			log_warning("%s:%d invalid configuration: %s\n",
				    __FUNCTION__, __LINE__, sval);
			error++;
		}
	}

	return error;
}

int parse_config(const char *fname)
{
	struct json_object *jo;

	jo = json_object_from_file(fname);
	if (jo == NULL) {
		return -1;
	}

	return parse_config_json(jo, config_add, NULL);
}

int parse_list(struct json_object *jo, enum peer_list_type plt, bpc_handle h, void *arg)
{
	struct json_object *jo_val;
	struct bfd_peer_cfg bpc;
	int allen, idx;
	int error = 0, result;

	allen = json_object_array_length(jo);
	for (idx = 0; idx < allen; idx++) {
		jo_val = json_object_array_get_idx(jo, idx);

		/* Set defaults. */
		memset(&bpc, 0, sizeof(bpc));
		bpc.bpc_detectmultiplier = BFD_DEFDETECTMULT;
		bpc.bpc_recvinterval = BFD_DEFREQUIREDMINRX;
		bpc.bpc_txinterval = BFD_DEFDESIREDMINTX;
		bpc.bpc_echointerval = BFD_DEF_REQ_MIN_ECHO;

		switch (plt) {
		case PLT_IPV4:
			log_debug("ipv4 peers %d:\n", allen);
			bpc.bpc_ipv4 = true;
			break;
		case PLT_IPV6:
			log_debug("ipv6 peers %d:\n", allen);
			bpc.bpc_ipv4 = false;
			break;
		case PLT_LABEL:
			log_debug("label peers %d:\n", allen);
			if (parse_peer_label_config(jo_val, &bpc) != 0) {
				error++;
				continue;
			}
			break;

		default:
			error++;
			log_error("%s:%d: unsupported peer type\n",
				__FUNCTION__, __LINE__);
			break;
		}

		result = parse_peer_config(jo_val, &bpc);
		error += result;
		if (result == 0)
			error += (h(&bpc, arg) != 0);
	}

	return error;
}

int parse_peer_config(struct json_object *jo, struct bfd_peer_cfg *bpc)
{
	const char *key, *sval;
	struct json_object *jo_val;
	struct json_object_iterator joi, join;
	int family_type = (bpc->bpc_ipv4) ? AF_INET : AF_INET6;
	int error = 0;

	log_debug("\tpeer: %s\n", bpc->bpc_ipv4 ? "ipv4" : "ipv6");

	JSON_FOREACH (jo, joi, join) {
		key = json_object_iter_peek_name(&joi);
		jo_val = json_object_iter_peek_value(&joi);

		if (strcmp(key, "multihop") == 0) {
			bpc->bpc_mhop = json_object_get_boolean(jo_val);
			log_debug("\tmultihop: %s\n",
				  bpc->bpc_mhop ? "true" : "false");
		} else if (strcmp(key, "peer-address") == 0) {
			sval = json_object_get_string(jo_val);
			if (strtosa(sval, &bpc->bpc_peer) != 0
			    || bpc->bpc_peer.sa_sin.sin_family != family_type) {
				log_info(
					"%s:%d failed to parse peer-address '%s'",
					__FUNCTION__, __LINE__, sval);
				error++;
			}
			log_debug("\tpeer-address: %s\n", sval);
		} else if (strcmp(key, "local-address") == 0) {
			sval = json_object_get_string(jo_val);
			if (strtosa(sval, &bpc->bpc_local) != 0
			    || bpc->bpc_local.sa_sin.sin_family != family_type) {
				log_info(
					"%s:%d failed to parse local-address '%s'",
					__FUNCTION__, __LINE__, sval);
				error++;
			}
			log_debug("\tlocal-address: %s\n", sval);
		} else if (strcmp(key, "local-interface") == 0) {
			bpc->bpc_has_localif = true;
			sval = json_object_get_string(jo_val);
			if (strxcpy(bpc->bpc_localif, sval,
				    sizeof(bpc->bpc_localif))
			    > sizeof(bpc->bpc_localif)) {
				log_debug(
					"\tlocal-interface: %s (truncated)\n");
				error++;
			} else {
				log_debug("\tlocal-interface: %s\n", sval);
			}
		} else if (strcmp(key, "vxlan") == 0) {
			bpc->bpc_vxlan = json_object_get_int64(jo_val);
			bpc->bpc_has_vxlan = true;
			log_debug("\tvxlan: %ld\n", bpc->bpc_vxlan);
		} else if (strcmp(key, "vrf-name") == 0) {
			bpc->bpc_has_vrfname = true;
			sval = json_object_get_string(jo_val);
			if (strxcpy(bpc->bpc_vrfname, sval,
				    sizeof(bpc->bpc_vrfname))
			    > sizeof(bpc->bpc_vrfname)) {
				log_debug("\tvrf-name: %s (truncated)\n", sval);
				error++;
			} else {
				log_debug("\tvrf-name: %s\n", sval);
			}
		} else if (strcmp(key, "discriminator") == 0) {
                        bpc->bpc_has_discr = true;
                        bpc->bpc_discr = json_object_get_int(jo_val);
			log_debug("\tdiscriminator: %d\n", bpc->bpc_discr);
		} else if (strcmp(key, "detect-multiplier") == 0) {
			bpc->bpc_detectmultiplier =
				json_object_get_int64(jo_val);
			bpc->bpc_has_detectmultiplier = true;
			log_debug("\tdetect-multiplier: %llu\n",
				  bpc->bpc_detectmultiplier);
		} else if (strcmp(key, "receive-interval") == 0) {
			bpc->bpc_recvinterval = json_object_get_int64(jo_val);
			bpc->bpc_has_recvinterval = true;
			log_debug("\treceive-interval: %llu\n",
				  bpc->bpc_recvinterval);
		} else if (strcmp(key, "transmit-interval") == 0) {
			bpc->bpc_txinterval = json_object_get_int64(jo_val);
			bpc->bpc_has_txinterval = true;
			log_debug("\ttransmit-interval: %llu\n",
				  bpc->bpc_txinterval);
		} else if (strcmp(key, "echo-interval") == 0) {
			bpc->bpc_echointerval = json_object_get_int64(jo_val);
			bpc->bpc_has_echointerval = true;
			log_debug("\techo-interval: %llu\n",
				  bpc->bpc_echointerval);
		} else if (strcmp(key, "create-only") == 0) {
			bpc->bpc_createonly = json_object_get_boolean(jo_val);
			log_debug("\tcreate-only: %s\n",
				  bpc->bpc_createonly ? "true" : "false");
		} else if (strcmp(key, "shutdown") == 0) {
			bpc->bpc_shutdown = json_object_get_boolean(jo_val);
			log_debug("\tshutdown: %s\n",
				  bpc->bpc_shutdown ? "true" : "false");
		} else if (strcmp(key, "echo-mode") == 0) {
			bpc->bpc_echo = json_object_get_boolean(jo_val);
			log_debug("\techo-mode: %s\n",
				  bpc->bpc_echo ? "true" : "false");
		} else if (strcmp(key, "label") == 0) {
			bpc->bpc_has_label = true;
			sval = json_object_get_string(jo_val);
			if (strxcpy(bpc->bpc_label, sval,
				    sizeof(bpc->bpc_label))
			    > sizeof(bpc->bpc_label)) {
				log_debug("\tlabel: %s (truncated)\n", sval);
				error++;
			} else {
				log_debug("\tlabel: %s\n", sval);
			}
		} else if (strcmp(key, "track-sla") == 0) {
			bpc->bpc_track_sla = json_object_get_boolean(jo_val);
			log_debug("\ttrack-sla: %s\n",
				  bpc->bpc_track_sla ? "true" : "false");
		} else {
			sval = json_object_get_string(jo_val);
			log_warning("%s:%d invalid configuration: '%s: %s'\n",
				    __FUNCTION__, __LINE__, key, sval);
			error++;
		}
	}

	if (bpc->bpc_peer.sa_sin.sin_family == 0) {
		log_debug("%s:%d no peer address provided\n", __FUNCTION__,
			  __LINE__);
		error++;
	}

	return error;
}

int parse_peer_label_config(struct json_object *jo, struct bfd_peer_cfg *bpc)
{
	struct peer_label *pl;
	struct json_object *label;
	const char *sval;

	/* Get label and translate it to BFD daemon key. */
	if (!json_object_object_get_ex(jo, "label", &label)) {
		return 1;
	}

	sval = json_object_get_string(label);

	pl = pl_find(sval);
	if (pl == NULL)
		return 1;

	log_debug("\tpeer-label: %s\n", sval);

	/* Translate the label into BFD address keys. */
	bpc->bpc_ipv4 = !BFD_CHECK_FLAG(pl->pl_bs->flags, BFD_SESS_FLAG_IPV6);
	bpc->bpc_mhop = BFD_CHECK_FLAG(pl->pl_bs->flags, BFD_SESS_FLAG_MH);
	if (bpc->bpc_mhop) {
		bpc->bpc_peer = pl->pl_bs->mhop.peer;
		bpc->bpc_local = pl->pl_bs->mhop.local;
		if (pl->pl_bs->mhop.vrf_name[0]) {
			bpc->bpc_has_vrfname = true;
			strxcpy(bpc->bpc_vrfname, pl->pl_bs->mhop.vrf_name,
				sizeof(bpc->bpc_vrfname));
		}
	} else {
		bpc->bpc_peer = pl->pl_bs->shop.peer;
		if (pl->pl_bs->shop.port_name[0]) {
			bpc->bpc_has_localif = true;
			strxcpy(bpc->bpc_localif, pl->pl_bs->shop.port_name,
				sizeof(bpc->bpc_localif));
		}
	}

	return 0;
}


/*
 * Control socket JSON parsing.
 */
int config_request_add(const char *jsonstr)
{
	struct json_object *jo;

	jo = json_tokener_parse(jsonstr);
	if (jo == NULL)
		return -1;

	return parse_config_json(jo, config_add, NULL);
}

int config_request_del(const char *jsonstr)
{
	struct json_object *jo;

	jo = json_tokener_parse(jsonstr);
	if (jo == NULL)
		return -1;

	return parse_config_json(jo, config_del, NULL);
}

char *config_response(const char *status, const char *error)
{
	struct json_object *resp, *jo;
	char *jsonstr;

	resp = json_object_new_object();
	if (resp == NULL)
		return NULL;

	/* Add 'status' response key. */
	jo = json_object_new_string(status);
	if (jo == NULL) {
		json_object_put(resp);
		return NULL;
	}

	json_object_object_add(resp, "status", jo);

	/* Add 'error' response key. */
	if (error != NULL) {
		jo = json_object_new_string(error);
		if (jo == NULL) {
			json_object_put(resp);
			return NULL;
		}

		json_object_object_add(resp, "error", jo);
	}

	/* Generate JSON response. */
	jsonstr = strdup(
		json_object_to_json_string_ext(resp, BFDD_JSON_CONV_OPTIONS));
	json_object_put(resp);

	return jsonstr;
}

char *config_notify_sla(bfd_session *bs)
{
	struct json_object *resp;
	char *jsonstr;

	resp = json_object_new_object();
	if (resp == NULL)
		return NULL;

	json_object_add_string(resp, "op", BCM_NOTIFY_PEER_SLA_UPDATE);

	/* Add status information */
	json_object_add_int(resp, "id", bs->discrs.my_discr);
	json_object_add_int(resp, "remote-id", bs->discrs.my_discr);

        json_object_add_int(resp, "latency", bs->sla.lattency);
        json_object_add_int(resp, "jitter", bs->sla.jitter);
        json_object_add_float(resp, "pkt_loss", bs->sla.pkt_loss);

	/* Generate JSON response. */
	jsonstr = strdup(
		json_object_to_json_string_ext(resp, BFDD_JSON_CONV_OPTIONS));
	json_object_put(resp);

	return jsonstr;
}

char *config_notify(bfd_session *bs)
{
	struct json_object *resp;
	char *jsonstr;
	time_t now;

	resp = json_object_new_object();
	if (resp == NULL)
		return NULL;

	json_object_add_string(resp, "op", BCM_NOTIFY_PEER_STATUS);

	json_object_add_peer(resp, bs);

	/* Add status information */
	json_object_add_int(resp, "id", bs->discrs.my_discr);
	json_object_add_int(resp, "remote-id", bs->discrs.my_discr);

	switch (bs->ses_state) {
	case PTM_BFD_UP:
		json_object_add_string(resp, "state", "up");

		now = get_monotime(NULL);
		json_object_add_int(resp, "uptime", now - bs->uptime.tv_sec);
		break;
	case PTM_BFD_ADM_DOWN:
		json_object_add_string(resp, "state", "adm-down");
		break;
	case PTM_BFD_DOWN:
		json_object_add_string(resp, "state", "down");

		now = get_monotime(NULL);
		json_object_add_int(resp, "downtime",
				    now - bs->downtime.tv_sec);
		break;
	case PTM_BFD_INIT:
		json_object_add_string(resp, "state", "init");
		break;

	default:
		json_object_add_string(resp, "state", "unknown");
		break;
	}

	json_object_add_int(resp, "diagnostics", bs->local_diag);
	json_object_add_int(resp, "remote-diagnostics", bs->remote_diag);

	/* Generate JSON response. */
	jsonstr = strdup(
		json_object_to_json_string_ext(resp, BFDD_JSON_CONV_OPTIONS));
	json_object_put(resp);

	return jsonstr;
}

char *config_notify_config(const char *op, bfd_session *bs)
{
	struct json_object *resp;
	char *jsonstr;

	resp = json_object_new_object();
	if (resp == NULL)
		return NULL;

	json_object_add_string(resp, "op", op);

	json_object_add_peer(resp, bs);

	/* On peer deletion we don't need to add any additional information. */
	if (strcmp(op, BCM_NOTIFY_CONFIG_DELETE) == 0) {
		goto skip_config;
	}

	json_object_add_int(resp, "detect-multiplier", bs->detect_mult);
	json_object_add_int(resp, "receive-interval",
			    bs->timers.required_min_rx / 1000);
	json_object_add_int(resp, "transmit-interval",
			    bs->up_min_tx / 1000);
	json_object_add_int(resp, "echo-interval",
			    bs->timers.required_min_echo / 1000);

	json_object_add_int(resp, "remote-detect-multiplier",
			    bs->remote_detect_mult);
	json_object_add_int(resp, "remote-receive-interval",
			    bs->remote_timers.required_min_rx / 1000);
	json_object_add_int(resp, "remote-transmit-interval",
			    bs->remote_timers.desired_min_tx / 1000);
	json_object_add_int(resp, "remote-echo-interval",
			    bs->remote_timers.required_min_echo / 1000);

	json_object_add_bool(resp, "echo-mode",
			     BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_ECHO));
	json_object_add_bool(resp, "shutdown",
			     BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_SHUTDOWN));

skip_config:
	/* Generate JSON response. */
	jsonstr = strdup(
		json_object_to_json_string_ext(resp, BFDD_JSON_CONV_OPTIONS));
	json_object_put(resp);

	return jsonstr;
}

int config_notify_request(struct bfd_control_socket *bcs, const char *jsonstr,
			  bpc_handle bh)
{
	struct json_object *jo;

	jo = json_tokener_parse(jsonstr);
	if (jo == NULL)
		return -1;

	return parse_config_json(jo, bh, bcs);
}


/*
 * JSON helper functions
 */
int json_object_add_string(struct json_object *jo, const char *key,
			   const char *str)
{
	struct json_object *jon;

	jon = json_object_new_string(str);
	if (jon == NULL) {
		json_object_put(jon);
		return -1;
	}

	json_object_object_add(jo, key, jon);
	return 0;
}

int json_object_add_bool(struct json_object *jo, const char *key, bool boolean)
{
	struct json_object *jon;

	jon = json_object_new_boolean(boolean);
	if (jon == NULL) {
		json_object_put(jon);
		return -1;
	}

	json_object_object_add(jo, key, jon);
	return 0;
}

int json_object_add_int(struct json_object *jo, const char *key, int64_t value)
{
	struct json_object *jon;

	jon = json_object_new_int64(value);
	if (jon == NULL) {
		json_object_put(jon);
		return -1;
	}

	json_object_object_add(jo, key, jon);
	return 0;
}

int json_object_add_float(struct json_object *jo, const char *key, float value)
{
	struct json_object *jon;

	jon = json_object_new_double(value);
	if (jon == NULL) {
		json_object_put(jon);
		return -1;
	}

	json_object_object_add(jo, key, jon);
	return 0;
}

int json_object_add_peer(struct json_object *jo, bfd_session *bs)
{
	/* Add peer 'key' information. */
	json_object_add_bool(jo, "ipv6",
			     BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_IPV6));
	json_object_add_bool(jo, "multihop",
			     BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_MH));
	if (BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_MH)) {
		json_object_add_string(jo, "peer-address",
				       satostr(&bs->mhop.peer));
		json_object_add_string(jo, "local-address",
				       satostr(&bs->mhop.local));
		if (strlen(bs->mhop.vrf_name) > 0)
			json_object_add_string(jo, "vrf-name",
					       bs->mhop.vrf_name);
	} else {
		json_object_add_string(jo, "peer-address",
				       satostr(&bs->shop.peer));
		if (bs->local_ip.sa_sin.sin_family != AF_UNSPEC)
			json_object_add_string(jo, "local-address",
					       satostr(&bs->local_ip));
		if (strlen(bs->shop.port_name) > 0)
			json_object_add_string(jo, "local-interface",
					       bs->shop.port_name);
	}

	if (bs->pl)
		json_object_add_string(jo, "label", bs->pl->pl_label);

	return 0;
}


/*
 * Label handling
 */
struct peer_label *pl_find(const char *label)
{
	struct peer_label *pl;

	TAILQ_FOREACH (pl, &bglobal.bg_pllist, pl_entry) {
		if (strcmp(pl->pl_label, label) != 0)
			continue;

		return pl;
	}

	return NULL;
}

struct peer_label *pl_new(const char *label, bfd_session *bs)
{
	struct peer_label *pl;

	pl = calloc(1, sizeof(*pl));
	if (pl == NULL)
		return NULL;

	if (strxcpy(pl->pl_label, label, sizeof(pl->pl_label))
	    > sizeof(pl->pl_label)) {
		log_warning("%s:%d: label was truncated\n", __FUNCTION__,
			    __LINE__);
	}
	pl->pl_bs = bs;
	bs->pl = pl;

	TAILQ_INSERT_HEAD(&bglobal.bg_pllist, pl, pl_entry);

	return pl;
}

void pl_free(struct peer_label *pl)
{
	/* Remove the pointer back. */
	pl->pl_bs->pl = NULL;

	TAILQ_REMOVE(&bglobal.bg_pllist, pl, pl_entry);
	free(pl);
}
