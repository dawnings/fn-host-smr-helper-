// SPDX-License-Identifier: GPL-2.0-only
// Driver-init policy. DMA masks are restricted before the driver allocates DMA,
// and controller-wide NCQ can be forced off before ata_dev_configure() enables it.
//
// T9: user-selected controller policies.  Only `dma32` and `ncq` are handled
// here; single-disk NONCQ for compat-identified ZAC disks is still userspace.
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/pci.h>
#include <linux/libata.h>
#include <linux/dma-mapping.h>

static char *ncq = "", *dma32 = "";
module_param(ncq, charp, 0400);
module_param(dma32, charp, 0400);
MODULE_PARM_DESC(ncq, "Comma separated PCI BDFs whose ATA devices must not use NCQ");
MODULE_PARM_DESC(dma32, "Comma separated PCI BDFs whose drivers get a 32 bit DMA mask request");

/* Match a PCI BDF against the comma separated parameter list (unchanged). */
static bool selected(const char *list, struct device *d)
{
	const char *n;
	size_t len;
	const char *p;

	if (!list || !*list || !d || !dev_is_pci(d))
		return false;
	n = pci_name(to_pci_dev(d));
	len = strlen(n);
	for (p = list; *p;) {
		const char *q = strchr(p, ',');
		size_t l = q ? (size_t)(q - p) : strlen(p);

		if (l == len && !strncmp(p, n, len))
			return true;
		if (!q)
			break;
		p = q + 1;
	}
	return false;
}

static int mask_hook(struct kprobe *k, struct pt_regs *r)
{
	struct device *d = (void *)regs_get_kernel_argument(r, 0);
	u64 mask = regs_get_kernel_argument(r, 1);

	if (!selected(dma32, d) || mask <= DMA_BIT_MASK(32))
		return 0;
#if defined(CONFIG_X86_64)
	r->si = DMA_BIT_MASK(32);
#elif defined(CONFIG_ARM64)
	r->regs[1] = DMA_BIT_MASK(32);
#endif
	pr_info("smr_policy: %s driver DMA request limited to 32 bits\n",
		dev_name(d));
	return 0;
}

/* Runs before ata_dev_configure(); ATA_QUIRK_NONCQ is consumed there. */
static int cfg_hook(struct kprobe *k, struct pt_regs *r)
{
	struct ata_device *d = (void *)regs_get_kernel_argument(r, 0);

	if (d && d->link && d->link->ap && d->link->ap->host &&
	    selected(ncq, d->link->ap->host->dev)) {
		d->quirks |= ATA_QUIRK_NONCQ;
		pr_info("smr_policy: %s port%u user requested NONCQ\n",
			dev_name(d->link->ap->host->dev), d->link->ap->port_no);
	}
	return 0;
}

static struct kprobe streaming = {
	.symbol_name = "dma_set_mask",
	.pre_handler = mask_hook,
};
static struct kprobe coherent = {
	.symbol_name = "dma_set_coherent_mask",
	.pre_handler = mask_hook,
};
static struct kprobe configure = {
	.symbol_name = "ata_dev_configure",
	.pre_handler = cfg_hook,
};
static bool a, b, c;

static void release(void)
{
	if (c)
		unregister_kprobe(&configure);
	if (b)
		unregister_kprobe(&coherent);
	if (a)
		unregister_kprobe(&streaming);
}

static int __init start(void)
{
	int rc;

#if !defined(CONFIG_X86_64) && !defined(CONFIG_ARM64)
	if (dma32 && *dma32)
		return -EOPNOTSUPP;
#endif
	if (dma32 && *dma32) {
		rc = register_kprobe(&streaming);
		if (rc)
			goto fail;
		a = true;
		rc = register_kprobe(&coherent);
		if (rc)
			goto fail;
		b = true;
	}
	if (ncq && *ncq) {
		rc = register_kprobe(&configure);
		if (rc)
			goto fail;
		c = true;
	}
	pr_info("smr_policy: controller policies registered; existing DMA allocations unchanged\n");
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
MODULE_DESCRIPTION("Explicit controller NCQ and DMA policies for driver initialization");
