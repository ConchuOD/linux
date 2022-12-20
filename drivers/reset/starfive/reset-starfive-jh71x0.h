/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Emil Renner Berthing <kernel@esmil.dk>
 */

#ifndef __RESET_STARFIVE_JH71X0_H
#define __RESET_STARFIVE_JH71X0_H

struct reset_info {
	unsigned int nr_resets;
	unsigned int assert_offset;
	unsigned int status_offset;
};

int reset_starfive_jh71x0_register(struct device *dev, struct device_node *of_node,
				   void __iomem *assert, void __iomem *status,
				   const u32 *asserted, unsigned int nr_resets,
				   struct module *owner);

#endif /* __RESET_STARFIVE_JH71X0_H */
