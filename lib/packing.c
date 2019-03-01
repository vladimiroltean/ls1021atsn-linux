// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2018, NXP Semiconductors
 * Copyright (c) 2018-2019, Vladimir Oltean <olteanv@gmail.com>
 */
#include <linux/packing.h>
#include <linux/module.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/types.h>

/*
 * Generic field packing and unpacking functions
 * ---------------------------------------------
 *
 * Below are several interpretations of a contiguous memory region of
 * 8 bytes in size. The bytes are (implicitly) numbered 0, 1, 2, ..., 7.
 *
 * This API deals with 2 basic operations:
 *   - Packing a CPU-usable number into a memory buffer (with hardware
 *     constraints/quirks)
 *   - Unpacking a memory buffer (which has hardware constraints/quirks)
 *     into a CPU-usable number.
 *
 * The API offers an abstraction over said hardware constraints and quirks,
 * over CPU endianness and therefore between possible mismatches between
 * the two.
 *
 * The basic unit of these API functions is the u64. From the CPU's
 * perspective, bit 63 always means bit offset 7 of byte 7, albeit only
 * logically. The question is: where do we lay this bit out in memory?
 *
 * The following examples cover the memory layout of a packed u64 field.
 * The byte offsets in the packed buffer are always implicitly 0, 1, ... 7.
 * What the examples show is where the logical bytes and bits sit.
 *
 * 1. Normally (no quirks), we would do it like this:
 *
 * 63 62 61 60 59 58 57 56 55 54 53 52 51 50 49 48 47 46 45 44 43 42 41 40 39 38 37 36 35 34 33 32
 * 7                       6                       5                        4
 * 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
 * 3                       2                       1                        0
 *
 * That is, the MSByte (7) of the CPU-usable u64 sits at memory offset 0, and the
 * LSByte (0) of the u64 sits at memory offset 7.
 * This corresponds to what most folks would regard to as "big endian", where
 * bit i corresponds to the number 2^i. This is also referred to in further
 * comments as "logical" notation.
 *
 *
 * 2. If QUIRK_MSB_ON_THE_RIGHT is set, we do it like this:
 *
 * 56 57 58 59 60 61 62 63 48 49 50 51 52 53 54 55 40 41 42 43 44 45 46 47 32 33 34 35 36 37 38 39
 * 7                       6                        5                       4
 * 24 25 26 27 28 29 30 31 16 17 18 19 20 21 22 23  8  9 10 11 12 13 14 15  0  1  2  3  4  5  6  7
 * 3                       2                        1                       0
 *
 * That is, QUIRK_MSB_ON_THE_RIGHT does not affect byte positioning, but
 * inverts bit offsets inside a byte.
 *
 *
 * 3. If QUIRK_LITTLE_ENDIAN is set, we do it like this:
 *
 * 39 38 37 36 35 34 33 32 47 46 45 44 43 42 41 40 55 54 53 52 51 50 49 48 63 62 61 60 59 58 57 56
 * 4                       5                       6                       7
 * 7  6  5  4  3  2  1  0  15 14 13 12 11 10  9  8 23 22 21 20 19 18 17 16 31 30 29 28 27 26 25 24
 * 0                       1                       2                       3
 *
 * Therefore, QUIRK_LITTLE_ENDIAN means that inside the memory region, every
 * byte from each 4-byte word is placed at its mirrored position compared to
 * the boundary of that word.
 *
 * 4. If QUIRK_MSB_ON_THE_RIGHT and QUIRK_LITTLE_ENDIAN are both set, we do it
 *    like this:
 *
 * 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63
 * 4                       5                       6                       7
 * 0  1  2  3  4  5  6  7  8   9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
 * 0                       1                       2                       3
 *
 *
 * 5. If just QUIRK_LSW32_IS_FIRST is set, we do it like this:
 *
 * 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
 * 3                       2                       1                        0
 * 63 62 61 60 59 58 57 56 55 54 53 52 51 50 49 48 47 46 45 44 43 42 41 40 39 38 37 36 35 34 33 32
 * 7                       6                       5                        4
 *
 * In this case the 8 byte memory region is interpreted as follows: first
 * 4 bytes correspond to the least significant 4-byte word, next 4 bytes to
 * the more significant 4-byte word.
 *
 *
 * 6. If QUIRK_LSW32_IS_FIRST and QUIRK_MSB_ON_THE_RIGHT are set, we do it like
 *    this:
 *
 * 24 25 26 27 28 29 30 31 16 17 18 19 20 21 22 23  8  9 10 11 12 13 14 15  0  1  2  3  4  5  6  7
 * 3                       2                        1                       0
 * 56 57 58 59 60 61 62 63 48 49 50 51 52 53 54 55 40 41 42 43 44 45 46 47 32 33 34 35 36 37 38 39
 * 7                       6                        5                       4
 *
 *
 * 7. If QUIRK_LSW32_IS_FIRST and QUIRK_LITTLE_ENDIAN are set, it looks like
 *    this:
 *
 * 7  6  5  4  3  2  1  0  15 14 13 12 11 10  9  8 23 22 21 20 19 18 17 16 31 30 29 28 27 26 25 24
 * 0                       1                       2                       3
 * 39 38 37 36 35 34 33 32 47 46 45 44 43 42 41 40 55 54 53 52 51 50 49 48 63 62 61 60 59 58 57 56
 * 4                       5                       6                       7
 *
 *
 * 8. If QUIRK_LSW32_IS_FIRST, QUIRK_LITTLE_ENDIAN and QUIRK_MSB_ON_THE_RIGHT
 *    are set, it looks like this:
 *
 * 0  1  2  3  4  5  6  7  8   9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
 * 0                       1                       2                       3
 * 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63
 * 4                       5                       6                       7
 *
 *
 * We always think of our offsets as if there were no quirk, and we translate
 * them afterwards, before accessing the memory region.
 */

static int get_le_offset(int offset)
{
	int closest_multiple_of_4;

	closest_multiple_of_4 = (offset / 4) * 4;
	offset -= closest_multiple_of_4;
	return closest_multiple_of_4 + (3 - offset);
}

static int get_reverse_lsw32_offset(int offset, size_t len)
{
	int closest_multiple_of_4;
	int word_index;

	word_index = offset / 4;
	closest_multiple_of_4 = word_index * 4;
	offset -= closest_multiple_of_4;
	word_index = (len / 4) - word_index - 1;
	return word_index * 4 + offset;
}

static u64 bit_reverse(u64 val, unsigned int width)
{
	u64 new_val = 0;
	unsigned int bit;
	unsigned int i;

	for (i = 0; i < width; i++) {
		bit = (val & (1 << i)) != 0;
		new_val |= (bit << (width - i - 1));
	}
	return new_val;
}

static void adjust_for_msb_right_quirk(u64 *to_write, int *box_start_bit,
				       int *box_end_bit, u8 *box_mask)
{
	int box_bit_width = *box_start_bit - *box_end_bit + 1;
	int new_box_start_bit, new_box_end_bit;

	*to_write >>= *box_end_bit;
	*to_write = bit_reverse(*to_write, box_bit_width);
	*to_write <<= *box_end_bit;

	new_box_end_bit   = box_bit_width - *box_start_bit - 1;
	new_box_start_bit = box_bit_width - *box_end_bit - 1;
	*box_mask = GENMASK_ULL(new_box_start_bit, new_box_end_bit);
	*box_start_bit = new_box_start_bit;
	*box_end_bit   = new_box_end_bit;
}

/**
 * packing - Convert numbers (currently u64) between a packed and an unpacked
 *	     format. Unpacked means laid out in memory in the CPU's native
 *	     understanding of integers, while packed means anything else that
 *	     requires translation.
 *
 * @pbuf: Pointer to a buffer holding the packed value.
 * @uval: Pointer to an u64 holding the unpacked value.
 * @pstartbit: The index (in logical notation, compensated for quirks) where
 *	       the packed value starts within pbuf. Must be larger than, or
 *	       equal to, pendbit.
 * @pendbit: The index (in logical notation, compensated for quirks) where
 *	     the packed value ends within pbuf. Must be smaller than, or equal
 *	     to, pstartbit.
 * @op: If PACK, then uval will be treated as const pointer and copied (packed)
 *	into pbuf, between pstartbit and pendbit.
 *	If UNPACK, then pbuf will be treated as const pointer and the logical value
 *	between pstartbit and pendbit will be copied (unpacked) to uval.
 * @quirks: A bit mask of QUIRK_LITTLE_ENDIAN, QUIRK_LSW32_IS_FIRST and
 *	    QUIRK_MSB_ON_THE_RIGHT.
 *
 * Return: 0 on success, EINVAL or ERANGE if called incorrectly. Assuming
 * 	   correct usage, return code may be discarded.
 * 	   If op is PACK, pbuf is modified.
 * 	   If op is UNPACK, uval is modified.
 */
int packing(void *pbuf, u64 *uval, int pstartbit, int pendbit, size_t pbuflen,
	    enum packing_op op, u8 quirks)
{
	/* Number of bits for storing "uval"
	 * also width of the field to access in the pbuf
	 */
	u64 value_width;
	/* Logical byte indices corresponding to the
	 * start and end of the field.
	 */
	int plogicalfirstu8, plogicallastu8, box;

	/* pstartbit is expected to be larger than pendbit */
	if (pstartbit < pendbit)
		/* Invalid function call */
		return -EINVAL;

	value_width = pstartbit - pendbit + 1;
	if (value_width > 64)
		return -ERANGE;

	/* Check if "uval" fits in "value_width" bits.
	 * If value_width is 64, the check will fail, but any
	 * 64-bit uval will surely fit.
	 */
	if ((op == PACK) && (value_width < 64) &&
	    (*uval >= (1ull << value_width)))
		/* Cannot store "uval" inside "value_width" bits.
		 * Truncating "uval" is most certainly not desirable,
		 * so simply erroring out is appropriate.
		 */
		return -ERANGE;

	/* Initialize parameter */
	if (op == UNPACK)
		*uval = 0;

	/* Iterate through an idealistic view of the pbuf as an u64 with
	 * no quirks, u8 by u8 (aligned at u8 boundaries), from high to low
	 * logical bit significance. "box" denotes the current logical u8.
	 */
	plogicalfirstu8 = pstartbit / 8;
	plogicallastu8  = pendbit / 8;

	for (box = plogicalfirstu8; box >= plogicallastu8; box--) {
		/* Bit indices into the currently accessed 8-bit box */
		int box_start_bit, box_end_bit, box_addr;
		u8  box_mask;
		/* Corresponding bits from the unpacked u64 parameter */
		int proj_start_bit, proj_end_bit;
		u64 proj_mask;

		/* This u8 may need to be accessed in its entirety
		 * (from bit 7 to bit 0), or not, depending on the
		 * input arguments pstartbit and pendbit.
		 */
		if (box == plogicalfirstu8)
			box_start_bit = pstartbit % 8;
		else
			box_start_bit = 7;
		if (box == plogicallastu8)
			box_end_bit = pendbit % 8;
		else
			box_end_bit = 0;

		/* We have determined the box bit start and end.
		 * Now we calculate where this (masked) u8 box would fit
		 * in the unpacked (CPU-readable) u64 - the u8 box's
		 * projection onto the unpacked u64. Though the
		 * box is u8, the projection is u64 because it may fall
		 * anywhere within the unpacked u64.
		 */
		proj_start_bit = ((box * 8) + box_start_bit) - pendbit;
		proj_end_bit   = ((box * 8) + box_end_bit) - pendbit;
		proj_mask = GENMASK_ULL(proj_start_bit, proj_end_bit);
		box_mask  = GENMASK_ULL(box_start_bit, box_end_bit);

		/* Determine the offset of the u8 box inside the pbuf,
		 * adjusted for quirks. The adjusted box_addr will be used for
		 * effective addressing inside the pbuf (so it's not
		 * logical any longer).
		 */
		box_addr = pbuflen - box - 1;
		if (quirks & QUIRK_LITTLE_ENDIAN)
			box_addr = get_le_offset(box_addr);
		if (quirks & QUIRK_LSW32_IS_FIRST)
			box_addr = get_reverse_lsw32_offset(box_addr,
					pbuflen);

		if (op == UNPACK) {
			u64 pval;

			/* Read from pbuf, write to uval */
			pval = ((u8 *) pbuf)[box_addr] & box_mask;
			if (quirks & QUIRK_MSB_ON_THE_RIGHT)
				adjust_for_msb_right_quirk(&pval,
						&box_start_bit, &box_end_bit,
						&box_mask);

			pval >>= box_end_bit;
			pval <<= proj_end_bit;
			*uval &= ~proj_mask;
			*uval |= pval;
		} else {
			u64 pval;

			/* Write to pbuf, read from uval */
			pval = (*uval) & proj_mask;
			pval >>= proj_end_bit;
			if (quirks & QUIRK_MSB_ON_THE_RIGHT)
				adjust_for_msb_right_quirk(&pval,
						&box_start_bit, &box_end_bit,
						&box_mask);

			pval <<= box_end_bit;
			((u8 *) pbuf)[box_addr] &= ~box_mask;
			((u8 *) pbuf)[box_addr] |= pval;
		}
	}
	return 0;
}
EXPORT_SYMBOL(packing);

