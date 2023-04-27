// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) GPIO IRQ MUX
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 *
 * MUX Configuration:
 * - there are 3 gpio controllers on this SoC, each with 32 gpios
 * - there are significantly fewer than 96 interrupts allocated for gpio
 * - there are 3 muxed, or "non-direct" interrupts
 * - 14 are either gpio 0 or gpio 2's "direct" connections to the plic
 * - 24 are either gpio 1 or gpio 2's <ditto>
 * - the GPIO_INTERRUPT_FAB_CR register determines which of gpio m or 2 is wired
 *   directly, and which is non-direct
 * - the register is 32-bit but the mux has 38 lines. God only knows how those
 *   are meant to be configured. I claim that they are all gpio 1.
 * - anything not wired directly is muxed into the corresponding non-direct
 *   interrupt
 */

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
#define MPFS_MUX_GPIO0_2_MASK GENMASK(13, 0)
#define MPFS_MUX_GPIO1_2_MASK GENMASK(31, 14)

static const u32 mpfs_irq_mux_masks[3] = {
	MPFS_MUX_GPIO0_2_MASK,
	MPFS_MUX_GPIO1_2_MASK,
	GENMASK(31, 0),
};

struct mpfs_irq_mux_irqchip {
	struct irq_domain *domain;
	int i;
	int irq;
	u32 reg_mask;
	u8 offset;
};

struct mpfs_irq_mux {
	void __iomem *reg;
	u32 mux_config;
	struct mpfs_irq_mux_irqchip irqchip_data[MPFS_MUX_NUM_IRQS];
};

static struct irq_chip mpfs_irq_mux_irq_chip = {
	.name = "MPFS irq mux",
};

static inline unsigned long mpfs_irq_mux_get_muxxed_irqs(struct mpfs_irq_mux *priv, int i)
{
	u32 mux_config = priv->mux_config;

	/*
	 * Perhaps a hack, but we know that only gpio2 has all the bits set in
	 * its mask & from the register description:
	 * Setting these bits will disable the Pad interrupt, and enable
	 * the fabric GPIO interrupt for bits 31:0. When the bit is set the Pad
	 * interrupt will be ORED into the GPIO0 & GPIO1 non-direct interrupts.
	 * When the bit is not set the Fabric interrupt is ORED into the GPIO2
	 * non-direct interrupt. To prevent ORING then the interrupt should not
	 * be enabled in the GPIO block
	 */
	if (priv->irqchip_data[i].reg_mask == GENMASK(31, 0)) //this is really sloppy, remove the hack
		mux_config = ~mux_config;

	return priv->irqchip_data[i].reg_mask & mux_config;
}

static void mpfs_irq_mux_irq_handler(struct irq_desc *desc)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = irq_desc_get_handler_data(desc);
	int i = irqchip_data->i; //TODO maybe a hack?
	struct mpfs_irq_mux *priv = container_of(irqchip_data, struct mpfs_irq_mux,
						irqchip_data[i]);
	unsigned long muxxed_irqs;
	int pos;

	chained_irq_enter(irq_desc_get_chip(desc), desc);

	muxxed_irqs = mpfs_irq_mux_get_muxxed_irqs(priv, i);

	for_each_set_bit(pos, &muxxed_irqs, 32)
		generic_handle_domain_irq(irqchip_data->domain, pos);

	chained_irq_exit(irq_desc_get_chip(desc), desc);
}

static int mpfs_irq_mux_irq_map(struct irq_domain *h, unsigned int irq,
			      irq_hw_number_t hwirq)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = h->host_data;

	irq_set_chip_data(irq, irqchip_data);
	irq_set_chip_and_handler(irq, &mpfs_irq_mux_irq_chip, handle_level_irq); //may be wrong (handle_level_irq)
	pr_info("mpfs-irq-mux: mapped %lu as %u\n", hwirq, irq);

	return 0;
}

static int mpfs_irq_mux_irq_select(struct irq_domain *d, struct irq_fwspec *fwspec,
				 enum irq_domain_bus_token bus_token)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = d->host_data;
	bool is_it_us = false;
	int i = irqchip_data->i; //TODO maybe a hack?
	struct mpfs_irq_mux *priv = container_of(irqchip_data, struct mpfs_irq_mux,
						irqchip_data[i]);

	if (fwspec->fwnode != d->fwnode) {
		return false;
	}


	// really this should be 38 + start (I think this comment assumes that
	// direct mode would mean adding more hwirqs)
	u32 start = (irqchip_data->i - 38) * 32;
	u32 end = start + 32;

	if (fwspec->param[0] < end && fwspec->param[0] >= start) {
		u32 muxxed_irqs = mpfs_irq_mux_get_muxxed_irqs(priv, i);

		if(muxxed_irqs & fwspec->param[0])
			is_it_us = true;
	}

	pr_info("mpfs-irq-mux: is it us? %u, %pa %u\n", is_it_us, d, irqchip_data->i);
	return is_it_us;
}

static int mpfs_irq_mux_irq_xlate(struct irq_domain *d, struct device_node *node,
			      const u32 *intspec, unsigned int intsize,
			      unsigned long *out_hwirq, unsigned int *out_type)
{
	struct mpfs_irq_mux_irqchip *irqchip_data = d->host_data;
	*out_hwirq = intspec[0] - irqchip_data->offset;
	*out_type = IRQ_TYPE_LEVEL_HIGH;

	pr_info("mpfs-irq-mux: translated %u to %lu", intspec[0], *out_hwirq);

	return 0;
};

static const struct irq_domain_ops mpfs_irq_mux_domain_ops = {
	.map		= mpfs_irq_mux_irq_map,
	.xlate		= mpfs_irq_mux_irq_xlate,
	.select		= mpfs_irq_mux_irq_select,
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

	for (i = 0; i < (MPFS_MUX_NUM_IRQS - 3); i++)
		;

	/*
	 * Now the non-direct/muxxed domains can be set up
	 */
	for (; i < MPFS_MUX_NUM_IRQS; i++) {
		priv->irqchip_data[i].i = i;

		priv->irqchip_data[i].irq = irq_of_parse_and_map(node, i);
		if (priv->irqchip_data[i].irq <= 0)
			return -1; //TODO: add teardown code

		domain = irq_domain_add_linear(node, 32, &mpfs_irq_mux_domain_ops,
					       &priv->irqchip_data[i]);
		if (!domain)
			return -ENOMEM; //TODO: add teardown code

		priv->irqchip_data[i].domain = domain;
		priv->irqchip_data[i].reg_mask = mpfs_irq_mux_masks[i - 38];
		priv->irqchip_data[i].offset = (i - 38) * 32;

		irq_set_chained_handler_and_data(priv->irqchip_data[i].irq,
						 mpfs_irq_mux_irq_handler,
						 &priv->irqchip_data[i]);

		pr_info("mpfs-irq-mux: registered domain %d:%u\n", i, priv->irqchip_data[i].irq);
	}

	pr_info("mpfs-irq-mux: registered with mux config %x\n", priv->mux_config);
	return 0;
}

IRQCHIP_DECLARE(mpfs_irq_mux, "microchip,mpfs-gpio-irq-mux", mpfs_irq_mux_init);
