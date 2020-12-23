/*********************************************************************
 * Copyright 2013 Cumulus Networks, LLC.  All rights reserved.
 * Copyright 2014,2015,2016,2017 Cumulus Networks, Inc.  All rights reserved.
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
 * bfd.c: implements the BFD protocol.
 *
 * Authors
 * -------
 * Shrijeet Mukherjee [shm@cumulusnetworks.com]
 * Kanna Rajagopal [kanna@cumulusnetworks.com]
 * Radhika Mahankali [Radhika@cumulusnetworks.com]
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>

#include "uthash.h"
#include "bfd.h"

bfd_diag_str_list diag_list[] = {
	{.str = "NeighDown", .type = BFD_DIAGNEIGHDOWN},
	{.str = "DetectTime", .type = BFD_DIAGDETECTTIME},
	{.str = "AdminDown", .type = BFD_DIAGADMINDOWN},
	{.str = NULL},
};

bfd_state_str_list state_list[] = {
	{.str = "AdminDown", .type = PTM_BFD_ADM_DOWN},
	{.str = "Down", .type = PTM_BFD_DOWN},
	{.str = "Init", .type = PTM_BFD_INIT},
	{.str = "Up", .type = PTM_BFD_UP},
	{.str = NULL},
};

struct bfd_vrf *vrf_hash = NULL;
struct bfd_iface *iface_hash = NULL;

bfd_session *session_hash = NULL;    /* Find session from discriminator */
bfd_session *peer_hash = NULL;       /* Find session from peer address */
bfd_session *local_peer_hash = NULL; /* Find session from peer and local
				      * address */


/*
 * Prototypes
 */

uint32_t ptm_bfd_gen_ID(void);
void ptm_bfd_echo_xmt_TO(bfd_session *bfd);
void bfd_xmt_cb(evutil_socket_t sd, short ev, void *arg);
void bfd_echo_xmt_cb(evutil_socket_t sd, short ev, void *arg);
void bfd_recvtimer_cb(evutil_socket_t sd, short ev, void *arg);
void bfd_echo_recvtimer_cb(evutil_socket_t sd, short ev, void *arg);
void bfd_session_free(bfd_session *bs);
bfd_session *bfd_session_new(int sd);
bfd_session *bfd_find_disc(struct sockaddr_any *sa, uint32_t ldisc);
int bfd_session_update(bfd_session *bs, struct bfd_peer_cfg *bpc);


/*
 * Functions
 */

static char *get_diag_str(int diag)
{
	for (int i = 0; diag_list[i].str; i++) {
		if (diag_list[i].type == diag)
			return diag_list[i].str;
	}
	return "N/A";
}

bfd_session *bs_session_find(uint32_t discr)
{
	bfd_session *bs;

	HASH_FIND(sh, session_hash, &discr, sizeof(discr), bs);

	return bs;
}

int ptm_bfd_fetch_ifindex(const char *ifname)
{
	struct ifreq ifr;

	if (strxcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name))
	    > sizeof(ifr.ifr_name)) {
		CRITLOG("Interface name %s truncated", ifr.ifr_name);
	}

	if (ioctl(bglobal.bg_shop, SIOCGIFINDEX, &ifr) == -1) {
		CRITLOG("Getting ifindex for %s failed: %s", ifname,
			strerror(errno));
		return -1;
	}

	return ifr.ifr_ifindex;
}

static void ptm_bfd_fetch_local_mac(const char *ifname, uint8_t *mac)
{
	struct ifreq ifr;

	if (strxcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name))
	    > sizeof(ifr.ifr_name)) {
		CRITLOG("Interface name %s truncated", ifr.ifr_name);
	}

	if (ioctl(bglobal.bg_shop, SIOCGIFHWADDR, &ifr) == -1) {
		CRITLOG("Getting mac address for %s failed: %s", ifname,
			strerror(errno));
		return;
	}

	memcpy(mac, ifr.ifr_hwaddr.sa_data, ETHERNET_ADDRESS_LENGTH);
}

/* Was _fetch_portname_from_ifindex() */
void fetch_portname_from_ifindex(int ifindex, char *ifname, size_t ifnamelen)
{
	struct ifreq ifr;

	ifname[0] = 0;

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_ifindex = ifindex;

	if (ioctl(bglobal.bg_shop, SIOCGIFNAME, &ifr) == -1) {
		CRITLOG("Getting ifname for ifindex %d failed: %s", ifindex,
			strerror(errno));
		return;
	}

	strxcpy(ifname, ifr.ifr_name, ifnamelen);
}

uint32_t ptm_bfd_gen_ID(void)
{
	static uint32_t sessionID = 1;
	return (sessionID++);
}

void ptm_bfd_start_xmt_timer(bfd_session *bfd, bool is_echo)
{
	uint64_t jitter, xmt_TO;
	int maxpercent;

	xmt_TO = is_echo ? bfd->echo_xmt_TO : bfd->xmt_TO;

	/*
	 * From section 6.5.2: trasmit interval should be randomly jittered
	 * between
	 * 75% and 100% of nominal value, unless detect_mult is 1, then should
	 * be
	 * between 75% and 90%.
	 */
	maxpercent = (bfd->detect_mult == 1) ? 16 : 26;
	jitter = (xmt_TO * (75 + (random() % maxpercent))) / 100;
	/* XXX remove that division above */

	if (is_echo)
		bfd_echo_xmttimer_update(bfd, jitter);
	else
		bfd_xmttimer_update(bfd, jitter);
}

void ptm_bfd_echo_xmt_TO(bfd_session *bfd)
{
	/* Send the scheduled echo  packet */
	ptm_bfd_echo_snd(bfd);

        gettimeofday(&bfd->xmit_tv, NULL);
	/* Restart the timer for next time */
	ptm_bfd_start_xmt_timer(bfd, true);
}

void ptm_bfd_xmt_TO(bfd_session *bfd, int fbit)
{
	/* Send the scheduled control packet */
	ptm_bfd_snd(bfd, fbit);

        gettimeofday(&bfd->xmit_tv, NULL);
	/* Restart the timer for next time */
	ptm_bfd_start_xmt_timer(bfd, false);
}

void ptm_bfd_echo_stop(bfd_session *bfd, int polling)
{
	bfd->echo_xmt_TO = 0;
	bfd->echo_detect_TO = 0;
	BFD_UNSET_FLAG(bfd->flags, BFD_SESS_FLAG_ECHO_ACTIVE);

	bfd_echo_xmttimer_delete(bfd);
	bfd_echo_recvtimer_delete(bfd);

	if (polling) {
		bfd->polling = polling;
		bfd->new_timers.desired_min_tx = bfd->up_min_tx;
		bfd->new_timers.required_min_rx = bfd->timers.required_min_rx;
		ptm_bfd_snd(bfd, 0);
	}
}

void ptm_bfd_echo_start(bfd_session *bfd)
{
	bfd->echo_detect_TO = (bfd->remote_detect_mult * bfd->echo_xmt_TO);
	ptm_bfd_echo_xmt_TO(bfd);

	bfd->polling = 1;
	bfd->new_timers.desired_min_tx = bfd->up_min_tx;
	bfd->new_timers.required_min_rx = bfd->timers.required_min_rx;
	ptm_bfd_snd(bfd, 0);
}

void ptm_bfd_ses_up(bfd_session *bfd)
{
	bfd->local_diag = 0;
	bfd->ses_state = PTM_BFD_UP;
	bfd->polling = 1;
	get_monotime(&bfd->uptime);

	/* If the peer is capable to receiving Echo pkts */
	if (bfd->echo_xmt_TO && !BFD_CHECK_FLAG(bfd->flags, BFD_SESS_FLAG_MH)) {
		ptm_bfd_echo_start(bfd);
	} else {
		bfd->new_timers.desired_min_tx = bfd->up_min_tx;
		bfd->new_timers.required_min_rx = bfd->timers.required_min_rx;
		ptm_bfd_snd(bfd, 0);
	}

	control_notify(bfd);

	INFOLOG("Session 0x%x up peer %s", bfd->discrs.my_discr,
		satostr(&bfd->shop.peer));
}

void ptm_bfd_ses_dn(bfd_session *bfd, uint8_t diag)
{
	int old_state = bfd->ses_state;

	bfd->local_diag = diag;
	bfd->discrs.remote_discr = 0;
	bfd->ses_state = PTM_BFD_DOWN;
	bfd->polling = 0;
	bfd->demand_mode = 0;
	get_monotime(&bfd->downtime);

	ptm_bfd_snd(bfd, 0);

	/* only signal clients when going from up->down state */
	if (old_state == PTM_BFD_UP)
		control_notify(bfd);

	INFOLOG("Session 0x%x down peer %s Rsn %s prev st %s",
		bfd->discrs.my_discr, satostr(&bfd->shop.peer),
		get_diag_str(bfd->local_diag), state_list[old_state].str);

	/* Stop echo packet transmission if they are active */
	if (BFD_CHECK_FLAG(bfd->flags, BFD_SESS_FLAG_ECHO_ACTIVE)) {
		ptm_bfd_echo_stop(bfd, 0);
	}
}

static int ptm_bfd_get_vrf_name(char *port_name, char *vrf_name)
{
	struct bfd_iface *iface;
	struct bfd_vrf *vrf;

	if ((port_name == NULL) || (vrf_name == NULL)) {
		return -1;
	}

	HASH_FIND(ifh, iface_hash, port_name, strlen(port_name), iface);

	if (iface) {
		HASH_FIND(vh, vrf_hash, &iface->vrf_id, sizeof(iface->vrf_id),
			  vrf);
		if (vrf) {
			strxcpy(vrf_name, vrf->name, sizeof(vrf->name));
			return 0;
		}
	}
	return -1;
}

bfd_session *bfd_find_disc(struct sockaddr_any *sa, uint32_t ldisc)
{
	bfd_session *bs;

	HASH_FIND(sh, session_hash, &ldisc, sizeof(ldisc), bs);
	if (bs == NULL)
		return NULL;

	/* Remove unused fields. */
	switch (sa->sa_sin.sin_family) {
	case AF_INET:
		sa->sa_sin.sin_port = 0;
		break;
	case AF_INET6:
		sa->sa_sin6.sin6_port = 0;
		break;
	}

	if (bs->discrs.my_discr != ldisc
	    && !memcmp(sa, &bs->shop.peer, sizeof(*sa)))
		return NULL;

	return bs;
}

bfd_session *bfd_find_shop(bfd_shop_key *k)
{
	bfd_session *bs;

	/* Remove unused fields. */
	switch (k->peer.sa_sin.sin_family) {
	case AF_INET:
		k->peer.sa_sin.sin_port = 0;
		break;
	case AF_INET6:
		k->peer.sa_sin6.sin6_port = 0;
		break;
	}

	HASH_FIND(ph, peer_hash, k, sizeof(*k), bs);

	/*
	 * Since the local interface spec is optional, try searching the key
	 * without
	 * it as well.
	 */
	if (bs == NULL) {
		memset(k->port_name, 0, sizeof(k->port_name));
		HASH_FIND(ph, peer_hash, k, sizeof(*k), bs);
	}

	return bs;
}

bfd_session *bfd_find_mhop(bfd_mhop_key *k)
{
	bfd_session *bs;

	/* Remove unused fields. */
	switch (k->peer.sa_sin.sin_family) {
	case AF_INET:
		k->local.sa_sin.sin_port = 0;
		k->peer.sa_sin.sin_port = 0;
		break;
	case AF_INET6:
		k->local.sa_sin.sin_port = 0;
		k->peer.sa_sin6.sin6_port = 0;
		break;
	}

	HASH_FIND(mh, local_peer_hash, k, sizeof(*k), bs);

	return bs;
}

bfd_session *ptm_bfd_sess_find(bfd_pkt_t *cp, char *port_name,
			       struct sockaddr_any *peer,
			       struct sockaddr_any *local, char *vrf_name,
			       bool is_mhop)
{
	bfd_session *l_bfd = NULL;
	bfd_mhop_key mhop;
	bfd_shop_key shop;
	char peer_addr[64];
	char local_addr[64];
	char vrf_name_buf[MAXNAMELEN + 1];

	/* peer, local are in network-byte order */
	strxcpy(peer_addr, satostr(peer), sizeof(peer_addr));
	strxcpy(local_addr, satostr(local), sizeof(local_addr));

	if (cp) {
		if (cp->discrs.remote_discr) {
			uint32_t ldisc = ntohl(cp->discrs.remote_discr);
			/* Your discriminator not zero - use it to find session
			 */
			l_bfd = bfd_find_disc(peer, ldisc);

			if (l_bfd) {
				return (l_bfd);
			}
			DLOG("Can't find session for yourDisc 0x%x from %s",
			     ldisc, peer_addr);
		} else if (BFD_GETSTATE(cp->flags) == PTM_BFD_DOWN
			   || BFD_GETSTATE(cp->flags) == PTM_BFD_ADM_DOWN) {

			if (is_mhop) {
				memset(&mhop, 0, sizeof(mhop));
				mhop.peer = *peer;
				mhop.local = *local;
				if (vrf_name && strlen(vrf_name)) {
					strxcpy(mhop.vrf_name, vrf_name,
						sizeof(mhop.vrf_name));
				} else if (port_name) {
					memset(vrf_name_buf, 0,
					       sizeof(vrf_name_buf));
					if (ptm_bfd_get_vrf_name(port_name,
								 vrf_name_buf)
					    != -1) {
						strxcpy(mhop.vrf_name,
							vrf_name_buf,
							sizeof(mhop.vrf_name));
					}
				}

				/* Your discriminator zero -
				 *     use peer address and local address to
				 * find session */
				l_bfd = bfd_find_mhop(&mhop);
			} else {
				memset(&shop, 0, sizeof(shop));
				shop.peer = *peer;
				if (strlen(port_name))
					strxcpy(shop.port_name, port_name,
						sizeof(shop.port_name));
				/* Your discriminator zero -
				 *      use peer address and port to find
				 * session */
				l_bfd = bfd_find_shop(&shop);
			}
			if (l_bfd) {
				/* XXX maybe remoteDiscr should be checked for
				 * remoteHeard cases */
				return (l_bfd);
			}
		}
		if (is_mhop)
			DLOG("Can't find multi hop session peer/local %s/%s in vrf %s port %s",
			     peer_addr, local_addr,
			     strlen(mhop.vrf_name) ? mhop.vrf_name : "N/A",
			     port_name ? port_name : "N/A");
		else
			DLOG("Can't find single hop session for peer/port %s/%s",
			     peer_addr, port_name);
	} else if (peer->sa_sin.sin_addr.s_addr
		   || !IN6_IS_ADDR_UNSPECIFIED(&peer->sa_sin6.sin6_addr)) {

		if (is_mhop) {
			memset((void *)&mhop, 0, sizeof(bfd_mhop_key));
			mhop.peer = *peer;
			mhop.local = *local;
			if (vrf_name && strlen(vrf_name))
				strxcpy(mhop.vrf_name, vrf_name,
					sizeof(mhop.vrf_name));

			HASH_FIND(mh, local_peer_hash, &mhop, sizeof(mhop),
				  l_bfd);
		} else {
			memset((void *)&shop, 0, sizeof(bfd_shop_key));
			shop.peer = *peer;
			if (strlen(port_name)) {
				strxcpy(shop.port_name, port_name,
					sizeof(shop.port_name));
			}

			HASH_FIND(ph, peer_hash, &shop, sizeof(shop), l_bfd);
		}

		if (l_bfd) {
			/* XXX maybe remoteDiscr should be checked for
			 * remoteHeard cases */
			return (l_bfd);
		}

		DLOG("Can't find session for peer %s\n", peer_addr);
	}

	return (NULL);
}

#if 0  /* TODO VxLAN Support */
static void
_update_vxlan_sess_parms(bfd_session *bfd, bfd_sess_parms *sess_parms)
{
    bfd_session_vxlan_info_t *vxlan_info = &bfd->vxlan_info;
    bfd_parms_list *parms = &sess_parms->parms;

    vxlan_info->vnid = parms->vnid;
    vxlan_info->check_tnl_key = parms->check_tnl_key;
    vxlan_info->forwarding_if_rx = parms->forwarding_if_rx;
    vxlan_info->cpath_down = parms->cpath_down;
    vxlan_info->decay_min_rx = parms->decay_min_rx;

    inet_aton(parms->local_dst_ip, &vxlan_info->local_dst_ip);
    inet_aton(parms->remote_dst_ip, &vxlan_info->peer_dst_ip);

    memcpy(vxlan_info->local_dst_mac, parms->local_dst_mac, ETH_ALEN);
    memcpy(vxlan_info->peer_dst_mac, parms->remote_dst_mac, ETH_ALEN);

    /* The interface may change for Vxlan BFD sessions, so update
     * the local mac and ifindex */
    bfd->ifindex = sess_parms->ifindex;
    memcpy(bfd->local_mac, sess_parms->local_mac, sizeof(bfd->local_mac));
}
#endif /* VxLAN support */

void bfd_xmt_cb(evutil_socket_t sd __attribute__((unused)),
		short ev __attribute__((unused)), void *arg)
{
	bfd_session *bs = arg;

	ptm_bfd_xmt_TO(bs, 0);
}

void bfd_echo_xmt_cb(evutil_socket_t sd __attribute__((unused)),
		     short ev __attribute__((unused)), void *arg)
{
	bfd_session *bs = arg;

	ptm_bfd_echo_xmt_TO(bs);
}

/* Was ptm_bfd_detect_TO() */
void bfd_recvtimer_cb(evutil_socket_t sd __attribute__((unused)),
		      short ev __attribute__((unused)), void *arg)
{
	bfd_session *bs = arg;
	uint8_t old_state;

	old_state = bs->ses_state;

	switch (bs->ses_state) {
	case PTM_BFD_INIT:
	case PTM_BFD_UP:
		ptm_bfd_ses_dn(bs, BFD_DIAGDETECTTIME);
		INFOLOG("%s Detect timeout on session 0x%x with peer %s,"
			" in state %d",
			__FUNCTION__, bs->discrs.my_discr,
			satostr(&bs->shop.peer), bs->ses_state);
		bfd_recvtimer_update(bs);
		break;

	default:
		/* Second detect time expiration, zero remote discr (section
		 * 6.5.1) */
		bs->discrs.remote_discr = 0;
		break;
	}

	if (old_state != bs->ses_state) {
		DLOG("BFD Sess %d [%s] Old State [%s] : New State [%s]",
		     bs->discrs.my_discr, satostr(&bs->shop.peer),
		     state_list[old_state].str, state_list[bs->ses_state].str);
	}
}

/* Was ptm_bfd_echo_detect_TO() */
void bfd_echo_recvtimer_cb(evutil_socket_t sd __attribute__((unused)),
			   short ev __attribute__((unused)), void *arg)
{
	bfd_session *bs = arg;
	uint8_t old_state;

	old_state = bs->ses_state;

	switch (bs->ses_state) {
	case PTM_BFD_INIT:
	case PTM_BFD_UP:
		ptm_bfd_ses_dn(bs, BFD_DIAGDETECTTIME);
		INFOLOG("%s Detect timeout on session 0x%x with peer %s,"
			" in state %d",
			__FUNCTION__, bs->discrs.my_discr,
			satostr(&bs->shop.peer), bs->ses_state);
		break;
	}

	if (old_state != bs->ses_state) {
		DLOG("BFD Sess %d [%s] Old State [%s] : New State [%s]",
		     bs->discrs.my_discr, satostr(&bs->shop.peer),
		     state_list[old_state].str, state_list[bs->ses_state].str);
	}
}

bfd_session *bfd_session_new(int sd)
{
	bfd_session *bs;

	bs = calloc(1, sizeof(*bs));
	if (bs == NULL)
		return NULL;

	bs->up_min_tx = BFD_DEFDESIREDMINTX;
	bs->timers.required_min_rx = BFD_DEFREQUIREDMINRX;
	bs->timers.required_min_echo = BFD_DEF_REQ_MIN_ECHO;
	bs->detect_mult = BFD_DEFDETECTMULT;
	bs->mh_ttl = BFD_DEF_MHOP_TTL;

	bfd_recvtimer_assign(bs, bfd_recvtimer_cb, sd);
	bfd_echo_recvtimer_assign(bs, bfd_echo_recvtimer_cb, sd);
	bfd_xmttimer_assign(bs, bfd_xmt_cb);
	bfd_echo_xmttimer_assign(bs, bfd_echo_xmt_cb);

	bs->sock = sd;
	get_monotime(&bs->uptime);
	bs->downtime = bs->uptime;

	return bs;
}

static void _bfd_session_update(bfd_session *bs, struct bfd_peer_cfg *bpc)
{
	if (bpc->bpc_echo) {
		BFD_SET_FLAG(bs->flags, BFD_SESS_FLAG_ECHO);
		ptm_bfd_echo_start(bs);

		/* Activate/update echo receive timeout timer. */
		bfd_echo_recvtimer_update(bs);
	} else {
		BFD_UNSET_FLAG(bs->flags, BFD_SESS_FLAG_ECHO);
		ptm_bfd_echo_stop(bs, 0);
	}

        if (bpc->bpc_track_sla){
		BFD_SET_FLAG(bs->flags, BFD_SESS_FLAG_TRACK_SLA);
        } else {
		BFD_UNSET_FLAG(bs->flags, BFD_SESS_FLAG_TRACK_SLA);
        }

	if (bpc->bpc_has_txinterval) {
		bs->up_min_tx = bpc->bpc_txinterval * 1000;
	}

	if (bpc->bpc_has_recvinterval) {
		bs->timers.required_min_rx = bpc->bpc_recvinterval * 1000;
	}

	if (bpc->bpc_has_detectmultiplier) {
		bs->detect_mult = bpc->bpc_detectmultiplier;
	}

	if (bpc->bpc_has_echointerval)
		bs->timers.required_min_echo = bpc->bpc_echointerval * 1000;

	if (bpc->bpc_has_label) {
		do {
			/* Check for new label installation */
			if (bs->pl == NULL) {
				if (pl_find(bpc->bpc_label) != NULL) {
					/* Someone is already using it. */
					break;
				}

				pl_new(bpc->bpc_label, bs);
				break;
			}

			/*
			 * Test new label consistency:
			 * - Do nothing if its the same label;
			 * - Check if the future label is already taken;
			 * - Change label;
			 */
			if (strcmp(bpc->bpc_label, bs->pl->pl_label) == 0)
				break;
			if (pl_find(bpc->bpc_label) != NULL)
				break;

			strxcpy(bs->pl->pl_label, bpc->bpc_label,
				sizeof(bs->pl->pl_label));
		} while (0);
	}

	if (bpc->bpc_shutdown) {
		BFD_SET_FLAG(bs->flags, BFD_SESS_FLAG_SHUTDOWN);

		/* Disable all events. */
		bfd_recvtimer_delete(bs);
		bfd_echo_recvtimer_delete(bs);
		bfd_xmttimer_delete(bs);
		bfd_echo_xmttimer_delete(bs);

		/* Change and notify state change. */
		bs->ses_state = PTM_BFD_ADM_DOWN;
		control_notify(bs);

		ptm_bfd_snd(bs, 0);
	} else {
		BFD_UNSET_FLAG(bs->flags, BFD_SESS_FLAG_SHUTDOWN);

		/* Change and notify state change. */
		bs->ses_state = PTM_BFD_DOWN;
		control_notify(bs);

		/* Enable all timers. */
		bfd_recvtimer_update(bs);
		bfd_echo_recvtimer_update(bs);

		bfd_xmttimer_update(bs, bs->xmt_TO);
		if (BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_ECHO))
			bfd_echo_xmttimer_update(bs, bs->echo_xmt_TO);
	}
}

int bfd_session_update(bfd_session *bs, struct bfd_peer_cfg *bpc)
{
	/* User didn't want to update, return failure. */
	if (bpc->bpc_createonly)
		return -1;

	_bfd_session_update(bs, bpc);

	/* TODO add VxLAN support. */

	control_notify_config(BCM_NOTIFY_CONFIG_UPDATE, bs);

	return 0;
}

void bfd_session_free(bfd_session *bs)
{
	if (bs->sock != -1)
		close(bs->sock);

	bfd_recvtimer_delete(bs);
	bfd_echo_recvtimer_delete(bs);
	bfd_xmttimer_delete(bs);
	bfd_echo_xmttimer_delete(bs);

	HASH_DELETE(sh, session_hash, bs);
	if (BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_MH)) {
		HASH_DELETE(mh, local_peer_hash, bs);
	} else {
		HASH_DELETE(ph, peer_hash, bs);
	}

	free(bs);
}

bfd_session *ptm_bfd_sess_new(struct bfd_peer_cfg *bpc)
{
	struct peer_label *pl;
	bfd_session *bfd, *l_bfd;
	bfd_mhop_key mhop;
	bfd_shop_key shop;
	int psock;

	/* check to see if this needs a new session */
	if (bpc->bpc_has_label) {
		pl = pl_find(bpc->bpc_label);
		if (pl) {
			l_bfd = pl->pl_bs;
			goto skip_address_lookup;
		}
	}

	if (bpc->bpc_mhop) {
		memset(&mhop, 0, sizeof(mhop));
		mhop.peer = bpc->bpc_peer;
		mhop.local = bpc->bpc_local;
		if (bpc->bpc_has_vrfname)
			strxcpy(mhop.vrf_name, bpc->bpc_vrfname,
				sizeof(mhop.vrf_name));

		l_bfd = bfd_find_mhop(&mhop);
	} else {
		memset(&shop, 0, sizeof(shop));
		shop.peer = bpc->bpc_peer;
		if (!bpc->bpc_has_vxlan && bpc->bpc_has_localif)
			strxcpy(shop.port_name, bpc->bpc_localif,
				sizeof(shop.port_name));

		l_bfd = bfd_find_shop(&shop);
	}

skip_address_lookup:
	if (l_bfd) {
		/* Requesting a duplicated peer means update configuration. */
		if (bfd_session_update(l_bfd, bpc) == 0)
			return l_bfd;
		else
			return NULL;
	}

	/*
	 * Get socket for transmitting control packets.  Note that if we
	 * could use the destination port (3784) for the source
	 * port we wouldn't need a socket per session.
	 */
	if (bpc->bpc_ipv4) {
		if ((psock = bp_peer_socket(bpc)) == -1) {
			ERRLOG("Can't get socket for new session: %s",
			       strerror(errno));
			return NULL;
		}
	} else {
		if ((psock = bp_peer_socketv6(bpc)) == -1) {
			ERRLOG("Can't get IPv6 socket for new session: %s",
			       strerror(errno));
			return NULL;
		}
	}

	/* Get memory */
	if ((bfd = bfd_session_new(psock)) == NULL) {
		ERRLOG("Can't malloc memory for new session: %s",
		       strerror(errno));
		return NULL;
	}

	if (bpc->bpc_has_localif && !bpc->bpc_mhop) {
		bfd->ifindex = ptm_bfd_fetch_ifindex(bpc->bpc_localif);
		ptm_bfd_fetch_local_mac(bpc->bpc_localif, bfd->local_mac);
	}

	if (bpc->bpc_has_vxlan) {
		BFD_SET_FLAG(bfd->flags, BFD_SESS_FLAG_VXLAN);
	}

	if (bpc->bpc_ipv4 == false) {
		BFD_SET_FLAG(bfd->flags, BFD_SESS_FLAG_IPV6);
	}

        if (bpc->bpc_has_discr) {
            bfd->discrs.my_discr = bpc->bpc_discr;
        } else {
            bfd->discrs.my_discr = ptm_bfd_gen_ID();
        }

	/* Initialize the session */
	bfd->ses_state = PTM_BFD_DOWN;
	bfd->discrs.remote_discr = 0;
	bfd->local_ip = bpc->bpc_local;
	bfd->timers.desired_min_tx = bfd->up_min_tx;
	bfd->detect_TO = (bfd->detect_mult * BFD_DEF_SLOWTX);

	/* Use detect_TO first for slow detection, then use recvtimer_update. */
	bfd_recvtimer_update(bfd);

	HASH_ADD(sh, session_hash, discrs.my_discr, sizeof(uint32_t), bfd);

	if (bpc->bpc_mhop) {
		BFD_SET_FLAG(bfd->flags, BFD_SESS_FLAG_MH);
		bfd->mhop.peer = bpc->bpc_peer;
		bfd->mhop.local = bpc->bpc_local;
		if (bpc->bpc_has_vrfname)
			strxcpy(bfd->mhop.vrf_name, bpc->bpc_vrfname,
				sizeof(bfd->mhop.vrf_name));

		HASH_ADD(mh, local_peer_hash, mhop, sizeof(bfd->mhop), bfd);
	} else {
		bfd->shop.peer = bpc->bpc_peer;
		if (!bpc->bpc_has_vxlan)
			strxcpy(bfd->shop.port_name, bpc->bpc_localif,
				sizeof(bfd->shop.port_name));

		HASH_ADD(ph, peer_hash, shop, sizeof(bfd->shop), bfd);
	}

	if (BFD_CHECK_FLAG(bfd->flags, BFD_SESS_FLAG_VXLAN)) {
		static uint8_t bfd_def_vxlan_dmac[] = {0x00, 0x23, 0x20,
						       0x00, 0x00, 0x01};
		memcpy(bfd->peer_mac, bfd_def_vxlan_dmac,
		       sizeof(bfd_def_vxlan_dmac));
	}
#if 0 /* TODO */
	else if (event->rmac) {
		sscanf(event->rmac, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		       &bfd->peer_mac[0], &bfd->peer_mac[1], &bfd->peer_mac[2],
		       &bfd->peer_mac[3], &bfd->peer_mac[4], &bfd->peer_mac[5]);
		DLOG("%s: Assigning remote mac = %s\n", __FUNCTION__,
		     event->rmac);
	}
#endif

	/*
	 * XXX: session update triggers echo start, so we must have our
	 * discriminator ID set first.
	 */
	_bfd_session_update(bfd, bpc);

	/* Start transmitting with slow interval until peer responds */
	bfd->xmt_TO = BFD_DEF_SLOWTX;

	ptm_bfd_xmt_TO(bfd, 0);

	if (bpc->bpc_mhop) {
		INFOLOG("Created new session 0x%x with vrf %s peer %s local %s",
			bfd->discrs.my_discr,
			(bpc->bpc_has_vrfname) ? bfd->mhop.vrf_name : "N/A",
			satostr(&bfd->mhop.peer), satostr(&bfd->mhop.local));
	} else {
		INFOLOG("Created new session 0x%x with peer %s port %s",
			bfd->discrs.my_discr, satostr(&bfd->shop.peer),
			bpc->bpc_localif);
	}

	control_notify_config(BCM_NOTIFY_CONFIG_ADD, bfd);

	return bfd;
}

int ptm_bfd_ses_del(struct bfd_peer_cfg *bpc)
{
	bfd_session *bs;
	bfd_mhop_key mhop;
	bfd_shop_key shop;

	/* check to see if this needs a new session */
	if (bpc->bpc_mhop) {
		memset(&mhop, 0, sizeof(mhop));
		mhop.peer = bpc->bpc_peer;
		mhop.local = bpc->bpc_local;
		if (bpc->bpc_has_vrfname)
			strxcpy(mhop.vrf_name, bpc->bpc_vrfname,
				sizeof(mhop.vrf_name));

		bs = bfd_find_mhop(&mhop);
	} else {
		memset(&shop, 0, sizeof(shop));
		shop.peer = bpc->bpc_peer;
		if (!bpc->bpc_has_vxlan && bpc->bpc_has_localif)
			strxcpy(shop.port_name, bpc->bpc_localif,
				sizeof(shop.port_name));

		bs = bfd_find_shop(&shop);
	}
	if (bs == NULL)
		return -1;

	/*
	 * This pointer is being referenced somewhere, don't let it be deleted.
	 */
	if (bs->refcount > 0)
		return -1;

	if (BFD_CHECK_FLAG(bs->flags, BFD_SESS_FLAG_MH)) {
		INFOLOG("Deleting session 0x%x with vrf %s peer %s local %s",
			bs->discrs.my_discr,
			bpc->bpc_has_vrfname ? bpc->bpc_vrfname : "N/A",
			satostr(&bs->mhop.peer), satostr(&bs->mhop.local));
	} else {
		INFOLOG("Deleting session 0x%x with peer %s port %s\n",
			bs->discrs.my_discr, satostr(&bs->shop.peer),
			bs->shop.port_name);
	}

	control_notify_config(BCM_NOTIFY_CONFIG_DELETE, bs);

	bfd_session_free(bs);

	return 0;
}

void ptm_bfd_send_sla_update(bfd_session *bfd, struct timeval *recv_tv)
{
    uint32_t time_elapsed = ((recv_tv->tv_sec*1000 + recv_tv->tv_usec/1000)
            - (bfd->xmit_tv.tv_sec*1000 + bfd->xmit_tv.tv_usec/1000));
    bfd->sla.lattency += time_elapsed;
    if(bfd->sla.old_lat)
        bfd->sla.jitter += abs(bfd->sla.old_lat - time_elapsed);
    bfd->sla.old_lat = time_elapsed;
    uint32_t total_pkts = bfd->stats.rx_ctrl_pkt + bfd->stats.rx_echo_pkt;
    if(!(total_pkts % bfd->detect_mult)){
        if(total_pkts % PKTS_TO_CONSIDER_FOR_PKT_LOSS < bfd->detect_mult){
            uint32_t total_pkts_lost = ((bfd->stats.tx_ctrl_pkt + bfd->stats.tx_echo_pkt)
                    - (bfd->stats.rx_ctrl_pkt + bfd->stats.rx_echo_pkt));
            bfd->sla.pkt_loss = ((total_pkts_lost - bfd->sla.pkts_lost)/(float)PKTS_TO_CONSIDER_FOR_PKT_LOSS) * 100;
            bfd->sla.pkts_lost = total_pkts_lost;
        }

        bfd->sla.lattency /= bfd->detect_mult;
        bfd->sla.jitter /= (bfd->detect_mult - 1);

#ifdef BFD_EVENT_DEBUG
        log_debug("sla clac:\n\tlattency: %d\n\tjitter:%d\n\tpkt_loss:%f\n",
                bfd->sla.lattency, bfd->sla.jitter, bfd->sla.pkt_loss);
#endif /* BFD_EVENT_DEBUG */

        control_notify_sla(bfd);

        bfd->sla.lattency = 0;
        bfd->sla.jitter = 0;
        bfd->sla.old_lat = 0;
    }
}
