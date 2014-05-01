/*
 * Copyright (c) 2012 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/etherdevice.h>
#include <net/ieee80211_radiotap.h>
#include <linux/if_arp.h>
#include <linux/moduleparam.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/ipv6.h>

#include "wil6210.h"
#include "wmi.h"
#include "txrx.h"
#include "trace.h"

struct  wil_skb_cb {
	int sngl_mapped;	/* Last desc. index, single_mapped skb data */
	int nodma_desc;		/* Header desc. index with no DMA completion
				happens in some modes, e.g. in TSO */
};


static bool rtap_include_phy_info;
module_param(rtap_include_phy_info, bool, S_IRUGO);
MODULE_PARM_DESC(rtap_include_phy_info,
		 " Include PHY info in the radiotap header, default - no");

static inline int wil_vring_is_empty(struct vring *vring)
{
	return vring->swhead == vring->swtail;
}

static inline u32 wil_vring_next_tail(struct vring *vring)
{
	return (vring->swtail + 1) % vring->size;
}

static inline void wil_vring_advance_head(struct vring *vring, int n)
{
	vring->swhead = (vring->swhead + n) % vring->size;
}

static inline int wil_vring_is_full(struct vring *vring)
{
	return wil_vring_next_tail(vring) == vring->swhead;
}
/*
 * Available space in Tx Vring
 */
static inline int wil_vring_avail_tx(struct vring *vring)
{
	u32 swhead = vring->swhead;
	u32 swtail = vring->swtail;
	int used = (vring->size + swhead - swtail) % vring->size;

	return vring->size - used - 1;
}

static int wil_vring_alloc(struct wil6210_priv *wil, struct vring *vring)
{
	struct device *dev = wil_to_dev(wil);
	size_t sz = vring->size * sizeof(vring->va[0]);
	uint i;

	BUILD_BUG_ON(sizeof(vring->va[0]) != 32);

	vring->swhead = 0;
	vring->swtail = 0;
	vring->ctx = kzalloc(vring->size * sizeof(vring->ctx[0]), GFP_KERNEL);
	if (!vring->ctx) {
		vring->va = NULL;
		return -ENOMEM;
	}
	/*
	 * vring->va should be aligned on its size rounded up to power of 2
	 * This is granted by the dma_alloc_coherent
	 */
	vring->va = dma_alloc_coherent(dev, sz, &vring->pa, GFP_KERNEL);
	if (!vring->va) {
		wil_err(wil, "vring_alloc [%d] failed to alloc DMA mem\n",
			vring->size);
		kfree(vring->ctx);
		vring->ctx = NULL;
		return -ENOMEM;
	}
	/* initially, all descriptors are SW owned
	 * For Tx and Rx, ownership bit is at the same location, thus
	 * we can use any
	 */
	for (i = 0; i < vring->size; i++) {
		volatile struct vring_tx_desc *_d = &(vring->va[i].tx);
		_d->dma.status = TX_DMA_STATUS_DU;
	}

	wil_dbg_misc(wil, "vring[%d] 0x%p:0x%016llx 0x%p\n", vring->size,
		     vring->va, (unsigned long long)vring->pa, vring->ctx);

	return 0;
}

static void wil_vring_free(struct wil6210_priv *wil, struct vring *vring,
			   int tx)
{
	struct device *dev = wil_to_dev(wil);
	size_t sz = vring->size * sizeof(vring->va[0]);
	struct wil_skb_cb *skbcb;

	while (!wil_vring_is_empty(vring)) {
		if (tx) {
			volatile struct vring_tx_desc *d =
					&vring->va[vring->swtail].tx;
			dma_addr_t pa = d->dma.addr_low |
					((u64)d->dma.addr_high << 32);
			struct sk_buff *skb = vring->ctx[vring->swtail];
			if (skb) {
				skbcb = (struct wil_skb_cb *)skb->cb;
				if (vring->swtail <= skbcb->sngl_mapped)
					dma_unmap_single(dev, pa,
						d->dma.length, DMA_TO_DEVICE);
				else
					dma_unmap_page(dev, pa,
						d->dma.length, DMA_TO_DEVICE);
				dev_kfree_skb_any(skb);
				vring->ctx[vring->swtail] = NULL;
			}
			vring->swtail = wil_vring_next_tail(vring);
		} else { /* rx */
			volatile struct vring_rx_desc *d =
					&vring->va[vring->swtail].rx;
			dma_addr_t pa = d->dma.addr_low |
					((u64)d->dma.addr_high << 32);
			struct sk_buff *skb = vring->ctx[vring->swhead];
			dma_unmap_single(dev, pa, d->dma.length,
					 DMA_FROM_DEVICE);
			kfree_skb(skb);
			wil_vring_advance_head(vring, 1);
		}
	}
	dma_free_coherent(dev, sz, (void *)vring->va, vring->pa);
	kfree(vring->ctx);
	vring->pa = 0;
	vring->va = NULL;
	vring->ctx = NULL;
}

/**
 * Allocate one skb for Rx VRING
 *
 * Safe to call from IRQ
 */
static int wil_vring_alloc_skb(struct wil6210_priv *wil, struct vring *vring,
			       u32 i, int headroom)
{
	struct device *dev = wil_to_dev(wil);
	unsigned int sz = RX_BUF_LEN;
	volatile struct vring_rx_desc *d = &(vring->va[i].rx);
	dma_addr_t pa;

	/* TODO align */
	struct sk_buff *skb = dev_alloc_skb(sz + headroom);
	if (unlikely(!skb))
		return -ENOMEM;

	skb_reserve(skb, headroom);
	skb_put(skb, sz);

	pa = dma_map_single(dev, skb->data, skb->len, DMA_FROM_DEVICE);
	if (unlikely(dma_mapping_error(dev, pa))) {
		kfree_skb(skb);
		return -ENOMEM;
	}

	d->dma.d0 = BIT(9) | RX_DMA_D0_CMD_DMA_IT;
	d->dma.addr_low = lower_32_bits(pa);
	d->dma.addr_high = (u16)upper_32_bits(pa);
	/* ip_length don't care */
	/* b11 don't care */
	/* error don't care */
	d->dma.status = 0; /* BIT(0) should be 0 for HW_OWNED */
	d->dma.length = sz;
	vring->ctx[i] = skb;

	return 0;
}

/**
 * Adds radiotap header
 *
 * Any error indicated as "Bad FCS"
 *
 * Vendor data for 04:ce:14-1 (Wilocity-1) consists of:
 *  - Rx descriptor: 32 bytes
 *  - Phy info
 */
static void wil_rx_add_radiotap_header(struct wil6210_priv *wil,
				       struct sk_buff *skb)
{
	struct wireless_dev *wdev = wil->wdev;
	struct wil6210_rtap {
		struct ieee80211_radiotap_header rthdr;
		/* fields should be in the order of bits in rthdr.it_present */
		/* flags */
		u8 flags;
		/* channel */
		__le16 chnl_freq __aligned(2);
		__le16 chnl_flags;
		/* MCS */
		u8 mcs_present;
		u8 mcs_flags;
		u8 mcs_index;
	} __packed;
	struct wil6210_rtap_vendor {
		struct wil6210_rtap rtap;
		/* vendor */
		u8 vendor_oui[3] __aligned(2);
		u8 vendor_ns;
		__le16 vendor_skip;
		u8 vendor_data[0];
	} __packed;
	struct vring_rx_desc *d = wil_skb_rxdesc(skb);
	struct wil6210_rtap_vendor *rtap_vendor;
	int rtap_len = sizeof(struct wil6210_rtap);
	int phy_length = 0; /* phy info header size, bytes */
	static char phy_data[128];
	struct ieee80211_channel *ch = wdev->preset_chandef.chan;

	if (rtap_include_phy_info) {
		rtap_len = sizeof(*rtap_vendor) + sizeof(*d);
		/* calculate additional length */
		if (d->dma.status & RX_DMA_STATUS_PHY_INFO) {
			/**
			 * PHY info starts from 8-byte boundary
			 * there are 8-byte lines, last line may be partially
			 * written (HW bug), thus FW configures for last line
			 * to be excessive. Driver skips this last line.
			 */
			int len = min_t(int, 8 + sizeof(phy_data),
					wil_rxdesc_phy_length(d));
			if (len > 8) {
				void *p = skb_tail_pointer(skb);
				void *pa = PTR_ALIGN(p, 8);
				if (skb_tailroom(skb) >= len + (pa - p)) {
					phy_length = len - 8;
					memcpy(phy_data, pa, phy_length);
				}
			}
		}
		rtap_len += phy_length;
	}

	if (skb_headroom(skb) < rtap_len &&
	    pskb_expand_head(skb, rtap_len, 0, GFP_ATOMIC)) {
		wil_err(wil, "Unable to expand headrom to %d\n", rtap_len);
		return;
	}

	rtap_vendor = (void *)skb_push(skb, rtap_len);
	memset(rtap_vendor, 0, rtap_len);

	rtap_vendor->rtap.rthdr.it_version = PKTHDR_RADIOTAP_VERSION;
	rtap_vendor->rtap.rthdr.it_len = cpu_to_le16(rtap_len);
	rtap_vendor->rtap.rthdr.it_present = cpu_to_le32(
			(1 << IEEE80211_RADIOTAP_FLAGS) |
			(1 << IEEE80211_RADIOTAP_CHANNEL) |
			(1 << IEEE80211_RADIOTAP_MCS));
	if (d->dma.status & RX_DMA_STATUS_ERROR)
		rtap_vendor->rtap.flags |= IEEE80211_RADIOTAP_F_BADFCS;

	rtap_vendor->rtap.chnl_freq = cpu_to_le16(ch ? ch->center_freq : 58320);
	rtap_vendor->rtap.chnl_flags = cpu_to_le16(0);

	rtap_vendor->rtap.mcs_present = IEEE80211_RADIOTAP_MCS_HAVE_MCS;
	rtap_vendor->rtap.mcs_flags = 0;
	rtap_vendor->rtap.mcs_index = wil_rxdesc_mcs(d);

	if (rtap_include_phy_info) {
		rtap_vendor->rtap.rthdr.it_present |= cpu_to_le32(1 <<
				IEEE80211_RADIOTAP_VENDOR_NAMESPACE);
		/* OUI for Wilocity 04:ce:14 */
		rtap_vendor->vendor_oui[0] = 0x04;
		rtap_vendor->vendor_oui[1] = 0xce;
		rtap_vendor->vendor_oui[2] = 0x14;
		rtap_vendor->vendor_ns = 1;
		/* Rx descriptor + PHY data  */
		rtap_vendor->vendor_skip = cpu_to_le16(sizeof(*d) +
						       phy_length);
		memcpy(rtap_vendor->vendor_data, (void *)d, sizeof(*d));
		memcpy(rtap_vendor->vendor_data + sizeof(*d), phy_data,
		       phy_length);
	}
}

/*
 * Fast swap in place between 2 registers
 */
static void wil_swap_u16(u16 *a, u16 *b)
{
	*a ^= *b;
	*b ^= *a;
	*a ^= *b;
}

static void wil_swap_ethaddr(void *data)
{
	struct ethhdr *eth = data;
	u16 *s = (u16 *)eth->h_source;
	u16 *d = (u16 *)eth->h_dest;

	wil_swap_u16(s++, d++);
	wil_swap_u16(s++, d++);
	wil_swap_u16(s, d);
}

/**
 * reap 1 frame from @swhead
 *
 * Rx descriptor copied to skb->cb
 *
 * Safe to call from IRQ
 */
static struct sk_buff *wil_vring_reap_rx(struct wil6210_priv *wil,
					 struct vring *vring)
{
	struct device *dev = wil_to_dev(wil);
	struct net_device *ndev = wil_to_ndev(wil);
	volatile struct vring_rx_desc *d;
	struct vring_rx_desc *d1;
	struct sk_buff *skb;
	dma_addr_t pa;
	unsigned int sz = RX_BUF_LEN;
	u8 ftype;
	u8 ds_bits;
	int cid;
	struct wil_net_stats *stats;


	BUILD_BUG_ON(sizeof(struct vring_rx_desc) > sizeof(skb->cb));

	if (wil_vring_is_empty(vring))
		return NULL;

	d = &(vring->va[vring->swhead].rx);
	if (!(d->dma.status & RX_DMA_STATUS_DU)) {
		/* it is not error, we just reached end of Rx done area */
		return NULL;
	}

	pa = d->dma.addr_low | ((u64)d->dma.addr_high << 32);
	skb = vring->ctx[vring->swhead];
	vring->ctx[vring->swhead] = NULL;
	dma_unmap_single(dev, pa, sz, DMA_FROM_DEVICE);
	skb_trim(skb, d->dma.length);

	d1 = wil_skb_rxdesc(skb);
	*d1 = *d;
	cid = wil_rxdesc_cid(d1);
	stats = &wil->sta[cid].stats;

	wil->stats.last_mcs_rx = stats->last_mcs_rx = wil_rxdesc_mcs(d1);

	/* use radiotap header only if required */
	if (ndev->type == ARPHRD_IEEE80211_RADIOTAP)
		wil_rx_add_radiotap_header(wil, skb);

	trace_wil6210_rx(vring->swhead, d1);
	wil_dbg_txrx(wil, "Rx[%3d] : %d bytes\n", vring->swhead,
		     d1->dma.length);
	wil_hex_dump_txrx("Rx ", DUMP_PREFIX_NONE, 32, 4,
			  (const void *)d1, sizeof(*d1), false);

	wil_vring_advance_head(vring, 1);

	/* no extra checks if in sniffer mode */
	if (ndev->type != ARPHRD_ETHER)
		return skb;
	/*
	 * Non-data frames may be delivered through Rx DMA channel (ex: BAR)
	 * Driver should recognize it by frame type, that is found
	 * in Rx descriptor. If type is not data, it is 802.11 frame as is
	 */
	ftype = wil_rxdesc_ftype(d1) << 2;
	if (ftype != IEEE80211_FTYPE_DATA) {
		wil_dbg_txrx(wil, "Non-data frame ftype 0x%08x\n", ftype);
		/* TODO: process it */
		kfree_skb(skb);
		return NULL;
	}

	if (skb->len < ETH_HLEN) {
		wil_err(wil, "Short frame, len = %d\n", skb->len);
		/* TODO: process it (i.e. BAR) */
		kfree_skb(skb);
		return NULL;
	}

	ds_bits = wil_rxdesc_ds_bits(d1);
	if (ds_bits == 1) {
		/*
		 * HW bug - in ToDS mode, i.e. Rx on AP side,
		 * addresses get swapped
		 */
		wil_swap_ethaddr(skb->data);
	}
	/* Check checksum-offload status */
	if (ndev->features & NETIF_F_RXCSUM) {
		if (d->dma.status & RX_DMA_STATUS_L4_IDENT) {
			/* L4 protocol identified, csum calculated */
			if ((d->dma.error & RX_DMA_ERROR_L4_ERR) == 0)
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			else
				wil_err(wil, "Incorrect checksum reported\n");
		}
	}
	return skb;
}

/**
 * allocate and fill up to @count buffers in rx ring
 * buffers posted at @swtail
 */
static int wil_rx_refill(struct wil6210_priv *wil, int count)
{
	struct net_device *ndev = wil_to_ndev(wil);
	struct vring *v = &wil->vring_rx;
	u32 next_tail;
	int rc = 0;
	int headroom = ndev->type == ARPHRD_IEEE80211_RADIOTAP ?
			WIL6210_RTAP_SIZE : 0;

	for (; next_tail = wil_vring_next_tail(v),
			(next_tail != v->swhead) && (count-- > 0);
			v->swtail = next_tail) {
		rc = wil_vring_alloc_skb(wil, v, v->swtail, headroom);
		if (rc) {
			wil_err(wil, "Error %d in wil_rx_refill[%d]\n",
				rc, v->swtail);
			break;
		}
	}
	iowrite32(v->swtail, wil->csr + HOSTADDR(v->hwtail));

	return rc;
}

/*
 * Pass Rx packet to the netif. Update statistics.
 * Called in softirq context (NAPI poll).
 */
void wil_netif_rx_any(struct sk_buff *skb, struct net_device *ndev)
{
	int rc;
	struct wil6210_priv *wil = ndev_to_wil(ndev);
	unsigned int len = skb->len;
	struct vring_rx_desc *d = wil_skb_rxdesc(skb);
	int cid = wil_rxdesc_cid(d);
	struct wil_net_stats *stats = &wil->sta[cid].stats;

	skb_orphan(skb);

	rc = netif_receive_skb(skb);

	if (likely(rc == NET_RX_SUCCESS)) {
		ndev->stats.rx_packets++;
		stats->rx_packets++;
		ndev->stats.rx_bytes += len;
		stats->rx_bytes += len;

	} else {
		ndev->stats.rx_dropped++;
		stats->rx_dropped++;
		wil_dbg_txrx(wil, "Rx drop %d bytes\n", len);
	}
}

/**
 * Proceed all completed skb's from Rx VRING
 *
 * Safe to call from NAPI poll, i.e. softirq with interrupts enabled
 */
void wil_rx_handle(struct wil6210_priv *wil, int *quota)
{
	struct net_device *ndev = wil_to_ndev(wil);
	struct vring *v = &wil->vring_rx;
	struct sk_buff *skb;

	if (!v->va) {
		wil_err(wil, "Rx IRQ while Rx not yet initialized\n");
		return;
	}
	wil_dbg_txrx(wil, "%s()\n", __func__);
	while ((*quota > 0) && (NULL != (skb = wil_vring_reap_rx(wil, v)))) {
		wil_hex_dump_txrx("Rx ", DUMP_PREFIX_OFFSET, 16, 1,
				  skb->data, skb_headlen(skb), false);

		(*quota)--;

		if (wil->wdev->iftype == NL80211_IFTYPE_MONITOR) {
			skb->dev = ndev;
			skb_reset_mac_header(skb);
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			skb->pkt_type = PACKET_OTHERHOST;
			skb->protocol = htons(ETH_P_802_2);
			wil_netif_rx_any(skb, ndev);
		} else {
			skb->protocol = eth_type_trans(skb, ndev);
			wil_rx_reorder(wil, skb);
		}

	}
	wil_rx_refill(wil, v->size);
}

int wil_rx_init(struct wil6210_priv *wil)
{
	struct vring *vring = &wil->vring_rx;
	int rc;

	vring->size = WIL6210_RX_RING_SIZE;
	rc = wil_vring_alloc(wil, vring);
	if (rc)
		return rc;

	rc = wmi_rx_chain_add(wil, vring);
	if (rc)
		goto err_free;

	rc = wil_rx_refill(wil, vring->size);
	if (rc)
		goto err_free;

	return 0;
 err_free:
	wil_vring_free(wil, vring, 0);

	return rc;
}

void wil_rx_fini(struct wil6210_priv *wil)
{
	struct vring *vring = &wil->vring_rx;

	if (vring->va)
		wil_vring_free(wil, vring, 0);
}

int wil_vring_init_tx(struct wil6210_priv *wil, int id, int size,
		      int cid, int tid)
{
	int rc;
	struct wmi_vring_cfg_cmd cmd = {
		.action = cpu_to_le32(WMI_VRING_CMD_ADD),
		.vring_cfg = {
			.tx_sw_ring = {
				.max_mpdu_size = cpu_to_le16(TX_BUF_LEN),
				.ring_size = cpu_to_le16(size),
			},
			.ringid = id,
			.cidxtid = mk_cidxtid(cid, tid),
			.encap_trans_type = WMI_VRING_ENC_TYPE_802_3,
			.mac_ctrl = 0,
			.to_resolution = 0,
			.schd_params = {
				.priority = cpu_to_le16(0),
				.timeslot_us = cpu_to_le16(0xfff),
			},
		},
	};
	struct {
		struct wil6210_mbox_hdr_wmi wmi;
		struct wmi_vring_cfg_done_event cmd;
	} __packed reply;
	struct vring *vring = &wil->vring_tx[id];
	struct vring_tx_data *txdata = &wil->vring_tx_data[id];

	if (vring->va) {
		wil_err(wil, "Tx ring [%d] already allocated\n", id);
		rc = -EINVAL;
		goto out;
	}

	memset(txdata, 0, sizeof(*txdata));
	vring->size = size;
	rc = wil_vring_alloc(wil, vring);
	if (rc)
		goto out;

	wil->vring2cid_tid[id][0] = cid;
	wil->vring2cid_tid[id][1] = tid;

	cmd.vring_cfg.tx_sw_ring.ring_mem_base = cpu_to_le64(vring->pa);

	rc = wmi_call(wil, WMI_VRING_CFG_CMDID, &cmd, sizeof(cmd),
		      WMI_VRING_CFG_DONE_EVENTID, &reply, sizeof(reply), 100);
	if (rc)
		goto out_free;

	if (reply.cmd.status != WMI_FW_STATUS_SUCCESS) {
		wil_err(wil, "Tx config failed, status 0x%02x\n",
			reply.cmd.status);
		rc = -EINVAL;
		goto out_free;
	}
	vring->hwtail = le32_to_cpu(reply.cmd.tx_vring_tail_ptr);

	return 0;
 out_free:
	wil_vring_free(wil, vring, 1);
 out:

	return rc;
}

void wil_vring_fini_tx(struct wil6210_priv *wil, int id)
{
	struct vring *vring = &wil->vring_tx[id];

	if (!vring->va)
		return;

	wil_vring_free(wil, vring, 1);
}


/*
*
*/
static struct vring *wil_find_first_tx_vring(struct wil6210_priv *wil,
				       int *vring_index)
{
	int i =0;
	struct vring *v;
	/* find 1-st vring */
	for(i = 0; i < WIL6210_MAX_TX_RINGS; i++) {
		v = &wil->vring_tx[i];
		if (v && v->va) {
			*vring_index = i;
			return v;
		}
	}
	*vring_index = 0;
	return NULL;

}

static struct vring *wil_find_tx_vring(struct wil6210_priv *wil,
				       struct sk_buff *skb)
{
	int i;
	struct ethhdr *eth = (void *)skb->data;
	int cid = wil_find_cid(wil, eth->h_dest);

	if (cid < 0)
		return NULL;

	/* TODO: fix for multiple TID */
	for (i = 0; i < ARRAY_SIZE(wil->vring2cid_tid); i++) {
		if (wil->vring2cid_tid[i][0] == cid) {
			struct vring *v = &wil->vring_tx[i];
			wil_dbg_txrx(wil, "%s(%pM) -> [%d]\n",
				     __func__, eth->h_dest, i);
			if (v->va) {
				return v;
			} else {
				wil_dbg_txrx(wil, "vring[%d] not valid\n", i);
				return NULL;
			}
		}
	}

	return NULL;
}

static void wil_set_da_for_vring(struct wil6210_priv *wil,
				 struct sk_buff *skb, int vring_index)
{
	struct ethhdr *eth = (void *)skb->data;
	int cid = wil->vring2cid_tid[vring_index][0];
	memcpy(eth->h_dest, wil->sta[cid].addr, ETH_ALEN);
}

static int wil_tx_vring(struct wil6210_priv *wil, struct vring *vring,
			struct sk_buff *skb);
/*
 * Find 1-st vring and return it; set dest address for this vring in skb
 * duplicate skb and send it to other active vrings
 */
static struct vring *wil_tx_bcast(struct wil6210_priv *wil,
				       struct sk_buff *skb)
{
	struct vring *v, *v2;
	struct sk_buff *skb2;
	struct wireless_dev *wdev = wil->wdev;
	int i=0;
	int bcast_to_ucast = 1;

	/* In case we in STA side BCAST address will not be replaces
	   With unicast, packet will be sent on all vrings as BCAST
	*/
	if (wdev->iftype == NL80211_IFTYPE_STATION ||
	    wdev->iftype == NL80211_IFTYPE_P2P_CLIENT) {
		 bcast_to_ucast = 0;
	}
	/* find 1-st vring */
	v = wil_find_first_tx_vring(wil, &i);
	if (v && v->va)
		goto found;

	wil_err(wil, "Tx while no vrings active?\n");

	return NULL;

found:

	if (bcast_to_ucast) {
		wil_set_da_for_vring(wil, skb, i);
	}

    i++; // look for other virings to send this skb

	/* find other active vrings and duplicate skb for each */
	for(;i < WIL6210_MAX_TX_RINGS; i++) {
		v2 = &wil->vring_tx[i];
		if (!v2->va)
			continue;
		skb2 = skb_copy(skb, GFP_ATOMIC);
		if (skb2) {
			if (bcast_to_ucast) {
				wil_dbg_txrx(wil, "BCAST DUP and swap da addr -> ring %d\n", i);
				wil_set_da_for_vring(wil, skb2, i);
			}
			wil_tx_vring(wil, v2, skb2);
		} else {
			wil_err(wil, "skb_copy failed\n");
		}
	}

	return v;
}

static int wil_tx_desc_map(volatile struct vring_tx_desc *d,
			   dma_addr_t pa, u32 len, int vring_index)
{
	d->dma.addr_low = lower_32_bits(pa);
	d->dma.addr_high = (u16)upper_32_bits(pa);
	d->dma.offload_cfg = 0;
	d->dma.error = 0;
	d->dma.status = 0; /* BIT(0) should be 0 for HW_OWNED */
	d->dma.length = len;
	d->dma.d0 = (vring_index << DMA_CFG_DESC_TX_0_QID_POS);
	d->mac.d[0] = 0;
	d->mac.d[1] = 0;
	d->mac.d[2] = 0;
	d->mac.ucode_cmd = 0;
	/* use dst index 0 */
	d->mac.d[1] |= BIT(MAC_CFG_DESC_TX_1_DST_INDEX_EN_POS) |
		       (0 << MAC_CFG_DESC_TX_1_DST_INDEX_POS);
	/* translation type:  0 - bypass; 1 - 802.3; 2 - native wifi */
	d->mac.d[2] = BIT(MAC_CFG_DESC_TX_2_SNAP_HDR_INSERTION_EN_POS) |
		      (1 << MAC_CFG_DESC_TX_2_L2_TRANSLATION_TYPE_POS);

	return 0;
}

/**
 * Sets the descriptor @d up for csum and/or TSO offloaing. The corresponding
 * @skb is used to obtain the protocol and headers length.
 * @tso_desc_type is a descriptor type for TSO: -1 - no TSO send,
 * 0 - a header, 1 - first data, 2 - middle, 3 - last descriptor.
 * Returns the protocol: 0 - not TCP, 1 - TCPv4, 2 - TCPv6.
 * Note, if d==NULL, the function only returns the protocol result.
 */
static int wil_tx_desc_offload_setup(struct vring_tx_desc *d,
				struct sk_buff *skb, int tso_desc_type)
{
	int is_ip4 = 0, is_ip6 = 0, is_tcp = 0, is_udp = 0;


	if (skb->protocol == htons(ETH_P_IP)) {
		is_ip4 = 1;
	if (ip_hdr(skb)->protocol == IPPROTO_TCP)
		is_tcp = 1;
	else if (ip_hdr(skb)->protocol == IPPROTO_UDP)
		is_udp = 1;
	} else if (skb->protocol == htons(ETH_P_IPV6)) {
		unsigned int offset = 0;
		int ipv6hdr =  ipv6_find_hdr(skb,
					&offset, -1, NULL, NULL);
		is_ip6 = 1;
		if (ipv6hdr == NEXTHDR_TCP)
			is_tcp = 1;
		else if (ipv6hdr == NEXTHDR_UDP)
			is_udp = 1;
	}

	if (d && (is_ip4 || is_ip6)) {
		if (is_ip4)
			d->dma.offload_cfg |=
				BIT(DMA_CFG_DESC_TX_OFFLOAD_CFG_L3T_IPV4_POS);
		d->dma.offload_cfg |=
			(skb_network_header_len(skb) &
			DMA_CFG_DESC_TX_OFFLOAD_CFG_IP_LEN_MSK);
		d->dma.offload_cfg |=
			(0x0e << DMA_CFG_DESC_TX_OFFLOAD_CFG_MAC_LEN_POS);
		if (is_tcp || is_udp) {
			/* Enable TCP/UDP checksum */
			d->dma.d0 |=
				BIT(DMA_CFG_DESC_TX_0_TCP_UDP_CHECKSUM_EN_POS);
			/* Calculate pseudo-header */
			d->dma.d0 |=
			   BIT(DMA_CFG_DESC_TX_0_PSEUDO_HEADER_CALC_EN_POS);
			if (is_tcp)  {
				d->dma.d0 |=
					(2 << DMA_CFG_DESC_TX_0_L4_TYPE_POS);
			/* L4 header len: TCP header length */
				d->dma.d0 |=
					(tcp_hdrlen(skb) &
					DMA_CFG_DESC_TX_0_L4_LENGTH_MSK);
				if (tso_desc_type != -1) {
					/* Setup TSO: the bit and desc type */
					d->dma.d0 |=
				       (BIT(DMA_CFG_DESC_TX_0_TCP_SEG_EN_POS))
				       | (tso_desc_type <<
				    DMA_CFG_DESC_TX_0_SEGMENT_BUF_DETAILS_POS);
					if (is_ip4)
						d->dma.d0 |=
				BIT(DMA_CFG_DESC_TX_0_IPV4_CHECKSUM_EN_POS);
					d->mac.d[2] |= (1 << /* Descs count */
				MAC_CFG_DESC_TX_2_NUM_OF_DESCRIPTORS_POS);

				}
			} else  {
				/* L4 header len: UDP header length */
				d->dma.d0 |=
					(sizeof(struct udphdr) &
					DMA_CFG_DESC_TX_0_L4_LENGTH_MSK);
			}
		}
	}
	return is_tcp ? (is_ip4 ? 1 : 2) : 0;
}

static inline void wil_tx_last_desc(struct vring_tx_desc *d, int vring_index)
{
	d->dma.d0 |= BIT(DMA_CFG_DESC_TX_0_CMD_EOP_POS)
		| BIT(9) /* BUG: undocumented bit */
		| BIT(DMA_CFG_DESC_TX_0_CMD_DMA_IT_POS)
		| (vring_index << DMA_CFG_DESC_TX_0_QID_POS);
}

static inline void wil_set_tx_desc_count(struct vring_tx_desc *d, int cnt)
{
	d->mac.d[2] &= ~(MAC_CFG_DESC_TX_2_NUM_OF_DESCRIPTORS_MSK);
	d->mac.d[2] |= (cnt << MAC_CFG_DESC_TX_2_NUM_OF_DESCRIPTORS_POS);
}


static int wil_tx_vring_tso(struct wil6210_priv *wil, struct vring *vring,
		struct sk_buff *skb)
{
	struct device *dev = wil_to_dev(wil);
	struct vring_tx_desc *d;
	struct vring_tx_desc *hdrdesc, *firstdata = NULL;
	struct wil_skb_cb *skbcb = (struct wil_skb_cb *)skb->cb;
	u32 swhead = vring->swhead;
	u32 swtail = vring->swtail;
	int used = (vring->size + swhead - swtail) % vring->size;
	int avail = vring->size - used - 1;
	int nr_frags = skb_shinfo(skb)->nr_frags;
	int min_desc_required = (nr_frags*3)+1;	/* Min required descriptors */
	int mss = skb_shinfo(skb)->gso_size;	/* payload size w/o headers */
	int f, len, hdrlen;
	int vring_index = vring - wil->vring_tx;
	int i = swhead;
	int descs_used = 1;	/* Tx descs used. At least 1 with the header */
	dma_addr_t pa;
	const struct skb_frag_struct *frag;
	int tcp_ver = 0;
	struct vring_tx_desc *sg_desc = NULL;
	int sg_desc_cnt = 0;
	int rem_data = 0;
	int lenmss;
	int end_of_first_chunck = 1;

	if (avail < vring->size/8)
		netif_tx_stop_all_queues(wil_to_ndev(wil));
	if (avail < min_desc_required) {
		/*
		 A typical page 4K is 3-4 payloads, we assume each fragment
		 is a full payload, that's how min_desc_required has been
		 calculated. In real we might need more or less descriptors,
		 this is the initial check only.
		*/
		wil_err(wil, "Tx ring full. Need %d descriptors\n",
			min_desc_required);
		return -ENOMEM;
	}

	tcp_ver = wil_tx_desc_offload_setup(NULL, skb, 0);
	if (tcp_ver == 0) {
		wil_err(wil, "TSO requires TCP protocol\n");
		return -EINVAL;
	}
	hdrlen = 0x0e +	/* MAC header len */
		(int)skb_network_header_len(skb) +	/* IP header len */
		tcp_hdrlen(skb);		/* TCP header len */

	if (tcp_ver == 1) {
		/* TCP v4, zero out the IP length and IPv4 checksum fields
		as required by the offloading doc */
		ip_hdr(skb)->tot_len = 0;
		ip_hdr(skb)->check = 0;
	} else {
		/* TCP v6, zero out the payload length */
		ipv6_hdr(skb)->payload_len = 0;
	}


	d = (struct vring_tx_desc *)&(vring->va[i].tx);
	pa = dma_map_single(dev, skb->data, hdrlen, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(dev, pa))) {
		wil_err(wil, "DMA map error\n");
		goto err_exit;
	}
	wil_tx_desc_map((struct vring_tx_desc *)d, pa, hdrlen, vring_index);
	hdrdesc = d;
	wil_tx_desc_offload_setup(d, skb, 0);
	wil_tx_last_desc(d, vring_index);

	vring->ctx[i] = skb_get(skb);
	skbcb->sngl_mapped = i;
	skbcb->nodma_desc = i;	/* TSO header desc makes no DMA complete */


    rem_data = mss;
	len = skb_headlen(skb) - hdrlen;
	if (len > 0) {
		/* There is some data besides the headers, use a different
		descriptor */
		i = (swhead + 1) % vring->size;
		d = (struct vring_tx_desc *)&(vring->va[i].tx);
		pa = dma_map_single(dev, skb->data+hdrlen, len,
				DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(dev, pa)))
				goto dma_error;
		wil_tx_desc_map((struct vring_tx_desc *)d, pa, len, vring_index);
		firstdata = d;	/* 1st data descriptor */
		wil_tx_desc_offload_setup(d, skb, 1);
		skbcb->sngl_mapped = i;
		vring->ctx[i] = skb_get(skb);
		descs_used++;
		sg_desc = d;
		sg_desc_cnt = 1;
		rem_data = mss - len;

	}
	/* Data segments. split each fragment to chunks with mss size */
	for (f = 0; f < nr_frags; f++)  {
		frag = &skb_shinfo(skb)->frags[f];
		len = frag->size;
		while (len > 0)  {
			if (rem_data == 0) {
				/* got full mss descs chain. Complete the
				previous descriptor */
				wil_tx_last_desc(d, vring_index);
				if (sg_desc)
					wil_set_tx_desc_count(sg_desc,
								sg_desc_cnt+end_of_first_chunck);
				rem_data = mss;
				sg_desc = NULL;
				sg_desc_cnt = 0;
                end_of_first_chunck = 0;
			} else {
				if (descs_used == avail)  {
					wil_err(wil,
					"Tx: ring overflow TSO\n");
					goto dma_error;
				}
				lenmss = len > rem_data ? rem_data : len;
				i = (swhead + descs_used) % vring->size;
				pa = skb_frag_dma_map(dev, frag,
						(frag->size - len),
						lenmss, DMA_TO_DEVICE);
				if (unlikely(dma_mapping_error(dev, pa)))
					goto dma_error;
				d = (struct vring_tx_desc *)&(vring->va[i].tx);
				wil_tx_desc_map((struct vring_tx_desc *)d,
						pa, lenmss, vring_index);
				if (sg_desc == NULL)
					sg_desc = d;

				if (firstdata)  {
					/* middle desc */
					wil_tx_desc_offload_setup(d, skb, 2);
				} else {
					/* 1st data desc */
					wil_tx_desc_offload_setup(d, skb, 1);
					firstdata = d;
				}
				vring->ctx[i] = skb_get(skb);
				descs_used++;
				sg_desc_cnt++;
				len -= lenmss;
				rem_data -= lenmss;
			}
		}
	}
	wil_tx_last_desc(d, vring_index);
	if (sg_desc)
		wil_set_tx_desc_count(sg_desc, sg_desc_cnt + end_of_first_chunck);

	/* Last data descriptor */
	d->dma.d0 |= (3 << DMA_CFG_DESC_TX_0_SEGMENT_BUF_DETAILS_POS);
	/* Compensate the header descriptor (no DMA) in 1st data descriptor */
	//wil_set_tx_desc_count(firstdata, (firstdata->mac.d[2] &
	//			MAC_CFG_DESC_TX_2_NUM_OF_DESCRIPTORS_MSK) + 1);
	/* Fill the number of descriptors */
	wil_set_tx_desc_count(hdrdesc, descs_used);

	/* advance swhead */
	wil_vring_advance_head(vring, descs_used);
	iowrite32(vring->swhead, wil->csr + HOSTADDR(vring->hwtail));
	return 0;

 dma_error:
	wil_err(wil, "DMA map page error\n");
	while (descs_used > 0) {
		i = (swhead + descs_used) % vring->size;
		d = (struct vring_tx_desc *)&(vring->va[i].tx);
		d->dma.status = TX_DMA_STATUS_DU;
		pa = d->dma.addr_low | ((u64)d->dma.addr_high << 32);
		if (i > skbcb->sngl_mapped)
			dma_unmap_page(dev, pa, d->dma.length, DMA_TO_DEVICE);
		else
			dma_unmap_single(dev, pa, d->dma.length,
					DMA_TO_DEVICE);
		descs_used--;
		dev_kfree_skb_any(skb);	/* Decrease skb reference count */
	}
err_exit:
	return -EINVAL;
}

static int wil_tx_vring(struct wil6210_priv *wil, struct vring *vring,
			struct sk_buff *skb)
{
	struct device *dev = wil_to_dev(wil);
	struct net_device *ndev = wil_to_ndev(wil);
	volatile struct vring_tx_desc *d;
	struct wil_skb_cb *skbcb = (struct wil_skb_cb *)skb->cb;
	u32 swhead = vring->swhead;
	int avail = wil_vring_avail_tx(vring);
	int nr_frags = skb_shinfo(skb)->nr_frags;
	uint f;
	int vring_index = vring - wil->vring_tx;
	int skblen = 0;
	uint i = swhead;
	dma_addr_t pa;

	wil_dbg_txrx(wil, "%s()\n", __func__);

	if (avail < vring->size/8)
		netif_tx_stop_all_queues(wil_to_ndev(wil));
	if (avail < 1 + nr_frags) {
		wil_err(wil, "Tx ring full. No space for %d fragments\n",
			1 + nr_frags);
		return -ENOMEM;
	}
	d = &(vring->va[i].tx);
	skbcb->sngl_mapped = i;
	skbcb->nodma_desc = -1;
    	skblen = skb_headlen(skb);
	pa = dma_map_single(dev, skb->data,
			skblen, DMA_TO_DEVICE);

	wil_dbg_txrx(wil, "Tx skb %d bytes %p -> %#08llx\n", skblen,
		     skb->data, (unsigned long long)pa);
	wil_hex_dump_txrx("Tx ", DUMP_PREFIX_OFFSET, 16, 1,
			  skb->data, skblen, false);

	if (unlikely(dma_mapping_error(dev, pa))) {
		return -EINVAL;
	}
	/* 1-st segment */
	wil_tx_desc_map(d, pa, skblen, vring_index);

	/*
	 * Process offloading
	 */
	if ((skb->ip_summed == CHECKSUM_PARTIAL) &&
		(ndev->features & NETIF_F_HW_CSUM))
		wil_tx_desc_offload_setup((struct vring_tx_desc *)d, skb, -1);

	d->mac.d[2] |= ((nr_frags + 1) <<
		       MAC_CFG_DESC_TX_2_NUM_OF_DESCRIPTORS_POS);
	/* Keep reference to skb till the fragments and skb_data are done */
	vring->ctx[i] = skb_get(skb);
	/* middle segments */
	for (f = 0; f < nr_frags; f++) {
		const struct skb_frag_struct *frag =
				&skb_shinfo(skb)->frags[f];
		int len = skb_frag_size(frag);
		i = (swhead + f + 1) % vring->size;
		d = &(vring->va[i].tx);
		pa = skb_frag_dma_map(dev, frag, 0, len,
				DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(dev, pa)))
			goto dma_error;

		wil_tx_desc_map(d, pa, len, vring_index);

		if ((skb->ip_summed == CHECKSUM_PARTIAL) &&
			(ndev->features & NETIF_F_HW_CSUM))
			wil_tx_desc_offload_setup((struct vring_tx_desc *)d,
						skb, -1);
        	d->mac.d[2] |= ((nr_frags + 1) <<
		       MAC_CFG_DESC_TX_2_NUM_OF_DESCRIPTORS_POS);

		/* Keep reference to skb till all the fragments are done */
		vring->ctx[i] = skb_get(skb);
	}
	/* for the last seg only */
	wil_tx_last_desc((struct vring_tx_desc *)d, vring_index);

	wil_hex_dump_txrx("Tx", DUMP_PREFIX_NONE, 32, 4,
			  (const void *)d, sizeof(*d), false);

	if (wil_vring_is_empty(vring)) /* performance monitoring */
		vring->idle += get_cycles() - vring->last_idle;

	/* advance swhead */
	wil_vring_advance_head(vring, nr_frags + 1);
	wil_dbg_txrx(wil, "Tx swhead %d -> %d\n", swhead, vring->swhead);

	trace_wil6210_tx(vring_index, swhead, skb->len, nr_frags);
	iowrite32(vring->swhead, wil->csr + HOSTADDR(vring->hwtail));

	return 0;
 dma_error:
 	/* unmap what we have mapped */
	/* Note: increment @f to operate with positive index */
	for (f++; f > 0; f--) {
		i = (swhead + f - 1) % vring->size;
		d = &(vring->va[i].tx);
		d->dma.status = TX_DMA_STATUS_DU;
		pa = d->dma.addr_low | ((u64)d->dma.addr_high << 32);
		if (i <= skbcb->sngl_mapped)
			dma_unmap_single(dev, pa, d->dma.length, DMA_TO_DEVICE);
		else
			dma_unmap_page(dev, pa, d->dma.length, DMA_TO_DEVICE);
		dev_kfree_skb_any(skb);	/* Decrease skb reference count */
	}

	return -EINVAL;
}


netdev_tx_t wil_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct wil6210_priv *wil = ndev_to_wil(ndev);
	struct ethhdr *eth = (void *)skb->data;
	struct vring *vring;
	int rc;
	int drop_bcast=0;

	wil_dbg_txrx(wil, "%s()\n", __func__);
	if (!test_bit(wil_status_fwready, &wil->status)) {
		wil_err(wil, "FW not ready\n");
		goto drop;
	}
	if (!test_bit(wil_status_fwconnected, &wil->status)) {
		wil_err(wil, "FW not connected\n");
		goto drop;
	}
	if (wil->wdev->iftype == NL80211_IFTYPE_MONITOR) {
		wil_err(wil, "Xmit in monitor mode not supported\n");
		goto drop;
	}
	if (skb->protocol == cpu_to_be16(ETH_P_PAE)) {
		rc = wmi_tx_eapol(wil, skb);
	} else {
		/* find vring, for unicast address */
		if (is_unicast_ether_addr(eth->h_dest)) {
			vring = wil_find_tx_vring(wil, skb);
		} else {
			vring = wil_tx_bcast(wil, skb);
			drop_bcast = 1;
		}
		/* No viring to send this packet - dropp it, in case of BCAST addr - ERROR */
		if (!vring) {
				if(drop_bcast)
					goto drop_err;
				goto drop;
		}
		/* set up vring entry */
		if (skb_is_gso(skb))
			rc = wil_tx_vring_tso(wil, vring, skb);
		else
			rc = wil_tx_vring(wil, vring, skb);
	}
	switch (rc) {
	case 0:
		/* statistics will be updated on the tx_complete */
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	case -ENOMEM:
		return NETDEV_TX_BUSY;
	default:
		break; /* goto drop; */
	}
 drop_err:
	wil_err(wil, "No Tx VRING found for %pM\n",
		eth->h_dest);
	netif_tx_stop_all_queues(ndev);
 drop:
	wil_dbg_txrx(wil, "No Tx VRING found for %pM just drop packet\n",
		eth->h_dest);
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);

	return NET_XMIT_DROP;
}

/**
 * Clean up transmitted skb's from the Tx VRING
 *
 * Return number of descriptors cleared
 *
 * Safe to call from IRQ
 */
int wil_tx_complete(struct wil6210_priv *wil, int ringid)
{
	struct net_device *ndev = wil_to_ndev(wil);
	struct device *dev = wil_to_dev(wil);
	struct vring *vring = &wil->vring_tx[ringid];
	int cid = wil->vring2cid_tid[ringid][0];
	struct wil_net_stats *stats = &wil->sta[cid].stats;
	struct wil_skb_cb *skbcb;
	int swtail, swtail_to_rel;
	int nodma_comp;
	int done = 0;
	struct sk_buff *skb;

	if (!vring->va) {
		wil_err(wil, "Tx irq[%d]: vring not initialized\n", ringid);
		return 0;
	}

	wil_dbg_txrx(wil, "%s(%d)\n", __func__, ringid);


    swtail_to_rel = vring->swtail;

	for (swtail = vring->swtail; swtail != vring->swhead;
		swtail = (swtail + 1) % vring->size) {

		volatile struct vring_tx_desc *d = (struct vring_tx_desc *)
			&vring->va[swtail].tx;

		if (d->dma.status & TX_DMA_STATUS_DU) {
            swtail_to_rel = swtail;
		}
    }

	for (swtail = vring->swtail; swtail != swtail_to_rel;
		swtail = (swtail + 1) % vring->size) {

		volatile struct vring_tx_desc *d = (struct vring_tx_desc *)
			&vring->va[swtail].tx;

		dma_addr_t pa;


		trace_wil6210_tx_done(ringid, vring->swtail, d->dma.length,
				      d->dma.error);
		wil_dbg_txrx(wil,
			     "Tx[%3d] : %d bytes, status 0x%02x err 0x%02x\n",
			     vring->swtail, d->dma.length, d->dma.status,
			     d->dma.error);
		wil_hex_dump_txrx("TxC ", DUMP_PREFIX_NONE, 32, 4,
				  (const void *)d, sizeof(*d), false);


		pa = d->dma.addr_low | ((u64)d->dma.addr_high << 32);
		nodma_comp = false;
		skbcb = NULL;

		if ((!nodma_comp) && (d->dma.d0 &
			BIT(DMA_CFG_DESC_TX_0_CMD_EOP_POS)))
			if ((!(d->dma.status & TX_DMA_STATUS_DU))) {
				break;
			}

		skb = vring->ctx[swtail];

		if (skb) {
			if (d->dma.error == 0) {
				ndev->stats.tx_packets++;
				stats->tx_packets++;
				ndev->stats.tx_bytes += skb->len;
				stats->tx_bytes += skb->len;
			} else {
				ndev->stats.tx_errors++;
				stats->tx_errors++;
			}

			skbcb = (struct wil_skb_cb *)skb->cb;
			if (swtail == skbcb->nodma_desc)
				nodma_comp = true;
		}

		if (skbcb) {
			if (swtail <= skbcb->sngl_mapped) {
				dma_unmap_single(dev, pa, d->dma.length,
						DMA_TO_DEVICE);
			}
			else {
				dma_unmap_page(dev, pa, d->dma.length,
						DMA_TO_DEVICE);
            }
			dev_kfree_skb_any(skb);
 		}

		vring->ctx[swtail] = NULL;

		d->dma.addr_low = 0;
		d->dma.addr_high = 0;
		d->dma.length = 0;
		d->dma.status = 0;

		d->dma.offload_cfg = 0;
		done++;
	}

	if (wil_vring_is_empty(vring)) { /* performance monitoring */
		wil_dbg_txrx(wil, "Ring[%2d] empty\n", ringid);
		vring->last_idle = get_cycles();
	}
	vring->swtail = swtail;
	if (wil_vring_avail_tx(vring) > vring->size/4)
		netif_tx_wake_all_queues(wil_to_ndev(wil));

	return done;
}
