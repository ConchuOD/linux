// SPDX-License-Identifier: GPL-2.0

#include "linux/dev_printk.h"
#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>

#include "core.h"
#include "pinctrl-utils.h"
#include "pinconf.h"
#include "pinmux.h"

#define MPFS_PINCTRL_PAD_MUX_MASK GENMASK(3, 0)

#define MPFS_PINCTRL_IOCFG_MASK GENMASK(14, 0)
#define MPFS_PINCTRL_IBUFMD_MASK GENMASK(2, 0)
#define MPFS_PINCTRL_DRV_MASK GENMASK(6, 3)
#define MPFS_PINCTRL_CLAMP BIT(7)
#define MPFS_PINCTRL_ENHYST BIT(8)
#define MPFS_PINCTRL_LOCKDN BIT(9)
#define MPFS_PINCTRL_WPD BIT(10)
#define MPFS_PINCTRL_WPU BIT(11)
#define MPFS_PINCTRL_PULL_MASK GENMASK(11, 10)
#define MPFS_PINCTRL_LP_PERSIST_EN BIT(12)
#define MPFS_PINCTRL_LP_BYPASS_EN BIT(13)

#define MPFS_PINCTRL_LVCMOS25 0x6
#define MPFS_PINCTRL_BANK_VOLTAGE_MASK GENMASK(19, 16)

#define MPFS_PINCTRL_BANK2_CFG_CR 0x1c4
#define MPFS_PINCTRL_BANK4_CFG_CR 0x1c8
#define MPFS_PINCTRL_IOCFG01_REG 0x234

#define MPFS_PINCTRL_INTER_BANK_GAP 0x4

#define MPFS_PINCTRL_BANK2_START 14

#define MPFS_PINCTRL_LOCKDOWN (PIN_CONFIG_END + 1)
#define MPFS_PINCTRL_CLAMP_DIODE (PIN_CONFIG_END + 2)
#define MPFS_PINCTRL_IBUFMD (PIN_CONFIG_END + 3)

struct mpfs_pinctrl_mux_config {
	u8 pin;
	u8 function;
};

struct mpfs_pinctrl {
	struct pinctrl_dev *pctrl;
	struct device *dev;
	struct regmap *regmap;
	struct mutex mutex;
	struct pinctrl_desc desc;
	u32 bank2_voltage;
	u32 bank4_voltage;
};

struct mpfs_pinctrl_drive_strength {
	u8 ma;
	u8 val;
};

static struct mpfs_pinctrl_drive_strength mpfs_pinctrl_drive_strengths[8] = {
	{ 2,   2 },
	{ 4,   3 },
	{ 6,   4 },
	{ 8,   5 },
	{ 10,  6 },
	{ 12,  7 },
	{ 16, 10 },
	{ 20, 12 },
};

static char *mpfs_pinctrl_function_names[] = {
	"sd",
	"emmc",
	"qspi",
	"spi",
	"usb",
	"uart",
	"i2c",
	"can",
	"mdio",
	"misc",
	"reserved",
	"gpio",
	"fabric test",
	"tied-low",
	"tied-high",
	"tristate"
};

static int mpfs_pinctrl_function_map(const char *function)
{
	size_t num = ARRAY_SIZE(mpfs_pinctrl_function_names);

	for (int i = 0; i < num; i++)
		if (!strcmp(function, mpfs_pinctrl_function_names[i]))
			return i;

	return -EINVAL;
}

static const struct pinconf_generic_params mpfs_pinctrl_custom_bindings[] = {
	{ "microchip,bank-lockdown", MPFS_PINCTRL_LOCKDOWN, 1 },
	{ "microchip,clamp-diode", MPFS_PINCTRL_CLAMP_DIODE, 1 },
	{ "microchip,ibufmd", MPFS_PINCTRL_IBUFMD, 0x0 },
};

static int mpfs_pinctrl_pin_to_iomux_offset(unsigned int pin)
{
	int offset;

	switch (pin) {
	case 0 ... 7:
		offset = pin * 4;
		break;
	case 8 ... 13:
		offset = (pin - 8) * 4;
		break;
	case 14 ... 21:
		offset = (pin - 14) * 4;
		break;
	case 22 ... 29:
		offset = (pin - 22) * 4;
		break;
	case 30 ... 37:
		offset = (pin - 30) * 4;
		break;
	default:
		offset = -EINVAL;
	}

	return offset;
}

static int mpfs_pinctrl_pin_to_iomux_reg(unsigned int pin)
{
	int reg;

	switch (pin) {
	case 0 ... 7:
		reg = 0x204;
		break;
	case 8 ... 13:
		reg = 0x208;
		break;
	case 14 ... 21:
		reg = 0x20c;
		break;
	case 22 ... 29:
		reg = 0x210;
		break;
	case 30 ... 37:
		reg = 0x214;
		break;
	default:
		reg = -EINVAL;
	}

	return reg;
}

static int mpfs_pinctrl_pin_to_iocfg_reg(unsigned int pin)
{
	u32 reg = MPFS_PINCTRL_IOCFG01_REG;

	if (pin >= MPFS_PINCTRL_BANK2_START)
		reg += MPFS_PINCTRL_INTER_BANK_GAP;

	// 2 pins per 32-bit register
	reg += (pin / 2) * 0x4;

	return reg;
}

static int mpfs_pinctrl_pin_to_iocfg_offset(unsigned int pin)
{
	return 16 * (pin % 2);
}

static u32 mpfs_pinctrl_pin_to_bank_voltage(struct mpfs_pinctrl *pctrl, unsigned int pin)
{
	u32 bank_voltage;

	if (pin < MPFS_PINCTRL_BANK2_START)
		bank_voltage = pctrl->bank4_voltage;
	else
		bank_voltage = pctrl->bank2_voltage;

	return FIELD_GET(MPFS_PINCTRL_BANK_VOLTAGE_MASK, bank_voltage);
}

static void mpfs_pinctrl_dbg_show(struct pinctrl_dev *pctrl_dev, struct seq_file *seq,
				  unsigned int pin)
{
	struct mpfs_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	u32 func;
	int reg, offset;

	reg = mpfs_pinctrl_pin_to_iomux_reg(pin);
	offset = mpfs_pinctrl_pin_to_iomux_offset(pin);

	seq_printf(seq, "reg: %x, offset: %u ", reg, offset);
	seq_printf(seq, "pin: %u ", pin);

	if (reg < 0 || offset < 0)
		return;

	regmap_read(pctrl->regmap, reg, &func);
	func = (func >> offset) & MPFS_PINCTRL_PAD_MUX_MASK;
	seq_printf(seq, "func: %s (%x)\n", mpfs_pinctrl_function_names[func], func);
}

static int mpfs_pinctrl_dt_node_to_map(struct pinctrl_dev *pctrl_dev, struct device_node *np,
				       struct pinctrl_map **maps, unsigned int *num_maps)
{
	struct mpfs_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	struct device *dev = pctrl->dev;
	struct device_node *child;
	struct pinctrl_map *map;
	const char **group_names;
	const char *group_name;
	int ngroups = 0;
	int nmaps = 0;
	int ret;

	for_each_available_child_of_node(np, child)
		ngroups += 1;

	group_names = devm_kcalloc(dev, ngroups, sizeof(*group_names), GFP_KERNEL);
	if (!group_names)
		return -ENOMEM;

	map = kcalloc(ngroups * 2, sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	ngroups = 0;
	guard(mutex)(&pctrl->mutex);
	for_each_available_child_of_node_scoped(np, child) {
		struct mpfs_pinctrl_mux_config *pinmuxs;
		const char *function_name;
		unsigned int pin, *pins;
		int function, npins;

		npins = of_property_count_u32_elems(child, "pins");

		if (npins < 1) {
			dev_err(dev, "invalid pinctrl group %pOFn.%pOFn %d\n",
				np, child, npins);
			return npins;
		}

		group_name = devm_kasprintf(dev, GFP_KERNEL, "%pOFn.%pOFn",
					 np, child);
		if (!group_name)
			return -ENOMEM;

		group_names[ngroups++] = group_name;

		pins = devm_kcalloc(dev, npins, sizeof(*pins), GFP_KERNEL);
		if (!pins)
			return -ENOMEM;

		pinmuxs = devm_kcalloc(dev, npins, sizeof(*pinmuxs), GFP_KERNEL);
		if (!pinmuxs)
			return -ENOMEM;

		for (int i = 0; i < npins; i++) {
			ret = of_property_read_u32_index(child, "pins", i, &pin);
			if (ret)
				return ret;

			pins[i] = pin;
			pinmuxs[i].pin = pin;

			ret = of_property_read_string(child, "function", &function_name);
			function = mpfs_pinctrl_function_map(function_name);
			if (function < 0)
				return dev_err_probe(dev, function, "invalid function %s\n",
						     function_name);
			pinmuxs[i].function = function;
		}

		map[nmaps].type = PIN_MAP_TYPE_MUX_GROUP;
		map[nmaps].data.mux.function = np->name;
		map[nmaps].data.mux.group = group_name;
		nmaps += 1;

		ret = pinctrl_generic_add_group(pctrl_dev, group_name, pins, npins, pinmuxs);
		if (ret < 0)
			return dev_err_probe(dev, ret, "failed to add group %s: %d\n",
					     group_name, ret);

		ret = pinconf_generic_parse_dt_config(child, pctrl_dev,
						      &map[nmaps].data.configs.configs,
						      &map[nmaps].data.configs.num_configs);
		if (ret)
			return dev_err_probe(dev, ret, "failed to parse pin config of group %s\n",
				group_name);

		if (map[nmaps].data.configs.num_configs == 0)
			continue;

		map[nmaps].type = PIN_MAP_TYPE_CONFIGS_GROUP;
		map[nmaps].data.configs.group_or_pin = group_name;
		nmaps += 1;
	}

	ret = pinmux_generic_add_function(pctrl_dev, np->name, group_names, ngroups, NULL);
	if (ret < 0) {
		pinctrl_utils_free_map(pctrl_dev, map, nmaps);
		return dev_err_probe(dev, ret, "error adding function %s\n", np->name);
	}

	*maps = map;
	*num_maps = nmaps;

	return 0;
};

static const struct pinctrl_ops mpfs_pinctrl_ops = {
	.get_groups_count = pinctrl_generic_get_group_count,
	.get_group_name	= pinctrl_generic_get_group_name,
	.get_group_pins	= pinctrl_generic_get_group_pins,
	.pin_dbg_show = mpfs_pinctrl_dbg_show,
	.dt_node_to_map = mpfs_pinctrl_dt_node_to_map,
	.dt_free_map = pinctrl_utils_free_map,
};

static int mpfs_pinctrl_set_pin_func(struct mpfs_pinctrl *pctrl, u8 pin, u8 function)
{
	struct device *dev = pctrl->dev;
	u32 reg, func, mask, offset;

	reg = mpfs_pinctrl_pin_to_iomux_reg(pin);
	offset = mpfs_pinctrl_pin_to_iomux_offset(pin);

	func = function << offset;
	mask = MPFS_PINCTRL_PAD_MUX_MASK << offset;

	dev_dbg(dev, "Setting pin %u. reg: %x offset %u func %x\n", pin, reg, offset, func);

	if (reg < 0 || offset < 0)
		return -EINVAL;

	regmap_update_bits(pctrl->regmap, reg, mask, func);

	return 0;
}

static int mpfs_pinctrl_set_mux(struct pinctrl_dev *pctrl_dev, unsigned int fsel,
				unsigned int gsel)
{
	struct mpfs_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	const struct group_desc *group;
	struct mpfs_pinctrl_mux_config *configs;

	group = pinctrl_generic_get_group(pctrl_dev, gsel);
	if (!group)
		return -EINVAL;

	configs = group->data;

	for (int i = 0; i < group->grp.npins; i++)
		mpfs_pinctrl_set_pin_func(pctrl, configs[i].pin, configs[i].function);

	return 0;
}

static const struct pinmux_ops mpfs_pinctrl_pinmux_ops = {
	.get_functions_count	= pinmux_generic_get_function_count,
	.get_function_name	= pinmux_generic_get_function_name,
	.get_function_groups	= pinmux_generic_get_function_groups,
	.set_mux		= mpfs_pinctrl_set_mux,
};

static int mpfs_pinctrl_get_drive_strength_ma(u32 drive_strength)
{
	size_t num = ARRAY_SIZE(mpfs_pinctrl_drive_strengths);

	for (int i = 0; i < num; i++)
		if (drive_strength == mpfs_pinctrl_drive_strengths[i].val)
			return mpfs_pinctrl_drive_strengths[i].ma;

	return -EINVAL;
}

static int mpfs_pinctrl_get_drive_strength_val(u32 drive_strength_ma)
{
	size_t num = ARRAY_SIZE(mpfs_pinctrl_drive_strengths);

	if (!drive_strength_ma)
		return -EINVAL;

	for (int i = 0; i < num; i++)
		if (drive_strength_ma <= mpfs_pinctrl_drive_strengths[i].ma)
			return mpfs_pinctrl_drive_strengths[i].val;

	return mpfs_pinctrl_drive_strengths[num - 1].val;
}

static int mpfs_pinctrl_pinconf_get(struct pinctrl_dev *pctrl_dev, unsigned int pin,
				    unsigned long *config)
{
	struct mpfs_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	int param = pinconf_to_config_param(*config);
	int reg = mpfs_pinctrl_pin_to_iocfg_reg(pin);
	int val;
	u32 arg;
	//TODO bank_voltage;
	u8 str;

	regmap_read(pctrl->regmap, reg, &val);

	val = val >> mpfs_pinctrl_pin_to_iocfg_offset(pin);
	val = val & MPFS_PINCTRL_IOCFG_MASK;

	switch (param) {
	case PIN_CONFIG_BIAS_BUS_HOLD:
		if (!(val & MPFS_PINCTRL_WPD))
			return -EINVAL;

		if (!(val & MPFS_PINCTRL_WPU))
			return -EINVAL;

		arg = 1;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if (!(val & MPFS_PINCTRL_WPD))
			return -EINVAL;

		if (val & MPFS_PINCTRL_WPU)
			return -EINVAL;

		arg = 1;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if (!(val & MPFS_PINCTRL_WPU))
			return -EINVAL;

		if (val & MPFS_PINCTRL_WPD)
			return -EINVAL;

		arg = 1;
		break;
	case PIN_CONFIG_BIAS_DISABLE:
		if (val & MPFS_PINCTRL_PULL_MASK)
			return -EINVAL;

		arg = 1;
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		str = FIELD_GET(MPFS_PINCTRL_DRV_MASK, val);
		if (!str)
			return -EINVAL;

		arg = mpfs_pinctrl_get_drive_strength_ma(str);
		break;
	//TODO @Linus, it correct to group these 3? There's no control over voltage.
	case PIN_CONFIG_INPUT_SCHMITT:
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
	case PIN_CONFIG_INPUT_SCHMITT_UV:
		//TODO Is it enabled regardless of register setting, or must it
		// be set for lower voltage IO? Docs are missing, MSS Configurator
		// is not clear. Leaning towards the latter.
		//bank_voltage = mpfs_pinctrl_pin_to_bank_voltage(pctrl, pin);
		//if (bank_voltage < MPFS_PINCTRL_LVCMOS25) {
		//	arg = 1;
		//	break;
		//}

		if (!FIELD_GET(MPFS_PINCTRL_ENHYST, val))
			return -EINVAL;

		arg = 1;
		break;
	case PIN_CONFIG_PERSIST_STATE:
		if (!FIELD_GET(MPFS_PINCTRL_LP_PERSIST_EN, val))
			return -EINVAL;

		arg = 1;
		break;
	case PIN_CONFIG_MODE_LOW_POWER:
		if (!FIELD_GET(MPFS_PINCTRL_LP_BYPASS_EN, val))
			return -EINVAL;

		arg = 1;
		break;
	case MPFS_PINCTRL_CLAMP_DIODE:
		if (!FIELD_GET(MPFS_PINCTRL_CLAMP, val))
			return -EINVAL;

		arg = 1;
		break;
	case MPFS_PINCTRL_LOCKDOWN:
		if (!FIELD_GET(MPFS_PINCTRL_LOCKDN, val))
			return -EINVAL;

		arg = 1;
		break;
	case MPFS_PINCTRL_IBUFMD:
		arg = FIELD_GET(MPFS_PINCTRL_IBUFMD_MASK, val);
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int mpfs_pinctrl_pinconf_generate_config(struct mpfs_pinctrl *pctrl, unsigned int pin,
						unsigned long *configs, unsigned int num_configs,
						u32 *value)
{
	u32 val = 0;

	for (int i = 0; i < num_configs; i++) {
		int param, tmp;
		u32 arg;
		//TODO bank_voltage;

		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_BIAS_BUS_HOLD:
			val |= MPFS_PINCTRL_PULL_MASK;
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
			//TODO always start from val == 0, there's no reason to ever actually
			// clear anything AFAICT. @Linus, does the driver need to check mutual
			// exclusion on these, or can I drop the clearing?
			val &= ~MPFS_PINCTRL_PULL_MASK;
			val |= MPFS_PINCTRL_WPD;
			break;
		case PIN_CONFIG_BIAS_PULL_UP:
			val &= ~MPFS_PINCTRL_PULL_MASK;
			val |= MPFS_PINCTRL_WPU;
			break;
		case PIN_CONFIG_BIAS_DISABLE:
			val &= ~MPFS_PINCTRL_PULL_MASK;
			break;
		case PIN_CONFIG_DRIVE_STRENGTH:
			tmp = mpfs_pinctrl_get_drive_strength_val(arg);
			if (tmp < 0)
				return tmp;

			val |= FIELD_PREP(MPFS_PINCTRL_DRV_MASK, tmp);
			break;
		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			if (!arg)
				break;
			fallthrough;
		case PIN_CONFIG_INPUT_SCHMITT:
		case PIN_CONFIG_INPUT_SCHMITT_UV:
			//TODO Is it enabled regardless of register setting, or must it
			// be set for lower voltage IO? Docs are missing, MSS Configurator
			// is not clear. Leaning towards the latter.
			//bank_voltage = mpfs_pinctrl_pin_to_bank_voltage(pctrl, pin);
			//if (bank_voltage < MPFS_PINCTRL_LVCMOS25 && !arg) {
			//	dev_err(pctrl->dev,
			//		"schmitt always enabled for 1.2, 1.5 and 1.8 volt io\n");
			//	return -EINVAL;
			//}
			val |= MPFS_PINCTRL_ENHYST;
			break;
		case PIN_CONFIG_PERSIST_STATE:
			val |= MPFS_PINCTRL_LP_PERSIST_EN;
			break;
		case PIN_CONFIG_MODE_LOW_POWER:
			if (arg)
				val |= MPFS_PINCTRL_LP_BYPASS_EN;
			break;
		//TODO @Linus, do I have to document these custom controls other than in the binding?
		case MPFS_PINCTRL_CLAMP_DIODE:
			val |= MPFS_PINCTRL_CLAMP;
			break;
		case MPFS_PINCTRL_LOCKDOWN:
			val &= MPFS_PINCTRL_LOCKDN;
			break;
		case MPFS_PINCTRL_IBUFMD:
			val |= FIELD_PREP(MPFS_PINCTRL_IBUFMD_MASK, arg);
			break;
		default:
			dev_err(pctrl->dev, "config %u not supported\n", param);
			return -ENOTSUPP;
		}
	}

	*value = val;
	return 0;
}

static int mpfs_pinctrl_pin_set_config(struct mpfs_pinctrl *pctrl, unsigned int pin, u32 config)
{
	int reg = mpfs_pinctrl_pin_to_iocfg_reg(pin);
	int offset = mpfs_pinctrl_pin_to_iocfg_offset(pin);
	u32 val, mask;

	mask = MPFS_PINCTRL_IOCFG_MASK << offset;
	val = config << offset;

	regmap_update_bits(pctrl->regmap, reg, mask, val);

	return 0;
}

static int mpfs_pinctrl_pinconf_set(struct pinctrl_dev *pctrl_dev, unsigned int pin,
				    unsigned long *configs, unsigned int num_configs)
{
	struct mpfs_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	u32 val;
	int ret;

	ret = mpfs_pinctrl_pinconf_generate_config(pctrl, pin, configs, num_configs, &val);
	if (ret)
		return ret;

	return mpfs_pinctrl_pin_set_config(pctrl, pin, val);
}

static int mpfs_pinctrl_pinconf_group_set(struct pinctrl_dev *pctrl_dev, unsigned int gsel,
					  unsigned long *configs, unsigned int num_configs)
{
	struct mpfs_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	const struct group_desc *group;
	unsigned int pin;
	u32 val;
	int ret;

	group = pinctrl_generic_get_group(pctrl_dev, gsel);
	if (!group)
		return -EINVAL;

	/*
	 * Assume that the first pin in a group is representative, as the mss
	 * configurator doesn't allow splitting a function between two banks.
	 */
	pin = group->grp.pins[0];

	ret = mpfs_pinctrl_pinconf_generate_config(pctrl, pin, configs, num_configs, &val);
	if (ret)
		return ret;

	for (int i = 0; i < group->grp.npins; i++)
		mpfs_pinctrl_pin_set_config(pctrl, group->grp.pins[i], val);

	return 0;
}

static void mpfs_pinctrl_pinconf_dbg_show(struct pinctrl_dev *pctrl_dev, struct seq_file *seq,
					  unsigned int pin)
{
	struct mpfs_pinctrl *pctrl = pinctrl_dev_get_drvdata(pctrl_dev);
	u32 val;
	int reg, offset;

	seq_printf(seq, ", bank voltage: 0x%x, ", mpfs_pinctrl_pin_to_bank_voltage(pctrl, pin));

	reg = mpfs_pinctrl_pin_to_iocfg_reg(pin);
	offset = mpfs_pinctrl_pin_to_iocfg_offset(pin);

	seq_printf(seq, "pin: %u ", pin);
	seq_printf(seq, "reg: %x offset: %u ", reg, offset);

	if (reg < 0 || offset < 0)
		return;

	regmap_read(pctrl->regmap, reg, &val);
	val = (val & (MPFS_PINCTRL_IOCFG_MASK << offset)) >> offset;
	seq_printf(seq, "val: %x\n", val);
}

static const struct pinconf_ops mpfs_pinctrl_pinconf_ops = {
	.pin_config_get			= mpfs_pinctrl_pinconf_get,
	.pin_config_set			= mpfs_pinctrl_pinconf_set,
	.pin_config_group_set		= mpfs_pinctrl_pinconf_group_set,
	.pin_config_dbg_show		= mpfs_pinctrl_pinconf_dbg_show,
	.is_generic			= true,
};

static const struct pinctrl_pin_desc mpfs_pinctrl_pins[] = {
	PINCTRL_PIN(0,  "bank 4 0"),
	PINCTRL_PIN(1,  "bank 4 1"),
	PINCTRL_PIN(2,  "bank 4 2"),
	PINCTRL_PIN(3,  "bank 4 3"),
	PINCTRL_PIN(4,  "bank 4 4"),
	PINCTRL_PIN(5,  "bank 4 5"),
	PINCTRL_PIN(6,  "bank 4 6"),
	PINCTRL_PIN(7,  "bank 4 7"),
	PINCTRL_PIN(8,  "bank 4 8"),
	PINCTRL_PIN(9,  "bank 4 9"),
	PINCTRL_PIN(10, "bank 4 10"),
	PINCTRL_PIN(11, "bank 4 11"),
	PINCTRL_PIN(12, "bank 4 12"),
	PINCTRL_PIN(13, "bank 4 13"),

	PINCTRL_PIN(14, "bank 2 0"),
	PINCTRL_PIN(15, "bank 2 1"),
	PINCTRL_PIN(16, "bank 2 2"),
	PINCTRL_PIN(17, "bank 2 3"),
	PINCTRL_PIN(18, "bank 2 4"),
	PINCTRL_PIN(19, "bank 2 5"),
	PINCTRL_PIN(20, "bank 2 6"),
	PINCTRL_PIN(21, "bank 2 7"),
	PINCTRL_PIN(22, "bank 2 8"),
	PINCTRL_PIN(23, "bank 2 9"),
	PINCTRL_PIN(24, "bank 2 10"),
	PINCTRL_PIN(25, "bank 2 11"),
	PINCTRL_PIN(26, "bank 2 12"),
	PINCTRL_PIN(27, "bank 2 13"),
	PINCTRL_PIN(28, "bank 2 14"),
	PINCTRL_PIN(29, "bank 2 15"),
	PINCTRL_PIN(30, "bank 2 16"),
	PINCTRL_PIN(31, "bank 2 17"),
	PINCTRL_PIN(32, "bank 2 18"),
	PINCTRL_PIN(33, "bank 2 19"),
	PINCTRL_PIN(34, "bank 2 20"),
	PINCTRL_PIN(35, "bank 2 21"),
	PINCTRL_PIN(36, "bank 2 22"),
	PINCTRL_PIN(37, "bank 2 23"),
};

static int mpfs_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mpfs_pinctrl *pctrl;
	struct regmap *sysreg_scb;
	int ret;

	pctrl = devm_kzalloc(dev, sizeof(*pctrl), GFP_KERNEL);
	if (!pctrl)
		return -ENOMEM;

	pctrl->regmap = device_node_to_regmap(pdev->dev.parent->of_node);
	if (IS_ERR(pctrl->regmap))
		dev_err_probe(dev, PTR_ERR(pctrl->regmap), "Failed to find syscon regmap\n");

	sysreg_scb = syscon_regmap_lookup_by_compatible("microchip,mpfs-sysreg-scb");
	if (IS_ERR(sysreg_scb))
		return PTR_ERR(sysreg_scb);

	pctrl->desc.name = dev_name(dev);
	pctrl->desc.pins = mpfs_pinctrl_pins;
	pctrl->desc.npins = ARRAY_SIZE(mpfs_pinctrl_pins);
	pctrl->desc.pctlops = &mpfs_pinctrl_ops;
	pctrl->desc.pmxops = &mpfs_pinctrl_pinmux_ops;
	pctrl->desc.confops = &mpfs_pinctrl_pinconf_ops;
	pctrl->desc.owner = THIS_MODULE;
	pctrl->desc.num_custom_params = ARRAY_SIZE(mpfs_pinctrl_custom_bindings);
	pctrl->desc.custom_params = mpfs_pinctrl_custom_bindings;

	pctrl->dev = dev;

	ret = devm_mutex_init(dev, &pctrl->mutex);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, pctrl);

	pctrl->pctrl = devm_pinctrl_register(&pdev->dev, &pctrl->desc, pctrl);
	if (IS_ERR(pctrl->pctrl))
		return PTR_ERR(pctrl->pctrl);

	return 0;
}

static const struct of_device_id mpfs_pinctrl_of_match[] = {
	{ .compatible = "microchip,mpfs-pinctrl-mssio" },
	{ }
};
MODULE_DEVICE_TABLE(of, mpfs_pinctrl_of_match);

static struct platform_driver mpfs_pinctrl_driver = {
	.driver = {
		.name = "mpfs-pinctrl",
		.of_match_table = mpfs_pinctrl_of_match,
	},
	.probe = mpfs_pinctrl_probe,
};
module_platform_driver(mpfs_pinctrl_driver);

MODULE_AUTHOR("Conor Dooley <conor.dooley@microchip.com>");
MODULE_DESCRIPTION("Polarfire SoC mssio pinctrl driver");
MODULE_LICENSE("GPL");
