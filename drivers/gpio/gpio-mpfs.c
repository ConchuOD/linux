// SPDX-License-Identifier: (GPL-2.0)
/*
 * Microchip PolarFire SoC (MPFS) GPIO controller driver
 *
 * Copyright (c) 2018-2022 Microchip Technology Inc. and its subsidiaries
 *
 * Author: Lewis Hanly <lewis.hanly@microchip.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define MPFS_GPIO_CTRL(i)		(0x4 * (i))
#define NUM_GPIO			32
#define MPFS_GPIO_EN_INT		3
#define MPFS_GPIO_EN_OUT_BUF		BIT(2)
#define MPFS_GPIO_EN_IN			BIT(1)
#define MPFS_GPIO_EN_OUT		BIT(0)

#define MPFS_GPIO_TYPE_INT_EDGE_BOTH	0x80
#define MPFS_GPIO_TYPE_INT_EDGE_NEG	0x60
#define MPFS_GPIO_TYPE_INT_EDGE_POS	0x40
#define MPFS_GPIO_TYPE_INT_LEVEL_LOW	0x20
#define MPFS_GPIO_TYPE_INT_LEVEL_HIGH	0x00
#define MPFS_GPIO_TYPE_INT_MASK		GENMASK(7, 5)
#define MPFS_IRQ_REG			0x80
#define MPFS_INP_REG			0x84
#define MPFS_OUTP_REG			0x88

struct mpfs_gpio_chip {
	void __iomem	*base;
	struct clk	*clk;
	raw_spinlock_t	lock;
	struct gpio_chip gc;
	unsigned int	irq_number[NUM_GPIO];
};

static void mpfs_gpio_assign_bit(void __iomem *addr, unsigned int bit_offset, bool value)
{
	unsigned long reg = readl(addr);

	__assign_bit(bit_offset, &reg, value);
	writel(reg, addr);
}

static int mpfs_gpio_direction_input(struct gpio_chip *gc, unsigned int gpio_index)
{
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	u32 gpio_cfg;
	unsigned long flags;

	raw_spin_lock_irqsave(&mpfs_gpio->lock, flags);

	gpio_cfg = readl(mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index));
	gpio_cfg |= MPFS_GPIO_EN_IN;
	gpio_cfg &= ~(MPFS_GPIO_EN_OUT | MPFS_GPIO_EN_OUT_BUF);
	writel(gpio_cfg, mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index));

	raw_spin_unlock_irqrestore(&mpfs_gpio->lock, flags);

	return 0;
}

static int mpfs_gpio_direction_output(struct gpio_chip *gc, unsigned int gpio_index, int value)
{
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	u32 gpio_cfg;
	unsigned long flags;

	raw_spin_lock_irqsave(&mpfs_gpio->lock, flags);

	gpio_cfg = readl(mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index));
	gpio_cfg |= MPFS_GPIO_EN_OUT | MPFS_GPIO_EN_OUT_BUF;
	gpio_cfg &= ~MPFS_GPIO_EN_IN;
	writel(gpio_cfg, mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index));

	mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_OUTP_REG, gpio_index, value);

	raw_spin_unlock_irqrestore(&mpfs_gpio->lock, flags);

	return 0;
}

static int mpfs_gpio_get_direction(struct gpio_chip *gc,
				   unsigned int gpio_index)
{
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	u32 gpio_cfg;

	gpio_cfg = readl(mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index));
	if (gpio_cfg & MPFS_GPIO_EN_IN)
		return GPIO_LINE_DIRECTION_IN;

	return GPIO_LINE_DIRECTION_OUT;
}

static int mpfs_gpio_get(struct gpio_chip *gc,
			 unsigned int gpio_index)
{
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);

	return !!(readl(mpfs_gpio->base + MPFS_INP_REG) & BIT(gpio_index));
}

static void mpfs_gpio_set(struct gpio_chip *gc, unsigned int gpio_index, int value)
{
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	unsigned long flags;

	raw_spin_lock_irqsave(&mpfs_gpio->lock, flags);

	mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_OUTP_REG,
			     gpio_index, value);

	raw_spin_unlock_irqrestore(&mpfs_gpio->lock, flags);
}

static int mpfs_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	int gpio_index = irqd_to_hwirq(data);
	u32 interrupt_type;
	u32 gpio_cfg;
	unsigned long flags;

	switch (type) {
	case IRQ_TYPE_EDGE_BOTH:
		interrupt_type = MPFS_GPIO_TYPE_INT_EDGE_BOTH;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		interrupt_type = MPFS_GPIO_TYPE_INT_EDGE_NEG;
		break;
	case IRQ_TYPE_EDGE_RISING:
		interrupt_type = MPFS_GPIO_TYPE_INT_EDGE_POS;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		interrupt_type = MPFS_GPIO_TYPE_INT_LEVEL_HIGH;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		interrupt_type = MPFS_GPIO_TYPE_INT_LEVEL_LOW;
		break;
	}

	raw_spin_lock_irqsave(&mpfs_gpio->lock, flags);

	gpio_cfg = readl(mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index));
	gpio_cfg &= ~MPFS_GPIO_TYPE_INT_MASK;
	gpio_cfg |= interrupt_type;
	writel(gpio_cfg, mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index));

	raw_spin_unlock_irqrestore(&mpfs_gpio->lock, flags);

	return 0;
}

static void mpfs_gpio_irq_enable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	int gpio_index = hwirq % NUM_GPIO;

	gpiochip_enable_irq(gc, hwirq);
	irq_chip_enable_parent(data);

	mpfs_gpio_direction_input(gc, gpio_index);
	mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_IRQ_REG, gpio_index, 1);
	mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index),
			     MPFS_GPIO_EN_INT, 1);
}

static void mpfs_gpio_irq_disable(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	int gpio_index = hwirq % NUM_GPIO;

	mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_IRQ_REG, gpio_index, 1);
	mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_GPIO_CTRL(gpio_index),
			     MPFS_GPIO_EN_INT, 0);

	irq_chip_disable_parent(data);
	gpiochip_disable_irq(gc, hwirq);
}

static void mpfs_gpio_irq_eoi(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	int offset = irqd_to_hwirq(data) % NUM_GPIO;
	unsigned long flags;

	raw_spin_lock_irqsave(&mpfs_gpio->lock, flags);
	/* Clear pending interrupt */
	mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_IRQ_REG, offset, 1);
	raw_spin_unlock_irqrestore(&mpfs_gpio->lock, flags);

	irq_chip_eoi_parent(data);
}

static int mpfs_gpio_irq_set_affinity(struct irq_data *data,
				      const struct cpumask *dest,
				      bool force)
{
	if (data->parent_data)
		return irq_chip_set_affinity_parent(data, dest, force);

	return -EINVAL;
}

static const struct irq_chip mpfs_gpio_irqchip = {
	.name		= "mpfs",
	.irq_set_type	= mpfs_gpio_irq_set_type,
	.irq_mask	= irq_chip_mask_parent,
	.irq_unmask	= irq_chip_unmask_parent,
	.irq_enable	= mpfs_gpio_irq_enable,
	.irq_disable	= mpfs_gpio_irq_disable,
	.irq_eoi	= mpfs_gpio_irq_eoi,
	.irq_set_affinity = mpfs_gpio_irq_set_affinity,
	.flags		= IRQCHIP_IMMUTABLE,
	 GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static int mpfs_gpio_child_to_parent_hwirq(struct gpio_chip *gc,
					   unsigned int child,
					   unsigned int child_type,
					   unsigned int *parent,
					   unsigned int *parent_type)
{
	struct mpfs_gpio_chip *mpfs_gpio = gpiochip_get_data(gc);
	struct irq_data *d = irq_get_irq_data(mpfs_gpio->irq_number[child]);
	*parent_type = IRQ_TYPE_NONE;
	*parent = irqd_to_hwirq(d);

	printk("gpio: parent %u\n", *parent);

	return 0;
}

static int mpfs_gpio_probe(struct platform_device *pdev)
{
	struct clk *clk;
	struct device *dev = &pdev->dev;
	struct device_node *node = pdev->dev.of_node;
	struct device_node *irq_parent;
	struct gpio_irq_chip *girq;
	struct irq_domain *parent;
	struct mpfs_gpio_chip *mpfs_gpio;
	int i, ret, ngpio;

	mpfs_gpio = devm_kzalloc(dev, sizeof(*mpfs_gpio), GFP_KERNEL);
	if (!mpfs_gpio)
		return -ENOMEM;

	mpfs_gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mpfs_gpio->base))
		return dev_err_probe(dev, PTR_ERR(mpfs_gpio->clk), "input clock not found.\n");

	clk = devm_clk_get(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "devm_clk_get failed\n");

	ret = clk_prepare_enable(clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable clock\n");

	mpfs_gpio->clk = clk;

	ngpio = of_irq_count(node);
	if (ngpio > NUM_GPIO) {
		ret = -ENXIO;
		goto cleanup_clock;
	}

	irq_parent = of_irq_find_parent(node);
	if (!irq_parent) {
		ret = -ENODEV;
		goto cleanup_clock;
	}
	parent = irq_find_host(irq_parent);
	if (!parent) {
		ret = -ENODEV;
		goto cleanup_clock;
	}

	/* Get the interrupt numbers. */
	/* Clear/Disable All interrupts before enabling parent interrupts. */
	for (i = 0; i < ngpio; i++) {
		mpfs_gpio->irq_number[i] = platform_get_irq(pdev, i);
		mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_IRQ_REG, i, 1);
		mpfs_gpio_assign_bit(mpfs_gpio->base + MPFS_GPIO_CTRL(i),
				     MPFS_GPIO_EN_INT, 0);
	}

	raw_spin_lock_init(&mpfs_gpio->lock);

	mpfs_gpio->gc.direction_input = mpfs_gpio_direction_input;
	mpfs_gpio->gc.direction_output = mpfs_gpio_direction_output;
	mpfs_gpio->gc.get_direction = mpfs_gpio_get_direction;
	mpfs_gpio->gc.get = mpfs_gpio_get;
	mpfs_gpio->gc.set = mpfs_gpio_set;
	mpfs_gpio->gc.base = -1;
	mpfs_gpio->gc.ngpio = ngpio;
	mpfs_gpio->gc.label = dev_name(dev);
	mpfs_gpio->gc.parent = dev;
	mpfs_gpio->gc.owner = THIS_MODULE;

	girq = &mpfs_gpio->gc.irq;
	gpio_irq_chip_set_chip(girq, &mpfs_gpio_irqchip);
	girq->fwnode = of_node_to_fwnode(node);
	girq->parent_domain = parent;
	girq->child_to_parent_hwirq = mpfs_gpio_child_to_parent_hwirq;
	girq->handler = handle_bad_irq;
	girq->default_type = IRQ_TYPE_NONE;

	ret = devm_gpiochip_add_data(dev, &mpfs_gpio->gc, mpfs_gpio);
	if (ret)
		goto cleanup_clock;

	platform_set_drvdata(pdev, mpfs_gpio);

	return 0;

cleanup_clock:
	clk_disable_unprepare(mpfs_gpio->clk);
	return ret;
}

static int mpfs_gpio_remove(struct platform_device *pdev)
{
	struct mpfs_gpio_chip *mpfs_gpio = platform_get_drvdata(pdev);

	clk_disable_unprepare(mpfs_gpio->clk);
	return 0;
}

static const struct of_device_id mpfs_of_ids[] = {
	{ .compatible = "microchip,mpfs-gpio", },
	{ /* end of list */ }
};

static struct platform_driver mpfs_gpio_driver = {
	.probe = mpfs_gpio_probe,
	.driver = {
		.name = "microchip,mpfs-gpio",
		.of_match_table = mpfs_of_ids,
	},
	.remove = mpfs_gpio_remove,
};
builtin_platform_driver(mpfs_gpio_driver);
