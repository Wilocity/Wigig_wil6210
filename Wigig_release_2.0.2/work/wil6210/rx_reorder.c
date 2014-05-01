#include "wil6210.h"
#include "txrx.h"

#define SEQ_MODULO 0x1000
#define SEQ_MASK   0xfff

static inline int seq_less(u16 sq1, u16 sq2)
{
	return ((sq1 - sq2) & SEQ_MASK) > (SEQ_MODULO >> 1);
}

static inline u16 seq_inc(u16 sq)
{
	return (sq + 1) & SEQ_MASK;
}

static inline u16 seq_sub(u16 sq1, u16 sq2)
{
	return (sq1 - sq2) & SEQ_MASK;
}

static inline int reorder_index(struct wil_tid_ampdu_rx *r, u16 seq)
{
	return seq_sub(seq, r->ssn) % r->buf_size;
}

static void wil_release_reorder_frame(struct wil6210_priv *wil,
				      struct wil_tid_ampdu_rx *r,
				      int index)
{
	struct net_device *ndev = wil_to_ndev(wil);
	struct sk_buff *skb = r->reorder_buf[index];

	if (!skb)
		goto no_frame;

	/* release the frame from the reorder ring buffer */
	r->stored_mpdu_num--;
	r->reorder_buf[index] = NULL;
	wil_netif_rx_any(skb, ndev);

no_frame:
	r->head_seq_num = seq_inc(r->head_seq_num);

}

static void wil_release_reorder_frames(struct wil6210_priv *wil,
				       struct wil_tid_ampdu_rx *r,
				       u16 hseq)
{
	int index;

	while (seq_less(r->head_seq_num, hseq)) {
		index = reorder_index(r, r->head_seq_num);
		wil_release_reorder_frame(wil, r, index);
	}

}

static void wil_reorder_release(struct wil6210_priv *wil,
				struct wil_tid_ampdu_rx *r)
{
	int index = reorder_index(r, r->head_seq_num);

	while (r->reorder_buf[index]) {
		wil_release_reorder_frame(wil, r, index);
		index = reorder_index(r, r->head_seq_num);
	}
}

void wil_rx_reorder(struct wil6210_priv *wil, struct sk_buff *skb)
{
	struct net_device *ndev = wil_to_ndev(wil);
	struct vring_rx_desc *d = wil_skb_rxdesc(skb);
	int tid = wil_rxdesc_tid(d);
	int cid = wil_rxdesc_cid(d);
	int mid = wil_rxdesc_mid(d);
	u16 seq = wil_rxdesc_seq(d);
	struct wil_sta_info *sta = &wil->sta[cid];
	struct wil_tid_ampdu_rx *r = sta->tid_rx[tid];
	u16 hseq;
	int index;

	wil_dbg_txrx(wil, "MID %d CID %d TID %d Seq 0x%03x\n",
		     mid, cid, tid, seq);

	if (!r) {
		wil_netif_rx_any(skb, ndev);
		return;
	}

	hseq = r->head_seq_num;

	spin_lock(&r->reorder_lock);

	/* frame with out of date sequence number */
	if (seq_less(seq, r->head_seq_num)) {
		r->ssn_last_drop = seq;
		dev_kfree_skb(skb);
		goto out;
	}

	/*
	 * If frame the sequence number exceeds our buffering window
	 * size release some previous frames to make room for this one.
	 */
	if (!seq_less(seq, r->head_seq_num + r->buf_size)) {
		hseq = seq_inc(seq_sub(seq, r->buf_size));
		/* release stored frames up to new head to stack */
		wil_release_reorder_frames(wil, r, hseq);
	}

	/* Now the new frame is always in the range of the reordering buffer */

	index = reorder_index(r, seq);

	/* check if we already stored this frame */
	if (r->reorder_buf[index]) {
		dev_kfree_skb(skb);
		goto out;
	}

	/*
	 * If the current MPDU is in the right order and nothing else
	 * is stored we can process it directly, no need to buffer it.
	 * If it is first but there's something stored, we may be able
	 * to release frames after this one.
	 */
	if (seq == r->head_seq_num && r->stored_mpdu_num == 0) {
		r->head_seq_num = seq_inc(r->head_seq_num);
		wil_netif_rx_any(skb, ndev);
		goto out;
	}

	/* put the frame in the reordering buffer */
	r->reorder_buf[index] = skb;
	r->reorder_time[index] = jiffies;
	r->stored_mpdu_num++;
	wil_reorder_release(wil, r);

out:
	spin_unlock(&r->reorder_lock);
}

struct wil_tid_ampdu_rx *wil_tid_ampdu_rx_alloc(struct wil6210_priv *wil,
						int size, u16 ssn)
{
	struct wil_tid_ampdu_rx *r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return NULL;

	r->reorder_buf =
		kcalloc(size, sizeof(struct sk_buff *), GFP_KERNEL);
	r->reorder_time =
		kcalloc(size, sizeof(unsigned long), GFP_KERNEL);
	if (!r->reorder_buf || !r->reorder_time) {
		kfree(r->reorder_buf);
		kfree(r->reorder_time);
		kfree(r);
		return NULL;
	}

	spin_lock_init(&r->reorder_lock);
	r->ssn = ssn;
	r->head_seq_num = ssn;
	r->buf_size = size;
	r->stored_mpdu_num = 0;
	return r;
}

void wil_tid_ampdu_rx_free(struct wil6210_priv *wil,
			   struct wil_tid_ampdu_rx *r)
{
	if (!r)
		return;
	wil_release_reorder_frames(wil, r, r->head_seq_num + r->buf_size);
	kfree(r->reorder_buf);
	kfree(r->reorder_time);
	kfree(r);
}
/* ADDBA processing */

int wil_rcp_addba_request(struct wil6210_priv *wil, u8 cidxtid,
			  u8 dialog_token, __le16 ba_param_set,
			  __le16 ba_timeout, __le16 ba_seq_ctrl)
{
	struct wil_pending_back *req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->cidxtid = cidxtid;
	req->dialog_token = dialog_token;
	req->ba_param_set = le16_to_cpu(ba_param_set);
	req->ba_timeout = le16_to_cpu(ba_timeout);
	req->ba_seq_ctrl = le16_to_cpu(ba_seq_ctrl);

	mutex_lock(&wil->back_mutex);
	list_add_tail(&req->list, &wil->back_pending);
	mutex_unlock(&wil->back_mutex);

wil_err(wil, "EREZK !!! wil_rcp_addba_request: param %x", req->ba_param_set);
	queue_work(wil->back_wq, &wil->back_worker);

	return 0;
}

static void wil_back_handle(struct wil6210_priv *wil,
			    struct wil_pending_back *req)
{
	u8 cid, tid;
	u16 req_agg_wsize;
	struct wil_sta_info *sta;
	int rc;

	parse_cidxtid(req->cidxtid, &cid, &tid);

	/* sanity checks */
	if (cid >= WIL6210_MAX_CID) {
		wil_err(wil, "BACK: invalid CID %d\n", cid);
		return;
	}
	sta = &wil->sta[cid];
	if (sta->status != wil_sta_connected) {
		wil_err(wil, "BACK: CID %d not connected\n", cid);
		return;
	}
	req_agg_wsize = WIL_GET_BITS(req->ba_param_set, 6, 15);

wil_err(wil, "EREZK !!! wil_rcp_addba_request: cid %x, tid %x, req_agg_wsize %d", cid, tid, req_agg_wsize);


	wil_dbg_wmi(wil, "ADDBA request for CID %d %pM TID %d size %d\n",
		    cid, sta->addr, tid, req_agg_wsize);

	/* apply policies */
	req->agg_timeout = req->ba_timeout;
	req->agg_wsize = min_t(u16, 16, req_agg_wsize);
	req->agg_policy = 1;
	req->agg_amsdu = 0;

	rc = wmi_rcp_addba_resp(wil, cid, tid, req->dialog_token, 0,
				req->agg_amsdu, req->agg_wsize,
				req->agg_timeout);
	if (rc)
		return;

	/* apply */
	wil_tid_ampdu_rx_free(wil, sta->tid_rx[tid]);
	sta->tid_rx[tid] = wil_tid_ampdu_rx_alloc(wil, req->agg_wsize,
						  req->ba_seq_ctrl >> 4);
}

void wil_back_flush(struct wil6210_priv *wil)
{
	struct wil_pending_back *evt, *t;

	wil_dbg_misc(wil, "%s()\n", __func__);

	list_for_each_entry_safe(evt, t, &wil->back_pending, list) {
		list_del(&evt->list);
		kfree(evt);
	}
}

/*
 * Retrieve next ADDBA request from the pending list
 */
static struct list_head *next_back(struct wil6210_priv *wil)
{
	struct list_head *ret = NULL;

	mutex_lock(&wil->back_mutex);

	if (!list_empty(&wil->back_pending)) {
		ret = wil->back_pending.next;
		list_del(ret);
	}

	mutex_unlock(&wil->back_mutex);

	return ret;
}

void wil_back_worker(struct work_struct *work)
{
	struct wil6210_priv *wil = container_of(work, struct wil6210_priv,
						 back_worker);
	struct wil_pending_back *evt;
	struct list_head *lh;

	while ((lh = next_back(wil)) != NULL) {
		evt = list_entry(lh, struct wil_pending_back, list);

		wil_back_handle(wil, evt);
		kfree(evt);
	}
}
