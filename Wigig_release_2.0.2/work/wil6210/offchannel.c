#include "wil6210.h"

static void wil_roc_cancel_work(struct work_struct *work)
{
	struct wil_roc *roc = container_of(work, struct wil_roc, work.work);
	struct wil6210_priv *wil = container_of(roc, struct wil6210_priv, roc);

	wil_dbg_misc(wil, "%s(%d, %d ms)\n", __func__, roc->chan->center_freq,
		     roc->duration);

	mutex_lock(&wil->mutex);

	/* TODO: go back to our channel */
	wmi_stop_discovery(wil);

	if (!roc->mgmt_tx_cookie)
		cfg80211_remain_on_channel_expired(wil->wdev,
						   roc->cookie, roc->chan,
						   GFP_KERNEL);

	roc->cookie = 0;
	roc->mgmt_tx_cookie = 0;

	mutex_unlock(&wil->mutex);
}

int wil_cancel_roc(struct wil6210_priv *wil)
{
	flush_delayed_work(&wil->roc.work);
	return 0;
}

/*
 * Start ROC operation.
 * - if Tx frame provided, transmit it
 * - if delay specified, schedule cancel work
 *
 * It may be Tx frame with delay == 0,
 * in this case Tx and cancel immediately
 */
static int wil_start_roc(struct wil6210_priv *wil)
{
	struct wil_roc *roc = &wil->roc;
	int rc = 0;

	lockdep_assert_held(&wil->mutex);

	wil_dbg_misc(wil, "%s(%d, %d ms)\n", __func__, roc->chan->center_freq,
		     roc->duration);

	/* TODO: start with HW - set channel */
	rc = wmi_p2p_cfg(wil, 2, 100);
	if (rc)
		return rc;

	rc = wmi_set_ssid(wil, 7, "DIRECT-");
	if (rc)
		goto out_stop;

	rc = wmi_start_listen(wil);
	if (rc)
		goto out_stop;


	if (roc->buf) {
		/* TODO: Tx mgmt */
	}
	if (rc)
		return rc;

	roc->started = true;

	INIT_DELAYED_WORK(&roc->work, wil_roc_cancel_work);
	queue_delayed_work(wil->roc_wq, &roc->work,
			   msecs_to_jiffies(roc->duration));

	return rc;

out_stop:
	wmi_stop_discovery(wil);
	return rc;
}

int wil_prepare_roc(struct wil6210_priv *wil, struct ieee80211_channel *chan,
		    const u8 *buf, size_t len, unsigned int duration,
		    u64 *cookie)
{
	struct wil_roc *roc = &wil->roc;
	int rc = 0;

	wil_dbg_misc(wil, "%s(%d, %d ms)\n", __func__, chan->center_freq,
		     duration);

	if (!chan || !cookie)
		return -EINVAL;

	mutex_lock(&wil->mutex);

	if (roc->cookie) {
		rc = -EBUSY;
		goto out;
	}

	if (!duration && !buf)
		duration = 10;

	wil->roc_counter++;
	roc->cookie = wil->roc_counter;
	if (WARN_ON(roc->cookie == 0)) {
		roc->cookie = 1;
		wil->roc_counter++;
	}

	roc->chan = chan;
	roc->duration = duration;
	if (buf) {
		roc->buf = buf;
		roc->len = len;
		roc->mgmt_tx_cookie = (unsigned long)buf;
	} else {
		roc->mgmt_tx_cookie = 0;
	}
	rc = wil_start_roc(wil);
	if (roc->mgmt_tx_cookie) {
		*cookie = roc->mgmt_tx_cookie;
		cfg80211_mgmt_tx_status(wil->wdev, *cookie, buf, len,
					rc == 0, GFP_KERNEL);
	}
	if (rc == 0) {
		if (!roc->mgmt_tx_cookie) {
			*cookie = roc->cookie;
			cfg80211_ready_on_channel(wil->wdev, roc->cookie,
						  roc->chan, roc->duration,
						  GFP_KERNEL);
		}
	} else {
		roc->cookie = 0;
		roc->mgmt_tx_cookie = 0;
	}

out:
	mutex_unlock(&wil->mutex);
	return rc;
}
