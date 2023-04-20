// SPDX-License-Identifier: GPL-2.0
/*
 * Microchip PolarFire SoC (MPFS) GPIO IRQ MUX
 *
 * Author: Conor Dooley <conor.dooley@microchip.com>
 */

/*
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

#include <linux/interrupt.h>
#include <linux/irqdomain.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define MPFS_MUX_GPIO0_2_MASK GENMASK(0, 13)
#define MPFS_MUX_GPIO1_2_MASK GENMASK(14, 31)

struct mpfs_irq_mux_irqchip {
	u32 mux_config;
};

struct mpfs_irq_mux {
	void __iomem *reg;
};

static void imx_intmux_irq_handler(struct irq_desc *desc)
{
	struct intmux_irqchip_data *irqchip_data = irq_desc_get_handler_data(desc);
	int idx = irqchip_data->chanidx;
	struct intmux_data *data = container_of(irqchip_data, struct intmux_data,
						irqchip_data[idx]);
	unsigned long irqstat;
	int pos;

	chained_irq_enter(irq_desc_get_chip(desc), desc);
	/*
	 * What we want to do here is:
	 * check if this is > 50, in which case this could be any of the
	 * non-direct interrupts (or more than one of them). Trigger all of
	 * their handlers
	 *
	 * if < 50, only trigger the handlers for that specific one below us,
	 * based on what way the mux was configured.
	 */
	for_each_set_bit(pos, &irqstat, 32)
		generic_handle_domain_irq(irqchip_data->domain, pos);

	chained_irq_exit(irq_desc_get_chip(desc), desc);
}

static int mpfs_irq_mux_xlate(struct irq_domain *d, struct device_node *node,
			      const u32 *intspec, unsigned int intsize,
			      unsigned long *out_hwirq, unsigned int *out_type)
{
	*out_hwirq = 0; //TODO: uhh
	*out_type = IRQ_TYPE_LEVEL_HIGH; //TODO: check this
	return 0;
}
static const struct irq_domain_ops mpfs_irq_mux_domain_ops = {
	.map		= mpfs_irq_mux_irq_map,
	.xlate		= mpfs_irq_mux_irq_xlate,
	.select		= mpfs_irq_mux_irq_select,
};

static int mpfs_irq_mux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpfs_irq_mux *priv;
	struct irq_domain *domain;
	u32 mux_config;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->reg)) {
		dev_err(&pdev->dev, "failed to initialize reg\n");
		return PTR_ERR(priv->reg);
	}

	mux_config = readl(priv->reg);

	domain = irq_domain_add_linear(dev->of_node, 96, &mpfs_irq_mux_domain_ops, priv);
	return 0;
}

static void mpfs_irq_mux_remove(struct platform_device *pdev) { }

static const struct of_device_id mpfs_irq_mux_of_match[] = {
	{ .compatible = "microchip,mpfs-gpio-irq-mux", },
	{ },
};

static struct platform_driver mpfs_irq_mux_driver = {
	.driver = {
		.name = "mpfs-gpio-irq-mux",
		.of_match_table = mpfs_irq_mux_of_match,
	},
	.probe = mpfs_irq_mux_probe,
	.remove_new = mpfs_irq_mux_remove,
};
builtin_platform_driver(mpfs_irq_mux_driver);
