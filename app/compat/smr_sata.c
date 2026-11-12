// SPDX-License-Identifier: GPL-2.0-only
// One idle libata port, selected from live sysfs. No PCI vendor or kernel pin.
//
// D6 evidence gating for the HC620-class "4Kn not enumerated" ports:
//
//  * Before every hardreset/softreset the D2H Register FIS area of the AHCI
//    RX FIS buffer (pp->rx_fis + 0x40, 20 bytes) is snapshotted while the
//    port lock is held.  The snapshot is taken again for every reset; it is
//    never reused from an older reset or from boot.
//  * After the reset the same 20 bytes must (a) differ from that snapshot and
//    (b) match the strict ZAC signature (FIS type 0x34, count=1, lbal=1,
//    lbam=0xCD, lbah=0xAB, plus the status/error bytes already required by
//    0.2.0).  Only then is it "fresh evidence", and only then may the
//    classification be forced or the SStatus software window be opened.
//  * observe_only is passive: it never writes RX FIS, never resets and never
//    sets `evidence`.  It only reports `observe_worth_trying`, which decides
//    whether a bounded reset attempt is worth trying.  That value is not
//    classification evidence and must not be used as such by userspace.
//  * PORT with an already enabled device is rejected under the port lock.
//  * IDENTIFY is the only success path: `identified` is only set in
//    ata_dev_configure after a successful IDENTIFY on ZAC-classified device.
//
// D8 kernel compatibility: version dependent libata APIs are selected at
// compile time.  Kernels we cannot support are rejected with #error instead
// of being silently degraded.

#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/libata.h>
#include <linux/ata.h>
#include <linux/pci.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include <linux/spinlock.h>
#include <scsi/scsi_host.h>

/*
 * Minimum kernel: 6.12 introduced enum ata_quirks / ATA_QUIRK_NONCQ in
 * include/linux/libata.h (6.11 and older only had the private ATA_HORKAGE_*
 * flags, which an out-of-tree module cannot use without copying private
 * headers).  Do not silently degrade.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
#error "smr_sata: unsupported kernel (< 6.12): ATA_QUIRK_NONCQ is not available"
#endif

/*
 * 6.17 introduced struct ata_reset_operations and moved the reset callbacks
 * into ops->reset.{softreset,hardreset}.  6.12..6.16 still have them as
 * direct members of struct ata_port_operations.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define SMR_OPS_HARDRESET(ops)		((ops)->reset.hardreset)
#define SMR_OPS_SOFTRESET(ops)		((ops)->reset.softreset)
#define SMR_OPS_SET_HARDRESET(ops, fn)	((ops)->reset.hardreset = (fn))
#else
#define SMR_OPS_HARDRESET(ops)		((ops)->hardreset)
#define SMR_OPS_SOFTRESET(ops)		((ops)->softreset)
#define SMR_OPS_SET_HARDRESET(ops, fn)	((ops)->hardreset = (fn))
#endif

/* AHCI RX FIS layout: offset 0x40 is the D2H Register FIS (libahci RX_FIS_D2H_REG). */
#define SMR_D2H_REG_OFFSET	0x40
#define SMR_D2H_LEN		20
#define SMR_RX_FIS_MAX_OFFSET	4096
#define SMR_EVIDENCE_WINDOW	(3 * HZ)
#define SMR_RUN_WINDOW		(90 * HZ)

static char *controller;
static unsigned int host_no, target_port, rx_fis_offset;
static bool ahci_fis, observe_only, normal_retry;
static bool expected_proto_ok;
static bool evidence;			/* set only by verified fresh evidence */
static bool observe_worth_trying;	/* passive verdict, never classification evidence */

module_param(expected_proto_ok, bool, 0400);
MODULE_PARM_DESC(expected_proto_ok,
	"Userspace sets 1 only after kprobe prototypes were validated (D8/T2); 0 refuses to load");
module_param(controller, charp, 0400);
MODULE_PARM_DESC(controller, "PCI BDF of the controller, from live sysfs");
module_param(host_no, uint, 0400);
MODULE_PARM_DESC(host_no, "scsi host number of the port to adapt");
module_param(target_port, uint, 0400);
MODULE_PARM_DESC(target_port, "zero based port number");
module_param(rx_fis_offset, uint, 0400);
MODULE_PARM_DESC(rx_fis_offset, "BTF offset of ahci_port_priv.rx_fis in the running kernel");
module_param(ahci_fis, bool, 0400);
MODULE_PARM_DESC(ahci_fis, "1 when RX FIS evidence is available through ahci_port_priv");
module_param(observe_only, bool, 0400);
MODULE_PARM_DESC(observe_only, "Passive observation only: no reset, no forcing, no evidence");
module_param(normal_retry, bool, 0400);
MODULE_PARM_DESC(normal_retry, "Bounded standard retries: never force the device class");
module_param(evidence, bool, 0444);
MODULE_PARM_DESC(evidence, "Read only: fresh ZAC evidence was verified after a reset");
module_param(observe_worth_trying, bool, 0444);
MODULE_PARM_DESC(observe_worth_trying,
	"Read only: passive observation says a bounded attempt is worth trying; not classification evidence");

static struct Scsi_Host *sh;
static struct ata_port *selected;
static struct ata_port_operations *original, shadow;
static unsigned long expiry, evidence_until;
static struct ata_link *pending_link;
static unsigned long pending_until;
static atomic_t attempts = ATOMIC_INIT(0);
static bool identified;

/* Link belongs to the port we were pointed at.  No time check. */
static bool selected_link(struct ata_link *l)
{
	return l && selected && l->ap == selected && l == &selected->link;
}

/* Selected link inside this module's bounded run window. */
static bool ours(struct ata_link *l)
{
	return selected_link(l) && time_before(jiffies, expiry);
}

/*
 * RX FIS buffer of an AHCI port, resolved through BTF.  The offset is
 * validated by userspace (T2 prototype check) and bounded again here; rx_fis
 * is never at offset 0 inside struct ahci_port_priv.
 */
static u8 *smr_rx_fis(struct ata_port *ap)
{
	u8 *rx;

	if (!ahci_fis || !ap || !ap->private_data || !rx_fis_offset ||
	    rx_fis_offset > SMR_RX_FIS_MAX_OFFSET)
		return NULL;
	rx = *(u8 **)((char *)ap->private_data + rx_fis_offset);
	return rx;
}

/* Caller must hold ap->lock so the copy cannot race a HBA RX FIS update. */
static bool smr_d2h_copy_locked(struct ata_port *ap, u8 out[SMR_D2H_LEN])
{
	u8 *rx = smr_rx_fis(ap);

	if (!rx)
		return false;
	dma_rmb();
	memcpy(out, rx + SMR_D2H_REG_OFFSET, SMR_D2H_LEN);
	return true;
}

/*
 * Take the D2H copy under the port lock.  libata calls the reset callbacks
 * without ap->lock (ata_eh_reset has already frozen/unlocked the port), so
 * the trylock normally succeeds.  If a kernel ever invokes reset with the
 * port lock held we fail closed (no snapshot -> no fresh evidence) instead of
 * deadlocking the EH thread.
 */
static bool smr_d2h_snapshot(struct ata_port *ap, u8 out[SMR_D2H_LEN])
{
	unsigned long flags;
	bool got;

	if (!ahci_fis || !ap || !ap->lock)
		return false;
	if (!spin_trylock_irqsave(ap->lock, flags))
		return false;
	got = smr_d2h_copy_locked(ap, out);
	spin_unlock_irqrestore(ap->lock, flags);
	return got;
}

/* Existing strict 0.2.0 signature, deliberately not relaxed (D6 item 3). */
static bool smr_strict_zac_sig(const u8 b[SMR_D2H_LEN])
{
	return b[0] == 0x34 && b[2] == 0x50 && b[3] == 1 && b[4] == 1 &&
	       b[5] == 0xcd && b[6] == 0xab && b[12] == 1;
}

/* Taskfile variant: same LBA/count pattern, without an FIS type byte. */
static bool smr_tf_zac_sig(const struct ata_taskfile *tf)
{
	return tf && tf->nsect == 1 && tf->lbal == 1 &&
	       tf->lbam == 0xcd && tf->lbah == 0xab;
}

struct reset_data {
	struct ata_link *link;
	unsigned int *cls;
	bool *online;
	bool have_snapshot;
	u8 snapshot[SMR_D2H_LEN];
};

/*
 * kretprobe entry: runs immediately before the reset callback body.  The
 * pre-reset D2H snapshot is taken here, under the port lock, for every reset.
 * The snapshot is re-taken for each reset; an older snapshot is never reused.
 */
static int reset_enter(struct kretprobe_instance *ri, struct pt_regs *r)
{
	struct ata_link *l = (void *)regs_get_kernel_argument(r, 0);
	struct reset_data *d = (void *)ri->data;

	if (!ours(l))
		return 1;
	pending_link = l;
	pending_until = 0;
	d->link = l;
	d->cls = (void *)regs_get_kernel_argument(r, 1);
	d->online = (void *)regs_get_kernel_argument(r, 3);
	d->have_snapshot = false;
	if (ahci_fis) {
		d->have_snapshot = smr_d2h_snapshot(l->ap, d->snapshot);
		if (d->have_snapshot)
			pr_info("smr_sata: %s port%u pre-reset D2H snapshot=%*ph\n",
				controller, target_port, SMR_D2H_LEN, d->snapshot);
		else
			pr_warn("smr_sata: %s port%u pre-reset D2H snapshot unavailable (RX FIS or port lock); reset cannot produce fresh evidence\n",
				controller, target_port);
	}
	return 0;
}

static int soft_enter(struct kretprobe_instance *ri, struct pt_regs *r)
{
	int e = reset_enter(ri, r);

	if (!e)
		((struct reset_data *)ri->data)->online = NULL;
	return e;
}

/*
 * Freshness test after the reset callback returned: current D2H must differ
 * from the pre-reset snapshot and must carry the strict ZAC signature.
 * Unchanged content is stale (issue 3: do not read an uncleared RX FIS).
 */
static bool reset_fresh_evidence(struct reset_data *d)
{
	struct ata_port *ap = d->link->ap;
	u8 now[SMR_D2H_LEN];

	if (!ahci_fis || !d->have_snapshot)
		return false;
	if (!smr_d2h_snapshot(ap, now)) {
		pr_warn("smr_sata: %s port%u post-reset D2H unavailable (RX FIS or port lock); no fresh evidence\n",
			controller, target_port);
		return false;
	}
	pr_info("smr_sata: %s port%u post-reset D2H=%*ph\n",
		controller, target_port, SMR_D2H_LEN, now);
	if (!memcmp(now, d->snapshot, SMR_D2H_LEN)) {
		pr_info("smr_sata: %s port%u D2H unchanged across reset; stale, no fresh evidence\n",
			controller, target_port);
		return false;
	}
	if (!smr_strict_zac_sig(now)) {
		pr_info("smr_sata: %s port%u D2H changed but no strict ZAC signature\n",
			controller, target_port);
		return false;
	}
	return true;
}

static int reset_leave(struct kretprobe_instance *ri, struct pt_regs *r)
{
	struct reset_data *d = (void *)ri->data;
	bool same_reset = pending_link == d->link && pending_until &&
			  time_before(jiffies, pending_until);

	pending_link = NULL;
	pending_until = 0;

	/*
	 * live taskfile 签名来自 PxSIG（ahci_dev_classify 读 PORT_SIG）。新鲜性前提：
	 * COMRESET 会把 PxSIG 重置为 ffffffff，因此本次 reset 之后从 PxSIG 派生的
	 * ZAC 签名属于本次复位，而不是上一轮的旧值。
	 */
	if (!ours(d->link) || !d->cls)
		return 0;
	/* Never overwrite a class that is already known to be a real device. */
	if (!(*d->cls == ATA_DEV_UNKNOWN || *d->cls == ATA_DEV_NONE ||
	      *d->cls == ATA_DEV_ZAC))
		return 0;
	bool fresh = reset_fresh_evidence(d);

	if (!fresh && same_reset) {
		pr_info("smr_sata: %s port%u same-reset live taskfile signature; using as fresh evidence\n",
			controller, target_port);
		fresh = true;
	}
	if (!fresh)
		return 0;

	evidence = true;
	evidence_until = jiffies + SMR_EVIDENCE_WINDOW;
	if (normal_retry || regs_return_value(r))
		return 0;
	*d->cls = ATA_DEV_ZAC;
	if (d->online)
		*d->online = true;
	pr_info("smr_sata: %s port%u fresh ZAC evidence; forcing one classification attempt; real IDENTIFY required\n",
		controller, target_port);
	return 0;
}

struct classify_data {
	struct ata_port *ap;
	struct ata_taskfile *tf;
};

static int classify_enter(struct kretprobe_instance *ri, struct pt_regs *r)
{
	struct ata_port *ap = (void *)regs_get_kernel_argument(r, 0);
	struct classify_data *d = (void *)ri->data;

	if (ap != selected || time_after(jiffies, expiry))
		return 1;
	d->ap = ap;
	d->tf = (void *)regs_get_kernel_argument(r, 1);
	return 0;
}

static int classify_leave(struct kretprobe_instance *ri, struct pt_regs *r)
{
	struct classify_data *d = (void *)ri->data;

	if (!smr_tf_zac_sig(d->tf))
		return 0;
	pr_info("smr_sata: %s port%u ZAC signature in taskfile\n",
		controller, target_port);
	if (pending_link && pending_link == &d->ap->link &&
	    time_before(jiffies, expiry)) {
		pending_until = jiffies + 3 * HZ;
		evidence = true;
		evidence_until = jiffies + SMR_EVIDENCE_WINDOW;
		pr_info("smr_sata: %s port%u live taskfile ZAC signature during reset; accepted as fresh evidence\n",
			controller, target_port);
		return 0;
	}
	/*
	 * The SStatus software window exists only to let a real IDENTIFY run
	 * after fresh RX FIS evidence.  A taskfile signature alone must not
	 * open it, otherwise classification could be forced without fresh
	 * evidence (D6 item 2 / issue 3).
	 */
	if (!evidence) {
		pr_info("smr_sata: %s port%u taskfile signature without fresh RX FIS evidence; SStatus window stays closed\n",
			controller, target_port);
		return 0;
	}
	evidence_until = jiffies + SMR_EVIDENCE_WINDOW;
	return 0;
}

struct scr_data {
	u32 *value;
};

static int scr_enter(struct kretprobe_instance *ri, struct pt_regs *r)
{
	struct ata_link *l = (void *)regs_get_kernel_argument(r, 0);
	struct scr_data *d = (void *)ri->data;

	if (!ours(l) || regs_get_kernel_argument(r, 1) != SCR_STATUS)
		return 1;
	d->value = (void *)regs_get_kernel_argument(r, 2);
	return 0;
}

static int scr_leave(struct kretprobe_instance *ri, struct pt_regs *r)
{
	struct scr_data *d = (void *)ri->data;

	if (evidence && !regs_return_value(r) && evidence_until &&
	    time_before(jiffies, evidence_until) && d->value &&
	    (*d->value & 15) == 0)
		*d->value = 0x133;
	return 0;
}

/*
 * ata_dev_configure runs only after a successful IDENTIFY.  It is the only
 * place where `identified` is set and the only place where this module sets
 * NONCQ: on the one device that went through the fresh-evidence path.
 */
static int configure(struct kprobe *k, struct pt_regs *r)
{
	struct ata_device *d = (void *)regs_get_kernel_argument(r, 0);
	char serial[ATA_ID_SERNO_LEN + 1];

	if (!d || !selected_link(d->link) || !evidence ||
	    d->class != ATA_DEV_ZAC)
		return 0;
	ata_id_c_string(d->id, serial, ATA_ID_SERNO, sizeof(serial));
	if (!serial[0])
		return 0;
	d->quirks |= ATA_QUIRK_NONCQ;
	identified = true;
	pr_info("smr_sata: %s port%u identified ZAC serial=%s; only this device NONCQ\n",
		controller, target_port, serial);
	return 0;
}

static struct kretprobe hard = {
	.kp.symbol_name = "ahci_do_hardreset",
	.entry_handler = reset_enter,
	.handler = reset_leave,
	.data_size = sizeof(struct reset_data),
	.maxactive = 32,
};
static struct kretprobe soft = {
	.kp.symbol_name = "ahci_softreset",
	.entry_handler = soft_enter,
	.handler = reset_leave,
	.data_size = sizeof(struct reset_data),
	.maxactive = 32,
};
static struct kretprobe cls = {
	.kp.symbol_name = "ata_port_classify",
	.entry_handler = classify_enter,
	.handler = classify_leave,
	.data_size = sizeof(struct classify_data),
	.maxactive = 32,
};
static struct kretprobe scr = {
	.kp.symbol_name = "sata_scr_read",
	.entry_handler = scr_enter,
	.handler = scr_leave,
	.data_size = sizeof(struct scr_data),
	.maxactive = 32,
};
static struct kprobe cfg = {
	.symbol_name = "ata_dev_configure",
	.pre_handler = configure,
};

static bool hard_on, soft_on, cls_on, scr_on, cfg_on;

static void release(void)
{
	if (cfg_on)
		unregister_kprobe(&cfg);
	if (scr_on)
		unregister_kretprobe(&scr);
	if (cls_on)
		unregister_kretprobe(&cls);
	if (soft_on)
		unregister_kretprobe(&soft);
	if (hard_on)
		unregister_kretprobe(&hard);
	if (sh) {
		scsi_host_put(sh);
		sh = NULL;
	}
}

static int one_reset(struct ata_link *l, unsigned int *classes,
		     unsigned long deadline)
{
	unsigned long flags;
	int rc;

	if (!ours(l))
		return SMR_OPS_HARDRESET(original)(l, classes, deadline);
	spin_lock_irqsave(selected->lock, flags);
	if (selected->ops == &shadow)
		selected->ops = original;
	spin_unlock_irqrestore(selected->lock, flags);
	if (atomic_cmpxchg(&attempts, 0, 1))
		return SMR_OPS_HARDRESET(original)(l, classes, deadline);
	/* Diagnostic reset collects fresh evidence. No blind UNKNOWN->ZAC fallback. */
	rc = SMR_OPS_SOFTRESET(original)(l, classes, jiffies + 5 * HZ);
	if (!rc && *classes != ATA_DEV_NONE && *classes != ATA_DEV_UNKNOWN)
		return 0;
	return SMR_OPS_HARDRESET(original)(l, classes, jiffies + 10 * HZ);
}

/*
 * Passive observation (observe_only): read only, no reset, no SStatus writes,
 * no `evidence`.  Returns "is a bounded attempt worth trying".
 */
static bool observe_worth_trying_on(struct ata_port *ap)
{
	struct ata_taskfile tf = {};
	u32 sstatus = 0;
	bool worth = false;

	if (ahci_fis) {
		u8 b[SMR_D2H_LEN];

		if (smr_d2h_snapshot(ap, b)) {
			pr_info("smr_sata: %s port%u passive D2H=%*ph strict_sig=%d\n",
				controller, target_port, SMR_D2H_LEN, b,
				smr_strict_zac_sig(b));
			/*
			 * Any D2H Register FIS proves the device answered at some
			 * point.  It is weak evidence for "worth trying", never
			 * classification evidence: it may be stale by definition.
			 */
			worth = smr_strict_zac_sig(b) || b[0] == 0x34;
		}
	}
#ifdef CONFIG_ATA_SFF
	if (!ahci_fis && ap->ops->sff_tf_read) {
		ap->ops->sff_tf_read(ap, &tf);
		pr_info("smr_sata: %s port%u passive tf status=%02x error=%02x nsect=%u lbam=%02x\n",
			controller, target_port, tf.status, tf.error,
			tf.nsect, tf.lbam);
		worth = smr_tf_zac_sig(&tf);
	}
#endif
	if (ap->ops->scr_read &&
	    !sata_scr_read(&ap->link, SCR_STATUS, &sstatus)) {
		pr_info("smr_sata: %s port%u passive SStatus=%08x det=%u spd=%u\n",
			controller, target_port, sstatus,
			sstatus & 0xf, (sstatus >> 4) & 0xf);
		/* D6 strong evidence: real link speed.  Never classification evidence. */
		if ((sstatus & 0xf) == 3 && ((sstatus >> 4) & 0xf))
			worth = true;
	}
	return worth;
}

static int __init start(void)
{
	unsigned long flags;
	struct pci_dev *p;
	int rc, i;

	if (!expected_proto_ok) {
		pr_err("smr_sata: expected_proto_ok=0; refusing to load. Userspace must validate the kprobe prototypes (D8/T2) and pass expected_proto_ok=1\n");
		return -EINVAL;
	}
	if (!controller)
		return -EINVAL;
	sh = scsi_host_lookup(host_no);
	if (!sh)
		return -ENODEV;
	selected = ata_shost_to_port(sh);
	if (!selected || selected->scsi_host != sh ||
	    selected->port_no != target_port || !selected->host ||
	    !dev_is_pci(selected->host->dev)) {
		rc = -EINVAL;
		goto fail;
	}
	p = to_pci_dev(selected->host->dev);
	if (strcmp(pci_name(p), controller)) {
		rc = -ENODEV;
		goto fail;
	}
	if (ahci_fis && (!p->dev.driver || strcmp(p->dev.driver->name, "ahci"))) {
		rc = -EINVAL;
		goto fail;
	}
	spin_lock_irqsave(selected->lock, flags);
	rc = -EBUSY;
	if (selected->qc_active ||
	    (selected->pflags & (ATA_PFLAG_EH_IN_PROGRESS | ATA_PFLAG_EH_PENDING)))
		goto unlock;
	/* Any already enabled device: refuse under the port lock (D6 item 6). */
	for (i = 0; i < ATA_MAX_DEVICES; i++)
		if (ata_dev_enabled(&selected->link.device[i]))
			goto unlock;
	original = selected->ops;
	rc = -EOPNOTSUPP;
	if (!SMR_OPS_HARDRESET(original) || !SMR_OPS_SOFTRESET(original))
		goto unlock;
	rc = 0;
unlock:
	spin_unlock_irqrestore(selected->lock, flags);
	if (rc)
		goto fail;

	if (observe_only) {
		observe_worth_trying = observe_worth_trying_on(selected);
		pr_info("smr_sata: %s port%u passive observe worth_trying=%d; no reset, evidence=%d\n",
			controller, target_port, observe_worth_trying, evidence);
		return 0;
	}

	expiry = jiffies + SMR_RUN_WINDOW;
	rc = register_kretprobe(&cls);
	if (rc)
		goto fail;
	cls_on = true;
	if (!normal_retry) {
		rc = register_kprobe(&cfg);
		if (rc)
			goto fail;
		cfg_on = true;
	}
	if (ahci_fis) {
		rc = register_kretprobe(&hard);
		if (rc)
			goto fail;
		hard_on = true;
		rc = register_kretprobe(&soft);
		if (rc)
			goto fail;
		soft_on = true;
		if (!normal_retry) {
			rc = register_kretprobe(&scr);
			if (rc)
				goto fail;
			scr_on = true;
		}
	}

	spin_lock_irqsave(selected->lock, flags);
	/* Recheck under the same lock after installing observers. */
	rc = -EBUSY;
	if (selected->qc_active ||
	    (selected->pflags & (ATA_PFLAG_EH_IN_PROGRESS | ATA_PFLAG_EH_PENDING)))
		goto unlock2;
	for (i = 0; i < ATA_MAX_DEVICES; i++)
		if (ata_dev_enabled(&selected->link.device[i]))
			goto unlock2;
	shadow = *original;
	SMR_OPS_SET_HARDRESET(&shadow, one_reset);
	selected->ops = &shadow;
	selected->link.eh_info.probe_mask = ATA_ALL_DEVICES;
	selected->link.eh_info.action |= ATA_EH_RESET | ATA_EH_ENABLE_LINK;
	selected->link.eh_info.flags |= ATA_EHI_NO_AUTOPSY;
	ata_port_schedule_eh(selected);
	rc = 0;
unlock2:
	spin_unlock_irqrestore(selected->lock, flags);
	if (rc)
		goto fail;
	ata_port_wait_eh(selected);
	spin_lock_irqsave(selected->lock, flags);
	if (selected->ops == &shadow)
		selected->ops = original;
	spin_unlock_irqrestore(selected->lock, flags);
	ata_port_wait_eh(selected);
	pr_info("smr_sata: %s port%u finished attempts=%d evidence=%d identified=%d; original ops restored\n",
		controller, target_port, atomic_read(&attempts), evidence,
		identified);
	return 0;
fail:
	release();
	return rc;
}

static void __exit stop(void)
{
	release();
}

module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Bounded evidence-gated libata port adaptation");
