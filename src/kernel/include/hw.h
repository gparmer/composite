/**
 * Copyright 2016 by Gabriel Parmer, gparmer@gwu.edu
 *
 * Redistribution of this file is permitted under the GNU General
 * Public License v2.
 */

#ifndef HW_H
#define HW_H

#include "component.h"
#include "thd.h"
#include "chal/call_convention.h"
#include "inv.h"

#define HW_IRQ_TOTAL 256
#define HW_IRQ_EXTERNAL_MIN 32
#define HW_IRQ_EXTERNAL_MAX 63

/*
 * X86_64-FIXME:
 * Change globa var hw_asnd_caps to static
 */
static struct cap_asnd hw_asnd_caps[HW_IRQ_TOTAL];

struct cap_hw {
	struct cap_header h;
	u32_t             hw_bitmap;
} __attribute__((packed));

static void
hw_asndcap_init(void)
{
	memset(&hw_asnd_caps, 0, sizeof(struct cap_asnd) * HW_IRQ_TOTAL);
}

/*
 * FIXME: This is broken as it allows someone to create a hwcap with an
 * arbitrary bitmap.  This should be changed to only create a hwcap
 * from another, and only with a subset of the bitmap.  Any other HW
 * resources should not be passed on.
 */
static int
hw_activate(struct captbl *t, capid_t cap, capid_t capin, u32_t bitmap)
{
	struct cap_hw *hwc;
	int            ret;

	hwc = (struct cap_hw *)__cap_capactivate_pre(t, cap, capin, CAP_HW, &ret);
	if (!hwc) return ret;

	hwc->hw_bitmap = bitmap;

	__cap_capactivate_post(&hwc->h, CAP_HW);

	return 0;
}

static int
hw_deactivate(struct cap_captbl *t, capid_t capin, livenessid_t lid)
{
	return cap_capdeactivate(t, capin, CAP_HW, lid);
}

static int
hw_attach_rcvcap(struct cap_hw *hwc, hwid_t hwid, struct cap_arcv *rcvc, capid_t rcv_cap)
{
	if (hwid < HW_IRQ_EXTERNAL_MIN || hwid > HW_IRQ_EXTERNAL_MAX) return -EINVAL;
	if (!(hwc->hw_bitmap & (1 << (hwid - HW_IRQ_EXTERNAL_MIN)))) return -EINVAL;
	if (hw_asnd_caps[hwid].h.type == CAP_ASND) return -EEXIST;

	return asnd_construct(&hw_asnd_caps[hwid], rcvc, rcv_cap);
}

static int
hw_detach_rcvcap(struct cap_hw *hwc, hwid_t hwid)
{
	if (hwid < HW_IRQ_EXTERNAL_MIN || hwid > HW_IRQ_EXTERNAL_MAX) return -EINVAL;
	if (!(hwc->hw_bitmap & (1 << (hwid - HW_IRQ_EXTERNAL_MIN)))) return -EINVAL;

	/*
	 * FIXME: Need to synchronize using __xx_pre and
	 *        __xx_post perhaps in asnd_deconstruct()
	 */
	memset(&hw_asnd_caps[hwid], 0, sizeof(struct cap_asnd));

	return 0;
}

#endif /* HW_H */
