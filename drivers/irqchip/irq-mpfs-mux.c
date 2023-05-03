// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) GPIO IRQ MUX
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 */

#define pr_fmt(fmt) "mpfs-irq-mux: " fmt

#include <linux/bits.h>
#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define MPFS_MUX_NUM_IRQS 41
#define MPFS_MUX_NUM_DIRECT_IRQS 38
#define MPFS_IRQS_PER_GPIO 32

/*
 * There are 3 GPIO controllers on this SoC, each of which has 32 GPIOs.
 * All GPIOs are capable of generating interrupts, for a total of 96.
 * There are only 41 IRQs available however, so a configurable mux is used to
 * ensure all GPIOs can be used for interrupt generation.
 * 38 of the 41 interrupts are in what the documentation calls "direct mode",
 * as they provide an exclusive connection from a GPIO to the PLIC.
 * The 3 remaining interrupts are used to mux the interrupts which do not have
 * a exclusive connection, one for each GPIO controller.
 * A register is used to set this configuration of this mux, depending on how
 * the "MSS Configurator" (FPGA configuration tool) has set things up.
 * This is done by the platforms firmware, so access from Linux is read-only.
 *
 * Documentation also refers to GPIO controller 0 & 1 as "pad" GPIO controllers
 * and GPIO controller 2 as the "fabric" GPIO controller. Despite that wording,
 * all 3 are "hard" peripherals. If a bit is set in the mux's control register,
 * the "pad" controller's interrupt will be put in "non-direct" mode. If
 * cleared, the "fabric" controller's.
 *
 * Bits 0 to 13 mux between GPIO controller 1's first 14 GPIOs and GPIO
 * controller 2's first 14. The remaining bits mux between the first 18 GPIOs
 * of controller 1 and the last 18 GPIOS of controller 2.
 *
 * That leaves 6 exclusive, or "direct", interrupts remaining. These are,
 * according to documentation, used by GPIO controller 1's lines 19 to 24.
 */
static const unsigned long mpfs_irq_mux_masks[3] = {
	GENMASK_ULL(13, 0),
	GENMASK_ULL(37, 14),
	GENMASK_ULL(31, 0),
};

struct mpfs_irq_mux_irqchip {
	struct irq_domain *domain;
	int i;
	int irq;
	unsigned long reg_mask;
	u8 offset;
};

struct mpfs_irq_mux {
	void __iomem *reg;
	u32 mux_config;
	struct mpfs_irq_mux_irqchip irqchip_data[MPFS_MUX_NUM_IRQS];
};

static struct irq_chip mpfs_irq_mux_irq_chip = {
	.name = "MPFS GPIO Interrupt Mux",
};

static inline unsigned long mpfs_irq_mux_get_muxxed_irqs(struct mpfs_irq_mux *priv,
							 unsigned long mask)
{
	unsigned long mux_config = priv->mux_config;
	unsigned long muxxed_irqs;

	/*
	 * Assume that all interrupts outside of the mask are in "direct" mode
	 * to begin with. Anything not in the mask cannot be set to
	 * "direct" mode, so it is a useful analogue.
	 *
	 * Handily, this will account for the 6 extra "direct" interrupts that
	 * are connected to GPIO controller 1, which will be cleared by the
	 * inverted mask.
	 */
	muxxed_irqs = ~mask;

	/*
	 * If a bit is set in the mux, GPIO controller 2 is direct and
	 * controllers 0 & 1 are muxxed.
	 * Invert the bits in the configuration register, so that set bits
	 * equate to non-direct mode, for GPIO controller 2.
	 */
	if (mask == mpfs_irq_mux_masks[2])
		mux_config = ~mux_config;

	mux_config &= mask;
	muxxed_irqs |= mux_config;

	/*
	 * For GPIO controller 1, the first interrupt is not aligned with the
	 * start of register, rather with the start of the mask.
	 */
	muxxed_irqs >>= ffs(mask) - 1;

	return muxxed_irqs;
}

static void mpfs_irq_mux_nondirect_handler(struct irq_desc *desc)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = irq_desc_get_handler_data(desc);
	int i = irqchip_data->i;
	struct mpfs_irq_mux *priv = container_of(irqchip_data, struct mpfs_irq_mux,
						irqchip_data[i]);
	unsigned long muxxed_irqs;
	int pos;

	chained_irq_enter(irq_desc_get_chip(desc), desc);

	muxxed_irqs = mpfs_irq_mux_get_muxxed_irqs(priv, priv->irqchip_data[i].reg_mask);

	for_each_set_bit(pos, &muxxed_irqs, MPFS_IRQS_PER_GPIO)
		generic_handle_domain_irq(irqchip_data->domain, pos);

	chained_irq_exit(irq_desc_get_chip(desc), desc);
}

static int mpfs_irq_mux_nondirect_select(struct irq_domain *d, struct irq_fwspec *fwspec,
				 enum irq_domain_bus_token bus_token)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = d->host_data;
	bool is_it_us = false;
	int i = irqchip_data->i;
	struct mpfs_irq_mux *priv = container_of(irqchip_data, struct mpfs_irq_mux,
						irqchip_data[i]);

	if (fwspec->fwnode != d->fwnode) {
		return false;
	}

	u32 start = (irqchip_data->i - MPFS_MUX_NUM_DIRECT_IRQS) * MPFS_IRQS_PER_GPIO;
	u32 end = start + MPFS_IRQS_PER_GPIO;

	if (fwspec->param[0] < end && fwspec->param[0] >= start) {
		u32 muxxed_irqs = mpfs_irq_mux_get_muxxed_irqs(priv,
							       priv->irqchip_data[i].reg_mask);

		if(muxxed_irqs & BIT(fwspec->param[0] - start))
			is_it_us = true;
	}

	return is_it_us;
}

static int mpfs_irq_mux_nondirect_translate(struct irq_domain *d, struct irq_fwspec *fwspec,
				      unsigned long *out_hwirq, unsigned int *out_type)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = d->host_data;
	if (!is_of_node(fwspec->fwnode))
		return -EINVAL;

	*out_hwirq = fwspec->param[0] - irqchip_data->offset;
	*out_type = IRQ_TYPE_LEVEL_HIGH;

	pr_info("translated %u to %lu", fwspec->param[0], *out_hwirq);

	return 0;
}

static int mpfs_irq_mux_nondirect_alloc(struct irq_domain *d, unsigned int virq,
			          unsigned int nr_irqs, void *arg)
{
	int i, ret;
	irq_hw_number_t hwirq = 0;
	unsigned int type;
	struct irq_fwspec *fwspec = arg;

	ret = mpfs_irq_mux_nondirect_translate(d, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(d, virq + i, hwirq + i, &mpfs_irq_mux_irq_chip,
				    d->host_data, handle_level_irq, NULL, NULL);
		pr_info("mapped %lu as %u\n", hwirq + i, virq + i);
	}

	return 0;
}

static const struct irq_domain_ops mpfs_irq_mux_nondirect_domain_ops = {
	.select		= mpfs_irq_mux_nondirect_select,
	.translate	= mpfs_irq_mux_nondirect_translate,
	.alloc		= mpfs_irq_mux_nondirect_alloc,
	.free		= irq_domain_free_irqs_top,
};

static void mpfs_irq_mux_direct_handler(struct irq_desc *desc)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = irq_desc_get_handler_data(desc);

	chained_irq_enter(irq_desc_get_chip(desc), desc);

	generic_handle_irq(irq_find_mapping(irqchip_data->domain, 0)); //TODO 0 -> ?

	chained_irq_exit(irq_desc_get_chip(desc), desc);
}

static int mpfs_irq_mux_direct_select(struct irq_domain *d, struct irq_fwspec *fwspec,
				 enum irq_domain_bus_token bus_token)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = d->host_data;
	bool is_it_us = false;
	int i = irqchip_data->i;
	struct mpfs_irq_mux *priv = container_of(irqchip_data, struct mpfs_irq_mux,
						irqchip_data[i]);

	if (fwspec->fwnode != d->fwnode) {
		return false;
	}

	u32 bank = fwspec->param[0] / 32; //warning, 64-bit division?
	u32 muxxed_irqs = mpfs_irq_mux_get_muxxed_irqs(priv, mpfs_irq_mux_masks[bank]);
	if (BIT(fwspec->param[0] % 32) & ~muxxed_irqs)
		is_it_us = true;

	return is_it_us;
}

static int mpfs_irq_mux_direct_translate(struct irq_domain *d, struct irq_fwspec *fwspec,
				      unsigned long *out_hwirq, unsigned int *out_type)
{
	if (!is_of_node(fwspec->fwnode))
		return -EINVAL;

	*out_hwirq = 0;
	*out_type = IRQ_TYPE_LEVEL_HIGH;

	pr_info("xlated %u to %lu", fwspec->param[0], *out_hwirq);

	return 0;
}

static int mpfs_irq_mux_direct_map(struct irq_domain *h, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = h->host_data;

	irq_set_chip_data(irq, irqchip_data);
	irq_set_chip_and_handler(irq, &mpfs_irq_mux_irq_chip, handle_level_irq);

	pr_info("mapped %lu (direct) as %u\n", hwirq, irq);

	return 0;
}

static int mpfs_irq_mux_direct_alloc(struct irq_domain *d, unsigned int virq,
			          unsigned int nr_irqs, void *arg)
{
	int ret;
	irq_hw_number_t hwirq;
	unsigned int type;
	struct irq_fwspec *fwspec = arg;
	struct mpfs_irq_mux_irqchip *irqchip_data = d->host_data;

	ret = mpfs_irq_mux_direct_translate(d, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	irq_set_chip_data(virq, irqchip_data);
	irq_set_chip_and_handler(virq, &mpfs_irq_mux_irq_chip, handle_level_irq);

	pr_info("mapped %lu as %u\n", hwirq, virq);

	return 0;
}

static const struct irq_domain_ops mpfs_irq_mux_direct_domain_ops = {
	.select		= mpfs_irq_mux_direct_select,
	.translate	= mpfs_irq_mux_direct_translate,
	.alloc		= mpfs_irq_mux_direct_alloc,
	.free		= irq_domain_free_irqs_top,
};

static int __init mpfs_irq_mux_init(struct device_node *node, struct device_node *parent)
{
	struct mpfs_irq_mux *priv;
	struct irq_domain *domain;
	int i;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reg = of_iomap(node, 0);
	if (!priv->reg)
		return -ENODEV; //TODO: add teardown code

	priv->mux_config = readl(priv->reg);

	for (i = 0; i < (MPFS_MUX_NUM_DIRECT_IRQS); i++) {
		;
//		priv->irqchip_data[i].i = i;
//
//		priv->irqchip_data[i].irq = irq_of_parse_and_map(node, i);
//		if (priv->irqchip_data[i].irq <= 0)
//			return -1; //TODO: add teardown code
//
//		domain = irq_domain_add_linear(node, 32,
//					       &mpfs_irq_mux_direct_domain_ops,
//					       &priv->irqchip_data[i]);
//		if (!domain)
//			return -ENOMEM; //TODO: add teardown code
//
//		priv->irqchip_data[i].domain = domain;
//		priv->irqchip_data[i].reg_mask = GENMASK_ULL(37, 0);
//		priv->irqchip_data[i].offset = 0;
//
//		irq_set_chained_handler_and_data(priv->irqchip_data[i].irq,
//						 mpfs_irq_mux_direct_handler,
//						 &priv->irqchip_data[i]);
//
//		pr_info("registered domain %d:%u\n", i, priv->irqchip_data[i].irq);
	}

	/*
	 * Now the non-direct/muxxed domains can be set up
	 */
	for (; i < MPFS_MUX_NUM_IRQS; i++) {
		priv->irqchip_data[i].i = i;

		priv->irqchip_data[i].irq = irq_of_parse_and_map(node, i);
		if (priv->irqchip_data[i].irq <= 0)
			return -1; //TODO: add teardown code

		domain = irq_domain_add_linear(node, MPFS_IRQS_PER_GPIO,
					       &mpfs_irq_mux_nondirect_domain_ops,
					       &priv->irqchip_data[i]);
		if (!domain)
			return -ENOMEM; //TODO: add teardown code

		priv->irqchip_data[i].domain = domain;
		priv->irqchip_data[i].reg_mask = mpfs_irq_mux_masks[i - MPFS_MUX_NUM_DIRECT_IRQS];
		priv->irqchip_data[i].offset = (i - MPFS_MUX_NUM_DIRECT_IRQS) * MPFS_IRQS_PER_GPIO;

		irq_set_chained_handler_and_data(priv->irqchip_data[i].irq,
						 mpfs_irq_mux_nondirect_handler,
						 &priv->irqchip_data[i]);

		pr_info("registered domain %d:%u\n", i, priv->irqchip_data[i].irq);
	}

	pr_info("registered with mux config %x\n", priv->mux_config);
	return 0;
}

IRQCHIP_DECLARE(mpfs_irq_mux, "microchip,mpfs-gpio-irq-mux", mpfs_irq_mux_init);
