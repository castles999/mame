// license:BSD-3-Clause
// copyright-holders:Nicola Salmoria
#include "emu.h"
#include "livingarcade_raster_command.h"
#include "taito_helper.h"

/* These scanline drawing routines, currently used by the pc080sn, tc0080vco, tc0150rod and tc0480scp devices, were lifted from Taito F3: optimise / merge ? */


void taitoic_drawscanline( bitmap_ind16 &bitmap, const rectangle &cliprect, int x, int y,
		const u16 *src, bool transparent, u32 orient, bitmap_ind8 &priority, u8 pri, u8 primask)
{
	u16 *dsti = &bitmap.pix(y, x);
	u8 *dstp = &priority.pix(y, x);
	int length = cliprect.width();
	// The helper consumes an independently generated source scanline.
	if (auto *la = livingarcade_raster_command::active)
	{
		if (x != 0 || orient != ROT0) la->status |= 16;
		else if (la->sampled_tile_row(bitmap, priority, cliprect.min_x, y, cliprect.max_x+1, (u32(primask)<<8)|pri))
			for (int xx = cliprect.min_x; xx <= cliprect.max_x; ++xx)
			{
				la->put(src[xx], 2);
				la->put(!transparent || src[xx] < 0x7fff ? 1 : 0, 1);
			}
	}

	src += cliprect.min_x;
	dsti += cliprect.min_x;
	dstp += cliprect.min_x;
	if (transparent)
	{
		while (length--)
		{
			u32 spixel = *src++;

			if (spixel < 0x7fff)
			{
				*dsti = spixel;
				*dstp = (*dstp & primask) | pri;
			}

			dsti++;
			dstp++;
		}
	}
	else    /* Not transparent case */
	{
		while (length--)
		{
			*dsti++ = *src++;
			*dstp = (*dstp & primask) | pri;
			dstp++;
		}
	}
}
