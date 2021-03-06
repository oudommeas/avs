/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#define _POSIX_SOURCE 1
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <re/re.h>

#include "avs.h"

#include "flowmgr.h"

#include "avs_vie.h"
#include "avs_voe_stats.h"
#include "../media/rtp_stats.h"


static void call_destructor(void *arg)
{
	struct call *call = arg;
	struct le *le;

	info("flowmgr(%p): call(%p): dtor\n", call->fm, call);

	le = call->rrl.head;
	while (le) {
		struct rr_resp *rr = le->data;

		le = le->next;

		rr_cancel(rr);
	}

	list_flush(&call->ghostl);
	list_flush(&call->conf_parts);

	dict_flush(call->users);
	mem_deref(call->users);

	if (call->fm)
		dict_remove(call->fm->calls, call->convid);

	if (call->flows) {
		struct dict *flows = call->flows;

		call->flows = NULL;
		mem_deref(flows);
	}

	mem_deref(call->convid);
	mem_deref(call->sessid);

	call->fm = NULL;

	list_unlink(&call->post_le);
}


int call_alloc(struct call **callp, struct flowmgr *fm, const char *convid)
{
	struct call *call;
	int err;

	if (!callp || !fm)
		return EINVAL;

	call = mem_zalloc(sizeof(*call), call_destructor);
	if (call == NULL) {
		return ENOMEM;
	}

	info("flowmgr(%p): call(%p) alloc\n", fm, call);

	call->fm = fm;

	err = dict_add(fm->calls, convid, call);
	if (err) {
		warning("flowmgr: call_alloc: failed to add to calls: %m\n",
			err);
		goto out;
	}

	err = str_dup(&call->convid, convid);
	if (err) {
		goto out;
	}
	err = dict_alloc(&call->flows);
	if (err) {
		goto out;
	}

	err = dict_alloc(&call->users);
	if (err) {
		goto out;
	}

	list_init(&call->conf_parts);

	list_init(&call->rrl);
	list_init(&call->ghostl);

	/* Call is now owned by the dictionary */
	mem_deref(call);

 out:
	if (err) {
		/* If no one received the call object,
		 * it is referenced by the dictionary
		 */
		mem_deref(call);
	}
	else if (callp) {
		*callp = call;
	}

	return err;
}


bool call_has_good_flow(const struct call *call)
{
	struct flow *flow = NULL;

	if (call->flows)
		flow = dict_apply(call->flows, flow_good_handler, NULL);

	return flow != NULL;
}


static bool flow_active_rtp_handler(char *key, void *val, void *arg)
{
	struct flow *flow = val;
	uint32_t *n = arg;

	if (flow->est_st & FLOWMGR_ESTAB_ACTIVE){
		info("flow_active_rtp(%d): flowid %s has RTP = %d \n",
		     *n, flow->flowid,
		     (bool)(flow->est_st & FLOWMGR_ESTAB_RTP));
		*n = (*n) - 1;

		return !(flow->est_st & FLOWMGR_ESTAB_RTP);
	}
	else {
		return false;
	}
}


uint32_t flowmgr_call_count_active_flows(const struct call *call)
{
	uint32_t n = 0;

	if (!call)
		return 0;

	dict_apply(call->flows, flow_count_active_handler, &n);

	return n;
}


bool call_is_multiparty(const struct call *call)
{
	return flowmgr_call_count_active_flows(call) > 1;
}


void call_mestab_check(struct call *call)
{
	struct flow *flow;
	uint32_t n;

	if (!call)
		return;

	n = flowmgr_call_count_active_flows(call);

	if (n == 0)
		return;

	flow = dict_apply(call->flows, flow_active_rtp_handler, &n);
	if (flow != NULL || n > 0)
		return;

	info("call_mestab_check(%p): mestab=%d media on all active flows\n",
	     call, call->is_mestab);
	if (!call->is_mestab) {
		flowmgr_silencing(false);
		if (call->fm && call->fm->mestabh) {
			call->fm->mestabh(call->convid,
					  true, call->fm->mestab_arg);
		}
		call->is_mestab = true;
	}
}


int  call_count_flows(struct call *call)
{
	if (!call)
		return 0;

	return dict_count(call->flows);
}


struct list *call_conf_parts(struct call *call)
{
	return call ? &call->conf_parts : NULL;
}


struct flow *call_best_flow(const struct call *call)
{
	enum flowmgr_estab best_st = FLOWMGR_ESTAB_NONE;
	struct flow *best_flow = NULL;
	struct flow *res = NULL;

	if (!call->flows)
		return NULL;

	do {
		best_flow = dict_apply(call->flows,
				       flow_best_handler,
				       &best_st);
		if (best_flow)
			res = best_flow;
	}
	while (best_flow);

	return res;
}


bool call_cat_chg_pending(const struct call *call)
{
	return call ? call->catchg_pending : false;
}


enum flowmgr_mcat call_mcat(const struct call *call)
{
	return call ? call->mcat : FLOWMGR_MCAT_NORMAL;
}


struct flowmgr *call_flowmgr(struct call *call)
{
	return call ? call->fm : NULL;
}


const char *call_convid(const struct call *call)
{
	return call ? call->convid : NULL;
}


int call_set_sessid(struct call *call, const char *sessid)
{
	int err;

	if (!call)
		return EINVAL;

	if (!call->sessid) {
		info("flowmgr: set session-id: %s\n", sessid);
	}

	call->sessid = mem_deref(call->sessid);
	err = str_dup(&call->sessid, sessid);

	return err;
}


const char *flowmgr_call_sessid(const struct call *call)
{
	if (!call)
		return NULL;

	return call->sessid ? call->sessid : "0000";
}


int call_add_conf_part(struct call *call, struct flow *flow)
{
	const char *userid;
	int err;

	if (!call || !flow)
		return EINVAL;

	userid = flow_remoteid(flow);

	if (conf_part_find(&call->conf_parts, userid)) {
		warning("flowmgr: conf_part already exist for userid=%s\n",
			userid);
		return EEXIST;
	}

	flow->cp = mem_deref(flow->cp);
	err = conf_part_add(&flow->cp, &call->conf_parts, userid, flow);
	if (err)
		return err;

	conf_pos_sort(&call->conf_parts);

	err = flowmgr_update_conf_parts(&call->conf_parts);
	if (err)
		goto out;

	if (call->fm && call->fm->conf_posh) {
		call->fm->conf_posh(call->convid, &call->conf_parts,
				    call->fm->conf_pos_arg);
	}

 out:
	if (err)
		flow->cp = mem_deref(flow->cp);
	return err;
}


void call_remove_conf_part(struct call *call, struct flow *flow)
{
	if (!call || !flow)
		return;

	flow->cp = mem_deref(flow->cp);

	conf_pos_sort(&call->conf_parts);

	flowmgr_update_conf_parts(&call->conf_parts);

	if (call->fm && call->fm->conf_posh) {
		call->fm->conf_posh(call->convid, &call->conf_parts,
				    call->fm->conf_pos_arg);
	}
}


int call_add_flow(struct call *call, struct userflow *uf, struct flow *flow)
{
	int err;

	if (!call)
		return -1;

	err = dict_add(call->flows, flow->flowid, flow);
	if (err)
		return -1;

	flow->userflow = uf;

	++call->ix_ctr;
	return call->ix_ctr;
}


#if CALLING2_0 /* CALLING2.0 */
static bool flow_activate_handler(char *key, void *val, void *arg)
{
	struct flow *flow = val;
	struct call *call = arg;

	(void)key;

	flow_activate(flow, call->active);

	return false;
}
#endif


int call_set_active(struct call *call, bool active)
{
	if (!call)
		return EINVAL;

	call->active = active;

/* CALLING2.0 don't activate all flows, with eager flows
 * we will end up activating non-active flows, leading to
 * never unsilencing.
 */
#if CALLING2_0
	dict_apply(call->flows, flow_activate_handler, call);
#endif

	return 0;
}


static bool userflow_reset_state_handler(char *key, void *val, void *arg)
{
	struct userflow *uf = val;

	(void)arg;
	(void)key;

	userflow_set_state(uf, USERFLOW_STATE_IDLE);

	return false;
}


static bool userflow_post_state_handler(char *key, void *val, void *arg)
{
	struct userflow *uf = val;
	enum userflow_signal_state ss;

	(void)arg;
	(void)key;

	ss = userflow_signal_state(uf);

	switch (ss) {

	case USERFLOW_SIGNAL_STATE_STABLE:
		userflow_set_state(uf, USERFLOW_STATE_POST);
		break;

	default:
		break;
	}

	return false;
}


static bool userflow_post_handler(char *key, void *val, void *arg)
{
	struct userflow *uf = val;
	enum userflow_signal_state ss;
	int err;

	(void)arg;
	(void)key;

	ss = userflow_signal_state(uf);

	if (mediaflow_sdp_is_complete(userflow_mediaflow(uf))) {
		info("userflow_post_handler: %p mediaflow already has SDP\n",
		     uf);
		info("                       signal_state=%s\n",
		     userflow_signal_state_name(ss));
		return false;
	}

	switch (ss) {

	case USERFLOW_SIGNAL_STATE_STABLE:
		userflow_set_state(uf, USERFLOW_STATE_POST);
		err = userflow_generate_offer(uf);
		if (err) {
			warning("userflow_post: userflow_generate_offer"
				" failed (%m)\n", err);
		}
		return false;

	case USERFLOW_SIGNAL_STATE_HAVE_REMOTE_OFFER:
		info("flowmgr: userflow: %p have_remote_offer: accepting...\n",
		     uf);
		userflow_accept(uf, NULL);
		break;

	default:
		break;
	}

	return false;
}


void call_check_and_post(struct call *call)
{
	struct userflow *uf;
	int err = 0;

	if (!call)
		return;

	info("flowmgr: call_check_and_post(%p): users=%u flows=%u\n",
	     call, dict_count(call->users), dict_count(call->flows));

	uf = dict_apply(call->users, userflow_check_sdp_handler, call);
	if (uf == NULL) {
		err = flowmgr_post_flows(call);
		if (err) {
			warning("flowmgr: call: flowmgr_post_flows (%m)\n",
				err);
		}

		dict_apply(call->users, userflow_reset_state_handler, NULL);
	}
}


static bool userflow_sdp_handler(char *key, void *val, void *arg)
{
	struct userflow *uf = val;
	struct json_object **jop = arg;
	struct json_object *jpart;

	(void)key;

	if (!userflow_sdp_isready(uf))
		return false;
	if (!str_isset(uf->sdp.sdp))
		return false;

	if (*jop == NULL)
		*jop = json_object_new_object();

	jpart = json_object_new_object();
	json_object_object_add(jpart, "type",
			       json_object_new_string(uf->sdp.type));
	json_object_object_add(jpart, "sdp",
			       json_object_new_string(uf->sdp.sdp));
	json_object_object_add(*jop, uf->userid, jpart);

	return false;
}


struct json_object *call_userflow_sdp(struct call *call)
{
	struct json_object *jobj = NULL;
	struct json_object *jsdp = NULL;

	if (!call)
		return NULL;

	dict_apply(call->users, userflow_sdp_handler, &jsdp);

	if (jsdp) {
		jobj = json_object_new_object();
		json_object_object_add(jobj, "sdp", jsdp);
	}

	return jobj;
}


int call_lookup_alloc(struct call **callp, bool *allocated,
		      struct flowmgr *fm, const char *convid)
{
	struct call *call;
	int err = 0;

	call = dict_lookup(fm->calls, convid);
	if (call) {
		*callp = call;
		*allocated = false;

		return 0;
	}

	err = call_alloc(&call, fm, convid);
	if (err)
		return err;

	*callp = call;
	*allocated = true;

	return 0;
}


static bool userflow_update_handler(char *key, void *val, void *arg)
{
	struct userflow *uf = val;

	(void)key;
	(void)arg;

	userflow_update_config(uf);

	return false;
}


int call_postponed_flows(struct call *call)
{
	dict_apply(call->users, userflow_update_handler, call);

	return call_post_flows(call);
}


int call_post_flows(struct call *call)
{
	if (!call)
		return EINVAL;

	info("flowmgr: call_post_flows (call=%p, users=%d)\n",
	     call, (int)dict_count(call->users));

	/* Empty post if there are no users in this conversation */
	if (dict_count(call->users) == 0)
		return flowmgr_post_flows(call);

	/* If there are users set, then when gathering is complete,
	 * we will create a async POST
	 */
	dict_apply(call->users, userflow_post_state_handler, NULL);
	dict_apply(call->users, userflow_post_handler, NULL);

	return 0;
}


int call_userflow_lookup_alloc(struct userflow **ufp,
			       bool *allocated,
			       struct call *call,
			       const char *userid, const char *username)
{
	struct userflow *uf = NULL;
	int err = 0;

	if (!ufp || !call)
		return EINVAL;

	uf = dict_lookup(call->users, userid);
	if (uf) {
		if (allocated)
			*allocated = false;
	}
	else {
		err = userflow_alloc(&uf, call, userid, username);
		if (err) {
			warning("flowmgr: call: userflow_alloc (%m)\n", err);
			goto out;
		}

		if (allocated)
			*allocated = true;
	}

 out:
	if (err == 0)
		*ufp = uf;

	return err;
}


int call_mcat_change(struct call *call, enum flowmgr_mcat mcat)
{
	struct flowmgr *fm;

	if (!call)
		return EINVAL;

	fm = call->fm;

	call->mcat = mcat;
	call->catchg_pending = true;
	if (fm && fm->cath)
		fm->cath(call->convid, mcat, fm->marg);

	return 0;
}


static bool update_media_handler(char *key, void *val, void *arg)
{
	struct call *call = arg;
	struct flow *flow = val;

	(void)key;
	(void)call;

	flow_update_media(flow);

	return false;
}


int call_mcat_changed(struct call *call, enum flowmgr_mcat mcat)
{
	if (!call)
		return EINVAL;

	info("flowmgr(%p): mcat_changed: %s -> %s\n",
	     call->fm,
	     flowmgr_mediacat_name(call->mcat),
	     flowmgr_mediacat_name(mcat));

	call->mcat = mcat;
	call->catchg_pending = false;

	if (!call->flows) {
		warning("flowmgr: flowmgr_mcat_changed: no call or flows\n");
		return EIO;
	}

	dict_apply(call->flows, update_media_handler, call);

	return 0;
}


static bool interrupt_media_handler(char *key, void *val, void *arg)
{
	struct call *call = arg;
	struct flow *flow = val;
	bool *interrupted = arg;

	(void)key;
	(void)call;

	flow_interruption(flow, *interrupted);

	return false;
}


int call_interruption(struct call *call, bool interrupted)
{
	if (!call)
		return EINVAL;

	dict_apply(call->flows, interrupt_media_handler, &interrupted);

	return 0;
}


int call_debug(struct re_printf *pf, const struct call *call)
{
	int err = 0;

	err |= re_hprintf(pf, "  call:      %p\n", call);
	err |= re_hprintf(pf, "  convid:    %s\n", call_convid(call));
	err |= re_hprintf(pf, "  sessionid: %s\n", flowmgr_call_sessid(call));
	err |= re_hprintf(pf, "  mediacat:  %s\n",
			  flowmgr_mediacat_name(call_mcat(call)));
	err |= re_hprintf(pf, "  flows:     %u   (active flows is %u)\n",
			  dict_count(call->flows),
			  flowmgr_call_count_active_flows(call));

	dict_apply(call->flows, flow_debug_handler, pf);
	err |= re_hprintf(pf, "\n");

	err |= re_hprintf(pf, "  users:     %u\n",
			  dict_count(call->users));
	dict_apply(call->users, userflow_debug_handler, pf);
	err |= re_hprintf(pf, "\n");

	err |= re_hprintf(pf, "%H", conf_pos_print, &call->conf_parts);
	err |= re_hprintf(pf, "\n");

	return err;
}


bool call_debug_handler(char *key, void *val, void *arg)
{
	struct re_printf *pf = arg;
	struct call *call = val;
	int err = 0;

	err = call_debug(pf, call);

	return err != 0;
}


struct flow *call_find_flow(struct call *call, const char *flowid)
{
	return call ? dict_lookup(call->flows, flowid) : NULL;
}


static bool has_flow_handler(char *key, void *val, void *arg)
{
	(void)key;

	return val == arg;
}


bool call_has_flow(struct call *call, struct flow *flow)
{
	return dict_apply(call->flows, has_flow_handler, flow) != NULL;
}


void call_remove_flow(struct call *call, const char *flowid)
{
	if (!call)
		return;

	dict_remove(call->flows, flowid);

	call_mestab_check(call);
}


bool call_active_handler(char *key, void *val, void *arg)
{
	struct call *call = val;
	struct flow *active;

	(void)arg;

	active = dict_apply(call->flows, flow_active_handler, call);

	return active != NULL;
}


void call_restart(struct call *call)
{
	if (!call)
		return;

	dict_apply(call->flows, flow_restart_handler, NULL);
}


bool call_restart_handler(char *key, void *val, void *arg)
{
	struct call *call = val;

	(void)key;
	(void)arg;

	call_restart(call);

	return false;
}


int call_deestablish_media(struct call *call)
{
	struct dict *flows;

	if (!call)
		return EINVAL;

	if (call->flows)
		dict_apply(call->flows, flow_deestablish_media, NULL);

	/* De-refing flows will cause a re_thread_leave,
	 * meaning we must make sure call->flows is NULL
	 * before continuing to deref the dictionary
	 */
	flows = call->flows;
	call->flows = NULL;

	mem_deref(flows);

	return 0;
}


void call_rtp_started(struct call *call, bool started)
{
	struct flowmgr *fm;

	if (!call)
		return;

	fm = call->fm;

	debug("flowmgr(%p): call(%p): rtp_started: started=%d->%d\n",
	      fm, call, call->rtp_started, started);

	if (call->rtp_started == started)
		return;

	call->rtp_started = started;
	if (started && call->rtp_start_ts == 0) {
		uint64_t t;

		call->rtp_start_ts = tmr_jiffies();

		t = call->rtp_start_ts - call->start_ts;
		info("flowmgr(%p): call(%p): rtp_started "
		     "total setup time is %llu ms\n",
		     fm, call, t);
	}
}


bool call_has_media(struct call *call)
{
	bool has_media;

	if (!call)
		return false;

	has_media = (call->mcat == FLOWMGR_MCAT_CALL) && call->rtp_started;
	debug("flowmgr(%p): call_has_media: %d\n", call->fm, has_media);

	return has_media;
}

static bool stats_has_video(struct mediaflow *mf)
{
	struct rtp_stats* rtps = mediaflow_snd_video_rtp_stats(mf);
	bool has_video = false;
	if(rtps){
		if(rtps->bit_rate_stats.max != -1){
			has_video = true;
		}
	}
	return has_video;
}

bool call_stats_prepare(struct call *call, struct json_object *jobj)
{
	struct flow *flow;

	debug("flowmgr(%p): call_stats_prepare\n", call->fm);

	flow = dict_apply(call->flows, flow_stats_handler, jobj);

	if (flow != NULL) {
		struct mediaflow *mf;
		const struct mediaflow_stats *mf_stats;
		uint64_t t = call->rtp_start_ts - call->start_ts;
		int32_t n = dict_count(call->flows);
		bool ice = false;
		bool dtls = false;
		int err = 0;

		if (flow->userflow) {
			dtls = mediaflow_dtls_ready(
					 userflow_mediaflow(flow->userflow));
			ice = mediaflow_ice_ready(
					 userflow_mediaflow(flow->userflow));
		}

		json_object_object_add(jobj, "setup_time",
				       json_object_new_int((int32_t)t));

#if 0 /* Disable session-id for privacy */
		{
			struct json_object *jsess;

			jsess = json_object_new_string(call->sessid ?
						       call->sessid : "N/A");

			json_object_object_add(jobj, "session", jsess);
		}
#endif

		json_object_object_add(jobj, "num_flows",
				       json_object_new_int(n));
		json_object_object_add(jobj, "dtls",
				    json_object_new_boolean(dtls));
		json_object_object_add(jobj, "ice",
				    json_object_new_boolean(ice));
		json_object_object_add(jobj, "video",
					json_object_new_boolean(stats_has_video(userflow_mediaflow(flow->userflow))));
        
		struct aucodec_stats *voe_stats = mediaflow_codec_stats(userflow_mediaflow(flow->userflow));
		if (voe_stats) {
			err |= jzon_add_int(jobj, "mic_vol(dB)", voe_stats->in_vol.avg);
			err |= jzon_add_int(jobj, "spk_vol(dB)", voe_stats->out_vol.avg);
			err |= jzon_add_int(jobj, "avg_rtt", voe_stats->rtt.avg);
			err |= jzon_add_int(jobj, "max_rtt", voe_stats->rtt.max);
			err |= jzon_add_int(jobj, "avg_jb_loss", voe_stats->loss_d.avg);
			err |= jzon_add_int(jobj, "max_jb_loss", voe_stats->loss_d.max);
			err |= jzon_add_int(jobj, "avg_jb_size", voe_stats->jb_size.avg);
			err |= jzon_add_int(jobj, "max_jb_size", voe_stats->jb_size.max);
			err |= jzon_add_int(jobj, "avg_loss_u", voe_stats->loss_u.avg);
			err |= jzon_add_int(jobj, "max_loss_u", voe_stats->loss_u.max);
		}
		struct rtp_stats* rtps = mediaflow_rcv_audio_rtp_stats(userflow_mediaflow(flow->userflow));
		if (rtps) {
			err |= jzon_add_int(jobj, "avg_loss_d", (int)rtps->pkt_loss_stats.avg);
			err |= jzon_add_int(jobj, "max_loss_d", (int)rtps->pkt_loss_stats.max);
			err |= jzon_add_int(jobj, "avg_rate_d", (int)rtps->bit_rate_stats.avg);
			err |= jzon_add_int(jobj, "min_rate_d", (int)rtps->bit_rate_stats.min);
			err |= jzon_add_int(jobj, "avg_pkt_rate_d", (int)rtps->pkt_rate_stats.avg);
			err |= jzon_add_int(jobj, "min_pkt_rate_d", (int)rtps->pkt_rate_stats.min);
			err |= jzon_add_int(jobj, "a_dropouts", rtps->dropouts);
		}
		rtps = mediaflow_snd_audio_rtp_stats(userflow_mediaflow(flow->userflow));
		if (rtps) {
			err |= jzon_add_int(jobj, "avg_rate_u", (int)rtps->bit_rate_stats.avg);
			err |= jzon_add_int(jobj, "min_rate_u", (int)rtps->bit_rate_stats.min);
			err |= jzon_add_int(jobj, "avg_pkt_rate_u", (int)rtps->pkt_rate_stats.avg);
			err |= jzon_add_int(jobj, "min_pkt_rate_u", (int)rtps->pkt_rate_stats.min);
		}
		if (voe_stats) {
			struct json_object *jsess;
			jsess = json_object_new_string(voe_stats->audio_route);
			json_object_object_add(jobj, "audio_route", jsess);
			err |= jzon_add_int(jobj, "test_score", voe_stats->test_score);
		}
		rtps = mediaflow_rcv_video_rtp_stats(userflow_mediaflow(flow->userflow));
		if (rtps) {
			err |= jzon_add_int(jobj, "v_avg_rate_d", (int)rtps->bit_rate_stats.avg);
			err |= jzon_add_int(jobj, "v_min_rate_d", (int)rtps->bit_rate_stats.min);
			err |= jzon_add_int(jobj, "v_max_rate_d", (int)rtps->bit_rate_stats.max);
			err |= jzon_add_int(jobj, "v_avg_frame_rate_d", (int)rtps->frame_rate_stats.avg);
			err |= jzon_add_int(jobj, "v_min_frame_rate_d", (int)rtps->frame_rate_stats.min);
			err |= jzon_add_int(jobj, "v_max_frame_rate_d", (int)rtps->frame_rate_stats.max);
			err |= jzon_add_int(jobj, "v_dropouts", rtps->dropouts);
		}
		rtps = mediaflow_snd_video_rtp_stats(userflow_mediaflow(flow->userflow));
		if (rtps) {
			err |= jzon_add_int(jobj, "v_avg_rate_u", (int)rtps->bit_rate_stats.avg);
			err |= jzon_add_int(jobj, "v_min_rate_u", (int)rtps->bit_rate_stats.min);
			err |= jzon_add_int(jobj, "v_max_rate_u", (int)rtps->bit_rate_stats.max);
			err |= jzon_add_int(jobj, "v_avg_frame_rate_u", (int)rtps->frame_rate_stats.avg);
			err |= jzon_add_int(jobj, "v_min_frame_rate_u", (int)rtps->frame_rate_stats.min);
			err |= jzon_add_int(jobj, "v_max_frame_rate_u", (int)rtps->frame_rate_stats.max);
		}
		if (err)
			return NULL;

		mf = userflow_mediaflow(flow->userflow);
		mf_stats = mediaflow_stats_get(mf);
		if (mf_stats) {
			err |= jzon_add_int(jobj, "turn_alloc",
					    mf_stats->turn_alloc);
			err |= jzon_add_int(jobj, "nat_estab",
					    mf_stats->nat_estab);
			err |= jzon_add_int(jobj, "dtls_estab",
					    mf_stats->dtls_estab);
			if (err)
				return NULL;
		}

		err |= jzon_add_int(jobj, "flow_error", flow->err);
	}

	json_object_object_add(jobj, "media_established",
			  json_object_new_boolean(call->is_mestab));

	return flow != NULL;
}


void call_cancel(struct call *call)
{
	if (!call)
		return;

	call_deestablish_media(call);
}


void call_ghost_flow_handler(int status, struct rr_resp *rr,
			     struct json_object *jobj, void *arg)
{
	struct flow_elem *flel = arg;
	struct call *call = flel->call;

	list_unlink(&flel->le);

	if (call->ghostl.head == NULL)
		flowmgr_post_flows(call);

	mem_deref(flel);
}


bool call_can_send_video(struct call *call)
{
	struct flow *active;

	active = dict_apply(call->flows, flow_active_handler, call);
	if (!active) {
		info("call_can_send_video: active flow not found\n");
		return false;
	}

	return flow_can_send_video(active);
}


void call_set_video_send_active(struct call *call, bool video_active)
{
	struct flow *active;

	active = dict_apply(call->flows, flow_active_handler, call);
	if (!active) {
		info("call_set_video_send_active: active flow not found\n");
		return;
	}

	if (video_active && call->mcat == FLOWMGR_MCAT_CALL) {
		struct flowmgr *fm = call->fm;

		if (fm && fm->cath) {
			fm->cath(call->convid, FLOWMGR_MCAT_CALL_VIDEO,
				 fm->marg);
		}
	}

	flow_set_video_send_active(active, video_active);
}


void call_video_rcvd(struct call *call, bool rcvd)
{
	if (!call)
		return;

	if (rcvd && call->mcat == FLOWMGR_MCAT_CALL) {
		struct flowmgr *fm = call->fm;

		if (fm && fm->cath) {
			fm->cath(call->convid, FLOWMGR_MCAT_CALL_VIDEO,
				 fm->marg);
		}
	}
}


bool call_is_sending_video(struct call *call, const char *partid)
{
	struct flow *flow;

	flow = dict_apply(call->flows, flow_lookup_part_handler,
			  (void *)partid);
	if (!flow) {
		warning("flowmgr: call(%p): cannot find flow for part:%s\n",
			call, partid);
		return false;
	}

	return flow_is_sending_video(flow);
}


static bool remote_user_handler(char *key, void *val, void *arg)
{
	struct flow *flow = val;
	const char *remote_user = arg;

	return 0 == str_casecmp(flow->remoteid, remote_user);
}


struct flow *call_find_remote_user(const struct call *call,
				   const char *remote_user)
{
	struct flow *flow = NULL;

	if (!call)
		return NULL;

	flow = dict_apply(call->flows, remote_user_handler,
			  (void *)remote_user);

	return flow;
}


void call_mestab(struct call *call, bool mestab)
{
	if (!call)
		return;

	call->is_mestab = mestab;
}


void call_flush_users(struct call *call)
{
	if (!call)
		return;

	dict_flush(call->users);
}


static bool purge_handler(char *key, void *val, void *arg)
{
	struct userflow *uf = val;

	(void)key;
	(void)arg;

	return uf->flow == NULL;
}


void call_purge_users(struct call *call)
{
	struct userflow *uf;

	if (!call)
		return;

	do {
		uf = dict_apply(call->users, purge_handler, call);
		if (uf) {
			info("flowmgr: %s purging user: (%p)%s\n",
			     __func__, uf, uf->userid);
			dict_remove(call->users, uf->userid);
		}
	}
	while (uf != NULL);
}
