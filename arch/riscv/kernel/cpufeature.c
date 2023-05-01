// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copied from arch/arm64/kernel/cpufeature.c
 *
 * Copyright (C) 2015 ARM Ltd.
 * Copyright (C) 2017 SiFive
 */

#include <linux/bitmap.h>
#include <linux/ctype.h>
#include <linux/log2.h>
#include <linux/memory.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/hwcap.h>
#include <asm/patch.h>
#include <asm/processor.h>

#define NUM_ALPHA_EXTS ('z' - 'a' + 1)

unsigned long elf_hwcap __read_mostly;

/* Host ISA bitmap */
static DECLARE_BITMAP(riscv_isa, RISCV_ISA_EXT_MAX) __read_mostly;

/* Performance information */
DEFINE_PER_CPU(long, misaligned_access_speed);

/**
 * riscv_isa_extension_base() - Get base extension word
 *
 * @isa_bitmap: ISA bitmap to use
 * Return: base extension word as unsigned long value
 *
 * NOTE: If isa_bitmap is NULL then Host ISA bitmap will be used.
 */
unsigned long riscv_isa_extension_base(const unsigned long *isa_bitmap)
{
	if (!isa_bitmap)
		return riscv_isa[0];
	return isa_bitmap[0];
}
EXPORT_SYMBOL_GPL(riscv_isa_extension_base);

/**
 * __riscv_isa_extension_available() - Check whether given extension
 * is available or not
 *
 * @isa_bitmap: ISA bitmap to use
 * @bit: bit position of the desired extension
 * Return: true or false
 *
 * NOTE: If isa_bitmap is NULL then Host ISA bitmap will be used.
 */
bool __riscv_isa_extension_available(const unsigned long *isa_bitmap, int bit)
{
	const unsigned long *bmap = (isa_bitmap) ? isa_bitmap : riscv_isa;

	if (bit >= RISCV_ISA_EXT_MAX)
		return false;

	return test_bit(bit, bmap) ? true : false;
}
EXPORT_SYMBOL_GPL(__riscv_isa_extension_available);

static bool riscv_isa_extension_check(int id)
{
	switch (id) {
	case RISCV_ISA_EXT_ZICBOM:
		if (!riscv_cbom_block_size) {
			pr_err("Zicbom detected in ISA string, but no cbom-block-size found\n");
			return false;
		} else if (!is_power_of_2(riscv_cbom_block_size)) {
			pr_err("cbom-block-size present, but is not a power-of-2\n");
			return false;
		}
		return true;
	case RISCV_ISA_EXT_ZICBOZ:
		if (!riscv_cboz_block_size) {
			pr_err("Zicboz detected in ISA string, but no cboz-block-size found\n");
			return false;
		} else if (!is_power_of_2(riscv_cboz_block_size)) {
			pr_err("cboz-block-size present, but is not a power-of-2\n");
			return false;
		}
		return true;
	}

	return true;
}

const struct riscv_isa_extension riscv_isa_extensions[] = {
	RISCV_ISA_EXT_CFG(i, RISCV_ISA_EXT_i, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(m, RISCV_ISA_EXT_m, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(a, RISCV_ISA_EXT_a, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(f, RISCV_ISA_EXT_f, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(d, RISCV_ISA_EXT_d, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(q, RISCV_ISA_EXT_q, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(c, RISCV_ISA_EXT_c, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(b, RISCV_ISA_EXT_b, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(k, RISCV_ISA_EXT_k, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(j, RISCV_ISA_EXT_j, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(p, RISCV_ISA_EXT_p, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(v, RISCV_ISA_EXT_v, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(h, RISCV_ISA_EXT_h, "v1.0.0", false),
	RISCV_ISA_EXT_CFG(zicbom, RISCV_ISA_EXT_ZICBOM, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(zicboz, RISCV_ISA_EXT_ZICBOZ, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(zicsr, RISCV_ISA_EXT_ZICSR, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(zifencei, RISCV_ISA_EXT_ZIFENCEI, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(zihintpause, RISCV_ISA_EXT_ZIHINTPAUSE, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(zbb, RISCV_ISA_EXT_ZBB, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(sscofpmf, RISCV_ISA_EXT_SSCOFPMF, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(sstc, RISCV_ISA_EXT_SSTC, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(svinval, RISCV_ISA_EXT_SVINVAL, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(svnapot, RISCV_ISA_EXT_SVNAPOT, "v1.0.0", true),
	RISCV_ISA_EXT_CFG(svpbmt, RISCV_ISA_EXT_SVPBMT, "v1.0.0", true),
	RISCV_ISA_EXT_CFG("", RISCV_ISA_EXT_MAX, "", false),
};

static void __init riscv_fill_hwcap_isa_string(void)
{
	struct device_node *node;
	const char *isa;
	int rc;
	unsigned long isa2hwcap[26] = {0};
	unsigned int cpu;

	isa2hwcap['i' - 'a'] = COMPAT_HWCAP_ISA_I;
	isa2hwcap['m' - 'a'] = COMPAT_HWCAP_ISA_M;
	isa2hwcap['a' - 'a'] = COMPAT_HWCAP_ISA_A;
	isa2hwcap['f' - 'a'] = COMPAT_HWCAP_ISA_F;
	isa2hwcap['d' - 'a'] = COMPAT_HWCAP_ISA_D;
	isa2hwcap['c' - 'a'] = COMPAT_HWCAP_ISA_C;

	pr_info("Falling back to reading hwcap from deprecated riscv,isa\n");

	for_each_possible_cpu(cpu) {
		unsigned long this_hwcap = 0;
		DECLARE_BITMAP(this_isa, RISCV_ISA_EXT_MAX);
		bitmap_zero(this_isa, RISCV_ISA_EXT_MAX);

		node = of_cpu_device_node_get(cpu);
		if (!node) {
			pr_warn("Unable to find cpu node\n");
			continue;
		}

		rc = of_property_read_string(node, "riscv,isa", &isa);
		of_node_put(node);
		if (rc) {
			pr_warn("Unable to find \"riscv,isa\" devicetree entry\n");
			continue;
		}

		/*
		 * For all possible cpus, we have already validated in
		 * the boot process that they at least contain "rv" and
		 * whichever of "32"/"64" this kernel supports, and so this
		 * section can be skipped.
		 */
		isa += 4;

		while (*isa) {
			const char *ext = isa++;
			const char *ext_end = isa;
			bool ext_long = false, ext_err = false;

			switch (*ext) {
			case 's':
				/*
				 * Workaround for invalid single-letter 's' & 'u'(QEMU).
				 * No need to set the bit in riscv_isa as 's' & 'u' are
				 * not valid ISA extensions. It works until multi-letter
				 * extension starting with "Su" appears.
				 */
				if (ext[-1] != '_' && ext[1] == 'u') {
					++isa;
					ext_err = true;
					break;
				}
				fallthrough;
			case 'S':
			case 'x':
			case 'X':
			case 'z':
			case 'Z':
				/*
				 * Before attempting to parse the extension itself, we find its end.
				 * As multi-letter extensions must be split from other multi-letter
				 * extensions with an "_", the end of a multi-letter extension will
				 * either be the null character as of_property_read_string() returns
				 * null-terminated strings, or the "_" at the start of the next
				 * multi-letter extension.
				 *
				 * Next, as the extensions version is currently ignored, we
				 * eliminate that portion. This is done by parsing backwards from
				 * the end of the extension, removing any numbers. This may be a
				 * major or minor number however, so the process is repeated if a
				 * minor number was found.
				 *
				 * ext_end is intended to represent the first character *after* the
				 * name portion of an extension, but will be decremented to the last
				 * character itself while eliminating the extensions version number.
				 * A simple re-increment solves this problem.
				 */
				ext_long = true;
				for (; *isa && *isa != '_'; ++isa)
					if (unlikely(!isalnum(*isa)))
						ext_err = true;

				ext_end = isa;
				if (unlikely(ext_err))
					break;

				if (!isdigit(ext_end[-1]))
					break;

				while (isdigit(*--ext_end))
					;

				if (tolower(ext_end[0]) != 'p' || !isdigit(ext_end[-1])) {
					++ext_end;
					break;
				}

				while (isdigit(*--ext_end))
					;

				++ext_end;
				break;
			default:
				/*
				 * Things are a little easier for single-letter extensions, as they
				 * are parsed forwards.
				 *
				 * After checking that our starting position is valid, we need to
				 * ensure that, when isa was incremented at the start of the loop,
				 * that it arrived at the start of the next extension.
				 *
				 * If we are already on a non-digit, there is nothing to do. Either
				 * we have a multi-letter extension's _, or the start of an
				 * extension.
				 *
				 * Otherwise we have found the current extension's major version
				 * number. Parse past it, and a subsequent p/minor version number
				 * if present. The `p` extension must not appear immediately after
				 * a number, so there is no fear of missing it.
				 *
				 */
				if (unlikely(!isalpha(*ext))) {
					ext_err = true;
					break;
				}

				if (!isdigit(*isa))
					break;

				while (isdigit(*++isa))
					;

				if (tolower(*isa) != 'p')
					break;

				if (!isdigit(*++isa)) {
					--isa;
					break;
				}

				while (isdigit(*++isa))
					;

				break;
			}

			/*
			 * The parser expects that at the start of an iteration isa points to the
			 * first character of the next extension. As we stop parsing an extension
			 * on meeting a non-alphanumeric character, an extra increment is needed
			 * where the succeeding extension is a multi-letter prefixed with an "_".
			 */
			if (*isa == '_')
				++isa;

#define SET_ISA_EXT_MAP(name, bit)							\
			do {								\
				if ((ext_end - ext == sizeof(name) - 1) &&		\
				     !strncasecmp(ext, name, sizeof(name) - 1) &&	\
				     riscv_isa_extension_check(bit))			\
					set_bit(bit, this_isa);				\
			} while (false)							\

			if (unlikely(ext_err))
				continue;
			if (!ext_long) {
				int nr = tolower(*ext) - 'a';

				if (riscv_isa_extension_check(nr)) {
					this_hwcap |= isa2hwcap[nr];
					set_bit(nr, this_isa);
				}
			} else {
				for (int i = 0; i < ARRAY_SIZE(riscv_isa_extensions); i++)
					SET_ISA_EXT_MAP(riscv_isa_extensions[i].name, riscv_isa_extensions[i].key);
			}
#undef SET_ISA_EXT_MAP
		}

	/*
	 * Linux requires Zicsr & Zifencei, so we may as well always
	 * set them.
	 */
	set_bit(RISCV_ISA_EXT_ZIFENCEI, this_isa);
	set_bit(RISCV_ISA_EXT_ZICSR, this_isa);

	/*
	 * All "okay" hart should have same isa. Set HWCAP based on
	 * common capabilities of every "okay" hart, in case they don't
	 * have.
	 */
	if (elf_hwcap)
		elf_hwcap &= this_hwcap;
	else
		elf_hwcap = this_hwcap;

	if (bitmap_empty(riscv_isa, RISCV_ISA_EXT_MAX))
		bitmap_copy(riscv_isa, this_isa, RISCV_ISA_EXT_MAX);
	else
		bitmap_and(riscv_isa, riscv_isa, this_isa, RISCV_ISA_EXT_MAX);
	}
}

static bool __init riscv_fill_hwcap_new(void)
{
	struct device_node *node;
	bool detected;
	unsigned long isa2hwcap[26] = {0};
	unsigned int cpu;

	isa2hwcap['i' - 'a'] = COMPAT_HWCAP_ISA_I;
	isa2hwcap['m' - 'a'] = COMPAT_HWCAP_ISA_M;
	isa2hwcap['a' - 'a'] = COMPAT_HWCAP_ISA_A;
	isa2hwcap['f' - 'a'] = COMPAT_HWCAP_ISA_F;
	isa2hwcap['d' - 'a'] = COMPAT_HWCAP_ISA_D;
	isa2hwcap['c' - 'a'] = COMPAT_HWCAP_ISA_C;

	for_each_possible_cpu(cpu) {
		unsigned long this_hwcap = 0;
		DECLARE_BITMAP(this_isa, RISCV_ISA_EXT_MAX);

		node = of_cpu_device_node_get(cpu);
		if (!node) {
			pr_warn("Unable to find cpu node\n");
			continue;
		}

		for (int k = 0; k < ARRAY_SIZE(riscv_isa_extensions) - 1; k++) {
			const char *tmp;

			of_property_read_string(node, riscv_isa_extensions[k].prop_name, &tmp);
			if (!tmp)
				continue;

			detected = true;

			if (riscv_isa_extension_check(riscv_isa_extensions[k].key) &&
			    !strcmp(riscv_isa_extensions[k].version, tmp)) {
				if (!riscv_isa_extensions[k].multi_letter)
					this_hwcap |= isa2hwcap[riscv_isa_extensions[k].key];

				set_bit(riscv_isa_extensions[k].key, this_isa);
			}
		}

		/*
		 * All "okay" hart should have same isa. Set HWCAP based on
		 * common capabilities of every "okay" hart, in case they don't
		 * have.
		 */
		if (elf_hwcap)
			elf_hwcap &= this_hwcap;
		else
			elf_hwcap = this_hwcap;

		if (bitmap_empty(riscv_isa, RISCV_ISA_EXT_MAX))
			bitmap_copy(riscv_isa, this_isa, RISCV_ISA_EXT_MAX);
		else
			bitmap_and(riscv_isa, riscv_isa, this_isa, RISCV_ISA_EXT_MAX);
	}

	return detected;
}

void __init riscv_fill_hwcap(void)
{
	char print_str[NUM_ALPHA_EXTS + 1];
	int i, j;

	elf_hwcap = 0;

	bitmap_zero(riscv_isa, RISCV_ISA_EXT_MAX);

	/*
	 * Since old dtbs must continue to work just as well/badly as they ever
	 * did, fall back to the isa string if the new method doesn't work.
	 */
	if (!riscv_fill_hwcap_new())
		riscv_fill_hwcap_isa_string();

	/*
	 * We don't support systems with F but without D, so mask those out
	 * here.
	 */
	if ((elf_hwcap & COMPAT_HWCAP_ISA_F) && !(elf_hwcap & COMPAT_HWCAP_ISA_D)) {
		pr_info("This kernel does not support systems with F but not D\n");
		elf_hwcap &= ~COMPAT_HWCAP_ISA_F;
	}

	memset(print_str, 0, sizeof(print_str));
	for (i = 0, j = 0; i < NUM_ALPHA_EXTS; i++)
		if (riscv_isa[0] & BIT_MASK(i))
			print_str[j++] = (char)('a' + i);
	pr_info("riscv: base ISA extensions %s\n", print_str);

	memset(print_str, 0, sizeof(print_str));
	for (i = 0, j = 0; i < NUM_ALPHA_EXTS; i++)
		if (elf_hwcap & BIT_MASK(i))
			print_str[j++] = (char)('a' + i);
	pr_info("riscv: ELF capabilities %s\n", print_str);
}

#ifdef CONFIG_RISCV_ALTERNATIVE
/*
 * Alternative patch sites consider 48 bits when determining when to patch
 * the old instruction sequence with the new. These bits are broken into a
 * 16-bit vendor ID and a 32-bit patch ID. A non-zero vendor ID means the
 * patch site is for an erratum, identified by the 32-bit patch ID. When
 * the vendor ID is zero, the patch site is for a cpufeature. cpufeatures
 * further break down patch ID into two 16-bit numbers. The lower 16 bits
 * are the cpufeature ID and the upper 16 bits are used for a value specific
 * to the cpufeature and patch site. If the upper 16 bits are zero, then it
 * implies no specific value is specified. cpufeatures that want to control
 * patching on a per-site basis will provide non-zero values and implement
 * checks here. The checks return true when patching should be done, and
 * false otherwise.
 */
static bool riscv_cpufeature_patch_check(u16 id, u16 value)
{
	if (!value)
		return true;

	switch (id) {
	case RISCV_ISA_EXT_ZICBOZ:
		/*
		 * Zicboz alternative applications provide the maximum
		 * supported block size order, or zero when it doesn't
		 * matter. If the current block size exceeds the maximum,
		 * then the alternative cannot be applied.
		 */
		return riscv_cboz_block_size <= (1U << value);
	}

	return false;
}

void __init_or_module riscv_cpufeature_patch_func(struct alt_entry *begin,
						  struct alt_entry *end,
						  unsigned int stage)
{
	struct alt_entry *alt;
	void *oldptr, *altptr;
	u16 id, value;

	if (stage == RISCV_ALTERNATIVES_EARLY_BOOT)
		return;

	for (alt = begin; alt < end; alt++) {
		if (alt->vendor_id != 0)
			continue;

		id = PATCH_ID_CPUFEATURE_ID(alt->patch_id);

		if (id >= RISCV_ISA_EXT_MAX) {
			WARN(1, "This extension id:%d is not in ISA extension list", id);
			continue;
		}

		if (!__riscv_isa_extension_available(NULL, id))
			continue;

		value = PATCH_ID_CPUFEATURE_VALUE(alt->patch_id);
		if (!riscv_cpufeature_patch_check(id, value))
			continue;

		oldptr = ALT_OLD_PTR(alt);
		altptr = ALT_ALT_PTR(alt);

		mutex_lock(&text_mutex);
		patch_text_nosync(oldptr, altptr, alt->alt_len);
		riscv_alternative_fix_offsets(oldptr, alt->alt_len, oldptr - altptr);
		mutex_unlock(&text_mutex);
	}
}
#endif
