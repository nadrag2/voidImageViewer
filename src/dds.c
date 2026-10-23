//
// Copyright 2026 voidImageViewer contributors
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// dds layer
//
// Decodes the first surface (first mip level, first cube face / array slice / depth slice) of a DDS file to RGBA.
//
// Supported:
//   BC1/BC2/BC3 (DXT1-DXT5, including premultiplied DXT2/DXT4), BC4, BC5, BC6H, BC7 (FourCC and DX10 header)
//   uncompressed RGB / RGBA / luminance / alpha formats described by bit masks
//   16-bit unorm and 16/32-bit float formats (floats are clamped to 0..1 and sRGB encoded)
//
// This file does not use the C runtime or floating point math.

#include "viv.h"

// bcdec is a single header decoder for BC1-BC7 with no dependencies.
#define BCDEC_BC4BC5_PRECISE
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

// BC5 is almost always a tangent space normal map.
// reconstruct the blue channel (Z) from red and green (X and Y) so they look right.
#define _DDS_BC5_RECONSTRUCT_Z	1

// maximum width or height.
#define _DDS_MAX_DIMENSION		16384

#define _DDS_MAGIC				0x20534444 // "DDS "

#define _DDS_FOURCC(a,b,c,d)	((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))

#define _DDPF_ALPHAPIXELS		0x00000001
#define _DDPF_ALPHA				0x00000002
#define _DDPF_FOURCC			0x00000004
#define _DDPF_RGB				0x00000040
#define _DDPF_LUMINANCE			0x00020000

// file layout.
#define _DDS_OFFSET_HEADER_SIZE	4
#define _DDS_OFFSET_HEIGHT		12
#define _DDS_OFFSET_WIDTH		16
#define _DDS_OFFSET_PF_SIZE		76
#define _DDS_OFFSET_PF_FLAGS	80
#define _DDS_OFFSET_PF_FOURCC	84
#define _DDS_OFFSET_PF_BITCOUNT	88
#define _DDS_OFFSET_PF_RMASK	92
#define _DDS_OFFSET_PF_GMASK	96
#define _DDS_OFFSET_PF_BMASK	100
#define _DDS_OFFSET_PF_AMASK	104
#define _DDS_OFFSET_DX10		128
#define _DDS_SIZE_BASE			128
#define _DDS_SIZE_DX10			148

enum
{
	_DDS_TYPE_NONE = 0,
	_DDS_TYPE_BC1,
	_DDS_TYPE_BC2,
	_DDS_TYPE_BC2_PREMULTIPLIED,
	_DDS_TYPE_BC3,
	_DDS_TYPE_BC3_PREMULTIPLIED,
	_DDS_TYPE_BC4,
	_DDS_TYPE_BC4_SIGNED,
	_DDS_TYPE_BC5,
	_DDS_TYPE_BC5_SIGNED,
	_DDS_TYPE_BC6H,
	_DDS_TYPE_BC6H_SIGNED,
	_DDS_TYPE_BC7,
	_DDS_TYPE_MASKED,		// uncompressed, described by bit masks.
	_DDS_TYPE_UNORM16,		// 1-4 channels of 16 bit unsigned normalized.
	_DDS_TYPE_FLOAT,		// 1-4 channels of 16 or 32 bit float.
};

typedef struct _dds_format_s
{
	int type;
	int bitcount;			// bits per pixel for MASKED.
	int channels;			// UNORM16 and FLOAT.
	int component_size;		// bytes per channel for UNORM16 and FLOAT.
	int gray;				// MASKED: copy red to green and blue.
	int alpha_only;			// MASKED: only an alpha channel.
	DWORD rmask;
	DWORD gmask;
	DWORD bmask;
	DWORD amask;
	
}_dds_format_t;

typedef struct _dds_mask_s
{
	DWORD mask;
	int shift;
	int bits;
	
}_dds_mask_t;

static DWORD _dds_read32(const BYTE *p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static DWORD _dds_read16(const BYTE *p)
{
	return (DWORD)p[0] | ((DWORD)p[1] << 8);
}

static void _dds_format_init(_dds_format_t *f,int type)
{
	f->type = type;
	f->bitcount = 0;
	f->channels = 0;
	f->component_size = 0;
	f->gray = 0;
	f->alpha_only = 0;
	f->rmask = 0;
	f->gmask = 0;
	f->bmask = 0;
	f->amask = 0;
}

static void _dds_format_set_masked(_dds_format_t *f,int bitcount,DWORD rmask,DWORD gmask,DWORD bmask,DWORD amask)
{
	_dds_format_init(f,_DDS_TYPE_MASKED);
	
	f->bitcount = bitcount;
	f->rmask = rmask;
	f->gmask = gmask;
	f->bmask = bmask;
	f->amask = amask;
}

static void _dds_format_set_components(_dds_format_t *f,int type,int channels,int component_size)
{
	_dds_format_init(f,type);
	
	f->channels = channels;
	f->component_size = component_size;
}

// returns 1 if the format has an alpha channel.
static int _dds_format_has_alpha(const _dds_format_t *f)
{
	switch(f->type)
	{
		case _DDS_TYPE_BC1:
		case _DDS_TYPE_BC2:
		case _DDS_TYPE_BC2_PREMULTIPLIED:
		case _DDS_TYPE_BC3:
		case _DDS_TYPE_BC3_PREMULTIPLIED:
		case _DDS_TYPE_BC7:
			return 1;
			
		case _DDS_TYPE_MASKED:
			return (f->amask != 0) || f->alpha_only;
			
		case _DDS_TYPE_UNORM16:
		case _DDS_TYPE_FLOAT:
			return f->channels == 4;
	}
	
	return 0;
}

// map a DXGI_FORMAT to a format. returns 1 on success.
static int _dds_format_from_dxgi(_dds_format_t *f,DWORD dxgi)
{
	switch(dxgi)
	{
		// BC1
		case 70: case 71: case 72:
			_dds_format_init(f,_DDS_TYPE_BC1);
			return 1;
			
		// BC2
		case 73: case 74: case 75:
			_dds_format_init(f,_DDS_TYPE_BC2);
			return 1;
			
		// BC3
		case 76: case 77: case 78:
			_dds_format_init(f,_DDS_TYPE_BC3);
			return 1;
			
		// BC4
		case 79: case 80:
			_dds_format_init(f,_DDS_TYPE_BC4);
			return 1;
			
		case 81:
			_dds_format_init(f,_DDS_TYPE_BC4_SIGNED);
			return 1;
			
		// BC5
		case 82: case 83:
			_dds_format_init(f,_DDS_TYPE_BC5);
			return 1;
			
		case 84:
			_dds_format_init(f,_DDS_TYPE_BC5_SIGNED);
			return 1;
			
		// BC6H
		case 94: case 95:
			_dds_format_init(f,_DDS_TYPE_BC6H);
			return 1;
			
		case 96:
			_dds_format_init(f,_DDS_TYPE_BC6H_SIGNED);
			return 1;
			
		// BC7
		case 97: case 98: case 99:
			_dds_format_init(f,_DDS_TYPE_BC7);
			return 1;
			
		// float
		case 2: // R32G32B32A32_FLOAT
			_dds_format_set_components(f,_DDS_TYPE_FLOAT,4,4);
			return 1;
			
		case 6: // R32G32B32_FLOAT
			_dds_format_set_components(f,_DDS_TYPE_FLOAT,3,4);
			return 1;
			
		case 16: // R32G32_FLOAT
			_dds_format_set_components(f,_DDS_TYPE_FLOAT,2,4);
			return 1;
			
		case 41: // R32_FLOAT
			_dds_format_set_components(f,_DDS_TYPE_FLOAT,1,4);
			return 1;
			
		case 10: // R16G16B16A16_FLOAT
			_dds_format_set_components(f,_DDS_TYPE_FLOAT,4,2);
			return 1;
			
		case 34: // R16G16_FLOAT
			_dds_format_set_components(f,_DDS_TYPE_FLOAT,2,2);
			return 1;
			
		case 54: // R16_FLOAT
			_dds_format_set_components(f,_DDS_TYPE_FLOAT,1,2);
			return 1;
			
		// unorm 16
		case 11: // R16G16B16A16_UNORM
			_dds_format_set_components(f,_DDS_TYPE_UNORM16,4,2);
			return 1;
			
		case 35: // R16G16_UNORM
			_dds_format_set_components(f,_DDS_TYPE_UNORM16,2,2);
			return 1;
			
		case 56: // R16_UNORM
			_dds_format_set_components(f,_DDS_TYPE_UNORM16,1,2);
			return 1;
			
		// 8 bit
		case 27: case 28: case 29: // R8G8B8A8
			_dds_format_set_masked(f,32,0x000000ff,0x0000ff00,0x00ff0000,0xff000000);
			return 1;
			
		case 87: case 90: case 91: // B8G8R8A8
			_dds_format_set_masked(f,32,0x00ff0000,0x0000ff00,0x000000ff,0xff000000);
			return 1;
			
		case 88: case 92: case 93: // B8G8R8X8
			_dds_format_set_masked(f,32,0x00ff0000,0x0000ff00,0x000000ff,0);
			return 1;
			
		case 24: // R10G10B10A2_UNORM
			_dds_format_set_masked(f,32,0x000003ff,0x000ffc00,0x3ff00000,0xc0000000);
			return 1;
			
		case 49: // R8G8_UNORM
			_dds_format_set_masked(f,16,0x00ff,0xff00,0,0);
			return 1;
			
		case 61: // R8_UNORM
			_dds_format_set_masked(f,8,0xff,0,0,0);
			f->gray = 1;
			return 1;
			
		case 65: // A8_UNORM
			_dds_format_set_masked(f,8,0,0,0,0xff);
			f->alpha_only = 1;
			return 1;
			
		case 85: // B5G6R5_UNORM
			_dds_format_set_masked(f,16,0xf800,0x07e0,0x001f,0);
			return 1;
			
		case 86: // B5G5R5A1_UNORM
			_dds_format_set_masked(f,16,0x7c00,0x03e0,0x001f,0x8000);
			return 1;
			
		case 115: // B4G4R4A4_UNORM
			_dds_format_set_masked(f,16,0x0f00,0x00f0,0x000f,0xf000);
			return 1;
	}
	
	return 0;
}

// returns 1 if the mask only uses bits that exist in a pixel of the given size.
static int _dds_mask_fits(DWORD mask,DWORD bitcount)
{
	if (bitcount >= 32)
	{
		return 1;
	}
	
	return (mask >> bitcount) == 0;
}

// map the legacy pixel format to a format.
// returns 1 on success, 2 if there is a DX10 header, 0 on error.
static int _dds_format_from_legacy(_dds_format_t *f,const BYTE *header)
{
	DWORD flags;
	DWORD fourcc;
	DWORD bitcount;
	DWORD rmask;
	DWORD gmask;
	DWORD bmask;
	DWORD amask;
	
	flags = _dds_read32(header + _DDS_OFFSET_PF_FLAGS);
	fourcc = _dds_read32(header + _DDS_OFFSET_PF_FOURCC);
	bitcount = _dds_read32(header + _DDS_OFFSET_PF_BITCOUNT);
	rmask = _dds_read32(header + _DDS_OFFSET_PF_RMASK);
	gmask = _dds_read32(header + _DDS_OFFSET_PF_GMASK);
	bmask = _dds_read32(header + _DDS_OFFSET_PF_BMASK);
	amask = _dds_read32(header + _DDS_OFFSET_PF_AMASK);
	
	if (flags & _DDPF_FOURCC)
	{
		if (fourcc == _DDS_FOURCC('D','X','1','0'))
		{
			return 2;
		}
		
		if (fourcc == _DDS_FOURCC('D','X','T','1'))
		{
			_dds_format_init(f,_DDS_TYPE_BC1);
			return 1;
		}
		
		if (fourcc == _DDS_FOURCC('D','X','T','2'))
		{
			_dds_format_init(f,_DDS_TYPE_BC2_PREMULTIPLIED);
			return 1;
		}
		
		if (fourcc == _DDS_FOURCC('D','X','T','3'))
		{
			_dds_format_init(f,_DDS_TYPE_BC2);
			return 1;
		}
		
		if (fourcc == _DDS_FOURCC('D','X','T','4'))
		{
			_dds_format_init(f,_DDS_TYPE_BC3_PREMULTIPLIED);
			return 1;
		}
		
		if (fourcc == _DDS_FOURCC('D','X','T','5'))
		{
			_dds_format_init(f,_DDS_TYPE_BC3);
			return 1;
		}
		
		if ((fourcc == _DDS_FOURCC('A','T','I','1')) || (fourcc == _DDS_FOURCC('B','C','4','U')))
		{
			_dds_format_init(f,_DDS_TYPE_BC4);
			return 1;
		}
		
		if (fourcc == _DDS_FOURCC('B','C','4','S'))
		{
			_dds_format_init(f,_DDS_TYPE_BC4_SIGNED);
			return 1;
		}
		
		if ((fourcc == _DDS_FOURCC('A','T','I','2')) || (fourcc == _DDS_FOURCC('B','C','5','U')))
		{
			_dds_format_init(f,_DDS_TYPE_BC5);
			return 1;
		}
		
		if (fourcc == _DDS_FOURCC('B','C','5','S'))
		{
			_dds_format_init(f,_DDS_TYPE_BC5_SIGNED);
			return 1;
		}
		
		// D3DFORMAT values stored in the FourCC field.
		switch(fourcc)
		{
			case 36: // D3DFMT_A16B16G16R16
				_dds_format_set_components(f,_DDS_TYPE_UNORM16,4,2);
				return 1;
				
			case 111: // D3DFMT_R16F
				_dds_format_set_components(f,_DDS_TYPE_FLOAT,1,2);
				return 1;
				
			case 112: // D3DFMT_G16R16F
				_dds_format_set_components(f,_DDS_TYPE_FLOAT,2,2);
				return 1;
				
			case 113: // D3DFMT_A16B16G16R16F
				_dds_format_set_components(f,_DDS_TYPE_FLOAT,4,2);
				return 1;
				
			case 114: // D3DFMT_R32F
				_dds_format_set_components(f,_DDS_TYPE_FLOAT,1,4);
				return 1;
				
			case 115: // D3DFMT_G32R32F
				_dds_format_set_components(f,_DDS_TYPE_FLOAT,2,4);
				return 1;
				
			case 116: // D3DFMT_A32B32G32R32F
				_dds_format_set_components(f,_DDS_TYPE_FLOAT,4,4);
				return 1;
		}
		
		// unsupported (YUV, packed, etc..)
		return 0;
	}
	
	if (flags & _DDPF_RGB)
	{
		if ((bitcount != 8) && (bitcount != 16) && (bitcount != 24) && (bitcount != 32))
		{
			return 0;
		}
		
		if (!(flags & _DDPF_ALPHAPIXELS))
		{
			amask = 0;
		}
		
		// some writers write masks that don't fit in the pixel. use the defaults for the bit count.
		if ((!_dds_mask_fits(rmask,bitcount)) || (!_dds_mask_fits(gmask,bitcount)) || (!_dds_mask_fits(bmask,bitcount)) || (!_dds_mask_fits(amask,bitcount)))
		{
			rmask = 0;
			gmask = 0;
			bmask = 0;
			amask = (bitcount == 32) ? amask & 0xff000000 : 0;
		}
		
		// some writers leave the masks empty.
		if ((rmask == 0) && (gmask == 0) && (bmask == 0))
		{
			switch(bitcount)
			{
				case 32:
					rmask = 0x00ff0000;
					gmask = 0x0000ff00;
					bmask = 0x000000ff;
					break;
					
				case 24:
					rmask = 0x00ff0000;
					gmask = 0x0000ff00;
					bmask = 0x000000ff;
					break;
					
				case 16:
					rmask = 0xf800;
					gmask = 0x07e0;
					bmask = 0x001f;
					break;
					
				default:
					return 0;
			}
		}
		
		_dds_format_set_masked(f,(int)bitcount,rmask,gmask,bmask,amask);
		return 1;
	}
	
	if (flags & _DDPF_LUMINANCE)
	{
		if ((bitcount != 8) && (bitcount != 16))
		{
			return 0;
		}
		
		if (!(flags & _DDPF_ALPHAPIXELS))
		{
			amask = 0;
		}
		
		// some writers write masks that don't fit in the pixel.
		if ((rmask == 0) || (!_dds_mask_fits(rmask,bitcount)) || (!_dds_mask_fits(amask,bitcount)) || (rmask & amask))
		{
			if (amask && (bitcount == 16))
			{
				// luminance + alpha.
				rmask = 0x00ff;
				amask = 0xff00;
			}
			else
			{
				rmask = (bitcount == 8) ? 0xff : 0xffff;
				amask = 0;
			}
		}
		
		_dds_format_set_masked(f,(int)bitcount,rmask,0,0,amask);
		f->gray = 1;
		return 1;
	}
	
	if (flags & _DDPF_ALPHA)
	{
		if ((bitcount != 8) && (bitcount != 16))
		{
			return 0;
		}
		
		if ((amask == 0) || (!_dds_mask_fits(amask,bitcount)))
		{
			amask = (bitcount == 8) ? 0xff : 0xffff;
		}
		
		_dds_format_set_masked(f,(int)bitcount,0,0,0,amask);
		f->alpha_only = 1;
		return 1;
	}
	
	return 0;
}

static void _dds_mask_init(_dds_mask_t *m,DWORD mask)
{
	m->mask = mask;
	m->shift = 0;
	m->bits = 0;
	
	if (mask)
	{
		while (!(mask & 1))
		{
			mask >>= 1;
			m->shift++;
		}
		
		while (mask & 1)
		{
			mask >>= 1;
			m->bits++;
		}
	}
}

// extract a channel and scale it to 8 bits.
static DWORD _dds_mask_get(const _dds_mask_t *m,DWORD pixel)
{
	DWORD v;
	
	if (!m->bits)
	{
		return 0;
	}
	
	v = (pixel & m->mask) >> m->shift;
	
	if (m->bits == 8)
	{
		return v;
	}
	
	if (m->bits > 8)
	{
		return v >> (m->bits - 8);
	}
	
	{
		DWORD max;
		
		max = (1 << m->bits) - 1;
		
		return (v * 255 + (max / 2)) / max;
	}
}

// 0..4095 linear to 0..255 sRGB
static const BYTE _dds_srgb_table[4096] = 
{
	0,1,2,2,3,4,5,6,6,7,8,9,10,10,11,12,
	13,13,14,15,15,16,16,17,18,18,19,19,20,20,21,21,
	22,22,23,23,23,24,24,25,25,25,26,26,27,27,27,28,
	28,29,29,29,30,30,30,31,31,31,32,32,32,33,33,33,
	34,34,34,34,35,35,35,36,36,36,37,37,37,37,38,38,
	38,38,39,39,39,40,40,40,40,41,41,41,41,42,42,42,
	42,43,43,43,43,43,44,44,44,44,45,45,45,45,46,46,
	46,46,46,47,47,47,47,48,48,48,48,48,49,49,49,49,
	49,50,50,50,50,50,51,51,51,51,51,52,52,52,52,52,
	53,53,53,53,53,54,54,54,54,54,55,55,55,55,55,55,
	56,56,56,56,56,57,57,57,57,57,57,58,58,58,58,58,
	58,59,59,59,59,59,59,60,60,60,60,60,60,61,61,61,
	61,61,61,62,62,62,62,62,62,63,63,63,63,63,63,64,
	64,64,64,64,64,64,65,65,65,65,65,65,66,66,66,66,
	66,66,66,67,67,67,67,67,67,67,68,68,68,68,68,68,
	68,69,69,69,69,69,69,69,70,70,70,70,70,70,70,71,
	71,71,71,71,71,71,72,72,72,72,72,72,72,72,73,73,
	73,73,73,73,73,74,74,74,74,74,74,74,74,75,75,75,
	75,75,75,75,75,76,76,76,76,76,76,76,77,77,77,77,
	77,77,77,77,78,78,78,78,78,78,78,78,78,79,79,79,
	79,79,79,79,79,80,80,80,80,80,80,80,80,81,81,81,
	81,81,81,81,81,81,82,82,82,82,82,82,82,82,83,83,
	83,83,83,83,83,83,83,84,84,84,84,84,84,84,84,84,
	85,85,85,85,85,85,85,85,85,86,86,86,86,86,86,86,
	86,86,87,87,87,87,87,87,87,87,87,88,88,88,88,88,
	88,88,88,88,88,89,89,89,89,89,89,89,89,89,90,90,
	90,90,90,90,90,90,90,90,91,91,91,91,91,91,91,91,
	91,91,92,92,92,92,92,92,92,92,92,92,93,93,93,93,
	93,93,93,93,93,93,94,94,94,94,94,94,94,94,94,94,
	95,95,95,95,95,95,95,95,95,95,96,96,96,96,96,96,
	96,96,96,96,96,97,97,97,97,97,97,97,97,97,97,98,
	98,98,98,98,98,98,98,98,98,98,99,99,99,99,99,99,
	99,99,99,99,99,100,100,100,100,100,100,100,100,100,100,100,
	101,101,101,101,101,101,101,101,101,101,101,102,102,102,102,102,
	102,102,102,102,102,102,103,103,103,103,103,103,103,103,103,103,
	103,103,104,104,104,104,104,104,104,104,104,104,104,105,105,105,
	105,105,105,105,105,105,105,105,105,106,106,106,106,106,106,106,
	106,106,106,106,106,107,107,107,107,107,107,107,107,107,107,107,
	107,108,108,108,108,108,108,108,108,108,108,108,108,109,109,109,
	109,109,109,109,109,109,109,109,109,110,110,110,110,110,110,110,
	110,110,110,110,110,111,111,111,111,111,111,111,111,111,111,111,
	111,111,112,112,112,112,112,112,112,112,112,112,112,112,113,113,
	113,113,113,113,113,113,113,113,113,113,113,114,114,114,114,114,
	114,114,114,114,114,114,114,114,115,115,115,115,115,115,115,115,
	115,115,115,115,115,116,116,116,116,116,116,116,116,116,116,116,
	116,116,117,117,117,117,117,117,117,117,117,117,117,117,117,117,
	118,118,118,118,118,118,118,118,118,118,118,118,118,119,119,119,
	119,119,119,119,119,119,119,119,119,119,119,120,120,120,120,120,
	120,120,120,120,120,120,120,120,120,121,121,121,121,121,121,121,
	121,121,121,121,121,121,122,122,122,122,122,122,122,122,122,122,
	122,122,122,122,122,123,123,123,123,123,123,123,123,123,123,123,
	123,123,123,124,124,124,124,124,124,124,124,124,124,124,124,124,
	124,125,125,125,125,125,125,125,125,125,125,125,125,125,125,125,
	126,126,126,126,126,126,126,126,126,126,126,126,126,126,127,127,
	127,127,127,127,127,127,127,127,127,127,127,127,127,128,128,128,
	128,128,128,128,128,128,128,128,128,128,128,128,129,129,129,129,
	129,129,129,129,129,129,129,129,129,129,129,130,130,130,130,130,
	130,130,130,130,130,130,130,130,130,130,131,131,131,131,131,131,
	131,131,131,131,131,131,131,131,131,131,132,132,132,132,132,132,
	132,132,132,132,132,132,132,132,132,133,133,133,133,133,133,133,
	133,133,133,133,133,133,133,133,133,134,134,134,134,134,134,134,
	134,134,134,134,134,134,134,134,134,135,135,135,135,135,135,135,
	135,135,135,135,135,135,135,135,135,136,136,136,136,136,136,136,
	136,136,136,136,136,136,136,136,136,137,137,137,137,137,137,137,
	137,137,137,137,137,137,137,137,137,138,138,138,138,138,138,138,
	138,138,138,138,138,138,138,138,138,139,139,139,139,139,139,139,
	139,139,139,139,139,139,139,139,139,139,140,140,140,140,140,140,
	140,140,140,140,140,140,140,140,140,140,140,141,141,141,141,141,
	141,141,141,141,141,141,141,141,141,141,141,141,142,142,142,142,
	142,142,142,142,142,142,142,142,142,142,142,142,142,143,143,143,
	143,143,143,143,143,143,143,143,143,143,143,143,143,143,144,144,
	144,144,144,144,144,144,144,144,144,144,144,144,144,144,144,145,
	145,145,145,145,145,145,145,145,145,145,145,145,145,145,145,145,
	145,146,146,146,146,146,146,146,146,146,146,146,146,146,146,146,
	146,146,147,147,147,147,147,147,147,147,147,147,147,147,147,147,
	147,147,147,147,148,148,148,148,148,148,148,148,148,148,148,148,
	148,148,148,148,148,148,149,149,149,149,149,149,149,149,149,149,
	149,149,149,149,149,149,149,149,150,150,150,150,150,150,150,150,
	150,150,150,150,150,150,150,150,150,150,150,151,151,151,151,151,
	151,151,151,151,151,151,151,151,151,151,151,151,151,152,152,152,
	152,152,152,152,152,152,152,152,152,152,152,152,152,152,152,152,
	153,153,153,153,153,153,153,153,153,153,153,153,153,153,153,153,
	153,153,154,154,154,154,154,154,154,154,154,154,154,154,154,154,
	154,154,154,154,154,155,155,155,155,155,155,155,155,155,155,155,
	155,155,155,155,155,155,155,155,156,156,156,156,156,156,156,156,
	156,156,156,156,156,156,156,156,156,156,156,156,157,157,157,157,
	157,157,157,157,157,157,157,157,157,157,157,157,157,157,157,158,
	158,158,158,158,158,158,158,158,158,158,158,158,158,158,158,158,
	158,158,159,159,159,159,159,159,159,159,159,159,159,159,159,159,
	159,159,159,159,159,159,160,160,160,160,160,160,160,160,160,160,
	160,160,160,160,160,160,160,160,160,160,161,161,161,161,161,161,
	161,161,161,161,161,161,161,161,161,161,161,161,161,161,162,162,
	162,162,162,162,162,162,162,162,162,162,162,162,162,162,162,162,
	162,162,163,163,163,163,163,163,163,163,163,163,163,163,163,163,
	163,163,163,163,163,163,164,164,164,164,164,164,164,164,164,164,
	164,164,164,164,164,164,164,164,164,164,164,165,165,165,165,165,
	165,165,165,165,165,165,165,165,165,165,165,165,165,165,165,165,
	166,166,166,166,166,166,166,166,166,166,166,166,166,166,166,166,
	166,166,166,166,167,167,167,167,167,167,167,167,167,167,167,167,
	167,167,167,167,167,167,167,167,167,168,168,168,168,168,168,168,
	168,168,168,168,168,168,168,168,168,168,168,168,168,168,168,169,
	169,169,169,169,169,169,169,169,169,169,169,169,169,169,169,169,
	169,169,169,169,170,170,170,170,170,170,170,170,170,170,170,170,
	170,170,170,170,170,170,170,170,170,171,171,171,171,171,171,171,
	171,171,171,171,171,171,171,171,171,171,171,171,171,171,171,172,
	172,172,172,172,172,172,172,172,172,172,172,172,172,172,172,172,
	172,172,172,172,172,173,173,173,173,173,173,173,173,173,173,173,
	173,173,173,173,173,173,173,173,173,173,173,174,174,174,174,174,
	174,174,174,174,174,174,174,174,174,174,174,174,174,174,174,174,
	174,175,175,175,175,175,175,175,175,175,175,175,175,175,175,175,
	175,175,175,175,175,175,175,176,176,176,176,176,176,176,176,176,
	176,176,176,176,176,176,176,176,176,176,176,176,176,176,177,177,
	177,177,177,177,177,177,177,177,177,177,177,177,177,177,177,177,
	177,177,177,177,178,178,178,178,178,178,178,178,178,178,178,178,
	178,178,178,178,178,178,178,178,178,178,178,179,179,179,179,179,
	179,179,179,179,179,179,179,179,179,179,179,179,179,179,179,179,
	179,179,180,180,180,180,180,180,180,180,180,180,180,180,180,180,
	180,180,180,180,180,180,180,180,180,181,181,181,181,181,181,181,
	181,181,181,181,181,181,181,181,181,181,181,181,181,181,181,181,
	182,182,182,182,182,182,182,182,182,182,182,182,182,182,182,182,
	182,182,182,182,182,182,182,182,183,183,183,183,183,183,183,183,
	183,183,183,183,183,183,183,183,183,183,183,183,183,183,183,184,
	184,184,184,184,184,184,184,184,184,184,184,184,184,184,184,184,
	184,184,184,184,184,184,184,185,185,185,185,185,185,185,185,185,
	185,185,185,185,185,185,185,185,185,185,185,185,185,185,185,186,
	186,186,186,186,186,186,186,186,186,186,186,186,186,186,186,186,
	186,186,186,186,186,186,186,187,187,187,187,187,187,187,187,187,
	187,187,187,187,187,187,187,187,187,187,187,187,187,187,187,187,
	188,188,188,188,188,188,188,188,188,188,188,188,188,188,188,188,
	188,188,188,188,188,188,188,188,189,189,189,189,189,189,189,189,
	189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,189,
	189,190,190,190,190,190,190,190,190,190,190,190,190,190,190,190,
	190,190,190,190,190,190,190,190,190,190,191,191,191,191,191,191,
	191,191,191,191,191,191,191,191,191,191,191,191,191,191,191,191,
	191,191,192,192,192,192,192,192,192,192,192,192,192,192,192,192,
	192,192,192,192,192,192,192,192,192,192,192,192,193,193,193,193,
	193,193,193,193,193,193,193,193,193,193,193,193,193,193,193,193,
	193,193,193,193,193,194,194,194,194,194,194,194,194,194,194,194,
	194,194,194,194,194,194,194,194,194,194,194,194,194,194,195,195,
	195,195,195,195,195,195,195,195,195,195,195,195,195,195,195,195,
	195,195,195,195,195,195,195,195,196,196,196,196,196,196,196,196,
	196,196,196,196,196,196,196,196,196,196,196,196,196,196,196,196,
	196,196,197,197,197,197,197,197,197,197,197,197,197,197,197,197,
	197,197,197,197,197,197,197,197,197,197,197,197,198,198,198,198,
	198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,198,
	198,198,198,198,198,198,199,199,199,199,199,199,199,199,199,199,
	199,199,199,199,199,199,199,199,199,199,199,199,199,199,199,199,
	200,200,200,200,200,200,200,200,200,200,200,200,200,200,200,200,
	200,200,200,200,200,200,200,200,200,200,200,201,201,201,201,201,
	201,201,201,201,201,201,201,201,201,201,201,201,201,201,201,201,
	201,201,201,201,201,201,202,202,202,202,202,202,202,202,202,202,
	202,202,202,202,202,202,202,202,202,202,202,202,202,202,202,202,
	202,203,203,203,203,203,203,203,203,203,203,203,203,203,203,203,
	203,203,203,203,203,203,203,203,203,203,203,203,204,204,204,204,
	204,204,204,204,204,204,204,204,204,204,204,204,204,204,204,204,
	204,204,204,204,204,204,204,205,205,205,205,205,205,205,205,205,
	205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,
	205,205,206,206,206,206,206,206,206,206,206,206,206,206,206,206,
	206,206,206,206,206,206,206,206,206,206,206,206,206,206,207,207,
	207,207,207,207,207,207,207,207,207,207,207,207,207,207,207,207,
	207,207,207,207,207,207,207,207,207,207,208,208,208,208,208,208,
	208,208,208,208,208,208,208,208,208,208,208,208,208,208,208,208,
	208,208,208,208,208,209,209,209,209,209,209,209,209,209,209,209,
	209,209,209,209,209,209,209,209,209,209,209,209,209,209,209,209,
	209,209,210,210,210,210,210,210,210,210,210,210,210,210,210,210,
	210,210,210,210,210,210,210,210,210,210,210,210,210,210,211,211,
	211,211,211,211,211,211,211,211,211,211,211,211,211,211,211,211,
	211,211,211,211,211,211,211,211,211,211,212,212,212,212,212,212,
	212,212,212,212,212,212,212,212,212,212,212,212,212,212,212,212,
	212,212,212,212,212,212,212,213,213,213,213,213,213,213,213,213,
	213,213,213,213,213,213,213,213,213,213,213,213,213,213,213,213,
	213,213,213,213,214,214,214,214,214,214,214,214,214,214,214,214,
	214,214,214,214,214,214,214,214,214,214,214,214,214,214,214,214,
	214,215,215,215,215,215,215,215,215,215,215,215,215,215,215,215,
	215,215,215,215,215,215,215,215,215,215,215,215,215,215,216,216,
	216,216,216,216,216,216,216,216,216,216,216,216,216,216,216,216,
	216,216,216,216,216,216,216,216,216,216,216,217,217,217,217,217,
	217,217,217,217,217,217,217,217,217,217,217,217,217,217,217,217,
	217,217,217,217,217,217,217,217,217,218,218,218,218,218,218,218,
	218,218,218,218,218,218,218,218,218,218,218,218,218,218,218,218,
	218,218,218,218,218,218,219,219,219,219,219,219,219,219,219,219,
	219,219,219,219,219,219,219,219,219,219,219,219,219,219,219,219,
	219,219,219,219,220,220,220,220,220,220,220,220,220,220,220,220,
	220,220,220,220,220,220,220,220,220,220,220,220,220,220,220,220,
	220,220,221,221,221,221,221,221,221,221,221,221,221,221,221,221,
	221,221,221,221,221,221,221,221,221,221,221,221,221,221,221,221,
	221,222,222,222,222,222,222,222,222,222,222,222,222,222,222,222,
	222,222,222,222,222,222,222,222,222,222,222,222,222,222,222,223,
	223,223,223,223,223,223,223,223,223,223,223,223,223,223,223,223,
	223,223,223,223,223,223,223,223,223,223,223,223,223,223,224,224,
	224,224,224,224,224,224,224,224,224,224,224,224,224,224,224,224,
	224,224,224,224,224,224,224,224,224,224,224,224,225,225,225,225,
	225,225,225,225,225,225,225,225,225,225,225,225,225,225,225,225,
	225,225,225,225,225,225,225,225,225,225,225,226,226,226,226,226,
	226,226,226,226,226,226,226,226,226,226,226,226,226,226,226,226,
	226,226,226,226,226,226,226,226,226,226,227,227,227,227,227,227,
	227,227,227,227,227,227,227,227,227,227,227,227,227,227,227,227,
	227,227,227,227,227,227,227,227,227,227,228,228,228,228,228,228,
	228,228,228,228,228,228,228,228,228,228,228,228,228,228,228,228,
	228,228,228,228,228,228,228,228,228,229,229,229,229,229,229,229,
	229,229,229,229,229,229,229,229,229,229,229,229,229,229,229,229,
	229,229,229,229,229,229,229,229,229,230,230,230,230,230,230,230,
	230,230,230,230,230,230,230,230,230,230,230,230,230,230,230,230,
	230,230,230,230,230,230,230,230,230,231,231,231,231,231,231,231,
	231,231,231,231,231,231,231,231,231,231,231,231,231,231,231,231,
	231,231,231,231,231,231,231,231,231,232,232,232,232,232,232,232,
	232,232,232,232,232,232,232,232,232,232,232,232,232,232,232,232,
	232,232,232,232,232,232,232,232,232,233,233,233,233,233,233,233,
	233,233,233,233,233,233,233,233,233,233,233,233,233,233,233,233,
	233,233,233,233,233,233,233,233,233,233,234,234,234,234,234,234,
	234,234,234,234,234,234,234,234,234,234,234,234,234,234,234,234,
	234,234,234,234,234,234,234,234,234,234,235,235,235,235,235,235,
	235,235,235,235,235,235,235,235,235,235,235,235,235,235,235,235,
	235,235,235,235,235,235,235,235,235,235,235,236,236,236,236,236,
	236,236,236,236,236,236,236,236,236,236,236,236,236,236,236,236,
	236,236,236,236,236,236,236,236,236,236,236,236,237,237,237,237,
	237,237,237,237,237,237,237,237,237,237,237,237,237,237,237,237,
	237,237,237,237,237,237,237,237,237,237,237,237,237,238,238,238,
	238,238,238,238,238,238,238,238,238,238,238,238,238,238,238,238,
	238,238,238,238,238,238,238,238,238,238,238,238,238,238,239,239,
	239,239,239,239,239,239,239,239,239,239,239,239,239,239,239,239,
	239,239,239,239,239,239,239,239,239,239,239,239,239,239,239,239,
	240,240,240,240,240,240,240,240,240,240,240,240,240,240,240,240,
	240,240,240,240,240,240,240,240,240,240,240,240,240,240,240,240,
	240,240,241,241,241,241,241,241,241,241,241,241,241,241,241,241,
	241,241,241,241,241,241,241,241,241,241,241,241,241,241,241,241,
	241,241,241,241,242,242,242,242,242,242,242,242,242,242,242,242,
	242,242,242,242,242,242,242,242,242,242,242,242,242,242,242,242,
	242,242,242,242,242,242,243,243,243,243,243,243,243,243,243,243,
	243,243,243,243,243,243,243,243,243,243,243,243,243,243,243,243,
	243,243,243,243,243,243,243,243,244,244,244,244,244,244,244,244,
	244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,244,
	244,244,244,244,244,244,244,244,244,244,245,245,245,245,245,245,
	245,245,245,245,245,245,245,245,245,245,245,245,245,245,245,245,
	245,245,245,245,245,245,245,245,245,245,245,245,245,246,246,246,
	246,246,246,246,246,246,246,246,246,246,246,246,246,246,246,246,
	246,246,246,246,246,246,246,246,246,246,246,246,246,246,246,246,
	247,247,247,247,247,247,247,247,247,247,247,247,247,247,247,247,
	247,247,247,247,247,247,247,247,247,247,247,247,247,247,247,247,
	247,247,247,248,248,248,248,248,248,248,248,248,248,248,248,248,
	248,248,248,248,248,248,248,248,248,248,248,248,248,248,248,248,
	248,248,248,248,248,248,249,249,249,249,249,249,249,249,249,249,
	249,249,249,249,249,249,249,249,249,249,249,249,249,249,249,249,
	249,249,249,249,249,249,249,249,249,250,250,250,250,250,250,250,
	250,250,250,250,250,250,250,250,250,250,250,250,250,250,250,250,
	250,250,250,250,250,250,250,250,250,250,250,250,250,251,251,251,
	251,251,251,251,251,251,251,251,251,251,251,251,251,251,251,251,
	251,251,251,251,251,251,251,251,251,251,251,251,251,251,251,251,
	251,252,252,252,252,252,252,252,252,252,252,252,252,252,252,252,
	252,252,252,252,252,252,252,252,252,252,252,252,252,252,252,252,
	252,252,252,252,252,253,253,253,253,253,253,253,253,253,253,253,
	253,253,253,253,253,253,253,253,253,253,253,253,253,253,253,253,
	253,253,253,253,253,253,253,253,253,254,254,254,254,254,254,254,
	254,254,254,254,254,254,254,254,254,254,254,254,254,254,254,254,
	254,254,254,254,254,254,254,254,254,254,254,254,254,255,255,255,
	255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};

// half float bits to 0..4095 linear, clamped. negative and NaN are 0.
static DWORD _dds_half_to_linear12(DWORD h)
{
	DWORD e;
	DWORD m;
	DWORD n;
	
	if (h & 0x8000)
	{
		return 0;
	}
	
	e = (h >> 10) & 31;
	m = h & 1023;
	
	if (e == 31)
	{
		return m ? 0 : 4095;
	}
	
	if (e >= 15)
	{
		return 4095;
	}
	
	// denormals are too small to matter.
	if (e == 0)
	{
		return 0;
	}
	
	// value = (1024 + m) / 1024 * 2^(e - 15)
	n = 10 + (15 - e);
	
	return ((1024 + m) * 4095 + (1 << (n - 1))) >> n;
}

// float bits to 0..4095 linear, clamped. negative and NaN are 0.
static DWORD _dds_float_to_linear12(DWORD f)
{
	DWORD e;
	DWORD m;
	DWORD n;
	
	if (f & 0x80000000)
	{
		return 0;
	}
	
	e = (f >> 23) & 255;
	m = f & 0x7fffff;
	
	if (e == 255)
	{
		return m ? 0 : 4095;
	}
	
	if (e >= 127)
	{
		return 4095;
	}
	
	// too small to matter (includes denormals)
	if (e < 97)
	{
		return 0;
	}
	
	// value = (0x800000 + m) / 0x800000 * 2^(e - 127)
	n = 23 + (127 - e);
	
	return (DWORD)((((ULONGLONG)(0x800000 | m)) * 4095 + (((ULONGLONG)1) << (n - 1))) >> n);
}

static BYTE _dds_linear12_to_srgb8(DWORD v)
{
	return _dds_srgb_table[v > 4095 ? 4095 : v];
}

static BYTE _dds_linear12_to_alpha8(DWORD v)
{
	return (BYTE)((v * 255 + 2047) / 4095);
}

static DWORD _dds_isqrt(DWORD v)
{
	DWORD r;
	DWORD bit;
	
	r = 0;
	
	for(bit=0x80;bit;bit>>=1)
	{
		DWORD t;
		
		t = r | bit;
		
		if (t * t <= v)
		{
			r = t;
		}
	}
	
	return r;
}

// reconstruct blue from red and green normal map channels.
static BYTE _dds_reconstruct_z(BYTE r,BYTE g)
{
	int x;
	int y;
	int zz;
	
	x = (int)r * 2 - 255;
	y = (int)g * 2 - 255;
	
	zz = 255 * 255 - x * x - y * y;
	
	if (zz < 0)
	{
		zz = 0;
	}
	
	return (BYTE)((_dds_isqrt((DWORD)zz) + 255) / 2);
}

// decode block compressed data.
static void _dds_decode_bc(const BYTE *src,DWORD wide,DWORD high,int type,BYTE *dst)
{
	DWORD bx_count;
	DWORD by_count;
	DWORD block_size;
	DWORD by;
	
	bx_count = (wide + 3) / 4;
	by_count = (high + 3) / 4;
	
	switch(type)
	{
		case _DDS_TYPE_BC1:
		case _DDS_TYPE_BC4:
		case _DDS_TYPE_BC4_SIGNED:
			block_size = 8;
			break;
			
		default:
			block_size = 16;
			break;
	}
	
	for(by=0;by<by_count;by++)
	{
		DWORD bx;
		
		for(bx=0;bx<bx_count;bx++)
		{
			BYTE rgba[4*4*4];
			DWORD y;
			DWORD x;
			DWORD y_run;
			DWORD x_run;
			
			// decode a 4x4 block into rgba.
			switch(type)
			{
				case _DDS_TYPE_BC1:
					bcdec_bc1(src,rgba,4*4);
					break;
					
				case _DDS_TYPE_BC2:
				case _DDS_TYPE_BC2_PREMULTIPLIED:
					bcdec_bc2(src,rgba,4*4);
					break;
					
				case _DDS_TYPE_BC3:
				case _DDS_TYPE_BC3_PREMULTIPLIED:
					bcdec_bc3(src,rgba,4*4);
					break;
					
				case _DDS_TYPE_BC7:
					bcdec_bc7(src,rgba,4*4);
					break;
					
				case _DDS_TYPE_BC4:
				case _DDS_TYPE_BC4_SIGNED:
				{
					BYTE r[4*4];
					int is_signed;
					
					is_signed = (type == _DDS_TYPE_BC4_SIGNED);
					
					bcdec_bc4(src,r,4,is_signed);
					
					for(y=0;y<4*4;y++)
					{
						BYTE v;
						
						v = is_signed ? (BYTE)((int)(signed char)r[y] + 128) : r[y];
						
						rgba[y*4+0] = v;
						rgba[y*4+1] = v;
						rgba[y*4+2] = v;
						rgba[y*4+3] = 255;
					}
					
					break;
				}
				
				case _DDS_TYPE_BC5:
				case _DDS_TYPE_BC5_SIGNED:
				{
					BYTE rg[4*4*2];
					int is_signed;
					
					is_signed = (type == _DDS_TYPE_BC5_SIGNED);
					
					bcdec_bc5(src,rg,4*2,is_signed);
					
					for(y=0;y<4*4;y++)
					{
						BYTE r;
						BYTE g;
						
						r = rg[y*2+0];
						g = rg[y*2+1];
						
						if (is_signed)
						{
							r = (BYTE)((int)(signed char)r + 128);
							g = (BYTE)((int)(signed char)g + 128);
						}
						
						rgba[y*4+0] = r;
						rgba[y*4+1] = g;
#if _DDS_BC5_RECONSTRUCT_Z
						rgba[y*4+2] = _dds_reconstruct_z(r,g);
#else
						rgba[y*4+2] = 0;
#endif
						rgba[y*4+3] = 255;
					}
					
					break;
				}
				
				case _DDS_TYPE_BC6H:
				case _DDS_TYPE_BC6H_SIGNED:
				{
					unsigned short half[4*4*3];
					
					bcdec_bc6h_half(src,half,4*3,(type == _DDS_TYPE_BC6H_SIGNED));
					
					for(y=0;y<4*4;y++)
					{
						rgba[y*4+0] = _dds_linear12_to_srgb8(_dds_half_to_linear12(half[y*3+0]));
						rgba[y*4+1] = _dds_linear12_to_srgb8(_dds_half_to_linear12(half[y*3+1]));
						rgba[y*4+2] = _dds_linear12_to_srgb8(_dds_half_to_linear12(half[y*3+2]));
						rgba[y*4+3] = 255;
					}
					
					break;
				}
			}
			
			// DXT2/DXT4 store premultiplied color.
			if ((type == _DDS_TYPE_BC2_PREMULTIPLIED) || (type == _DDS_TYPE_BC3_PREMULTIPLIED))
			{
				for(y=0;y<4*4;y++)
				{
					DWORD a;
					
					a = rgba[y*4+3];
					
					if ((a != 0) && (a != 255))
					{
						DWORD c;
						
						for(c=0;c<3;c++)
						{
							DWORD v;
							
							v = ((DWORD)rgba[y*4+c] * 255 + (a / 2)) / a;
							
							rgba[y*4+c] = (BYTE)(v > 255 ? 255 : v);
						}
					}
				}
			}
			
			// copy the part of the block that is inside the image.
			y_run = high - by * 4;
			
			if (y_run > 4)
			{
				y_run = 4;
			}
			
			x_run = wide - bx * 4;
			
			if (x_run > 4)
			{
				x_run = 4;
			}
			
			for(y=0;y<y_run;y++)
			{
				BYTE *d;
				const BYTE *s;
				
				d = dst + ((size_t)(by * 4 + y) * wide + bx * 4) * 4;
				s = rgba + y * 4 * 4;
				
				for(x=0;x<x_run*4;x++)
				{
					d[x] = s[x];
				}
			}
			
			src += block_size;
		}
	}
}

// uncompressed formats described by bit masks.
static void _dds_decode_masked(const BYTE *src,DWORD wide,DWORD high,const _dds_format_t *f,BYTE *dst)
{
	_dds_mask_t rm;
	_dds_mask_t gm;
	_dds_mask_t bm;
	_dds_mask_t am;
	DWORD bytes_per_pixel;
	DWORD y;
	
	_dds_mask_init(&rm,f->rmask);
	_dds_mask_init(&gm,f->gmask);
	_dds_mask_init(&bm,f->bmask);
	_dds_mask_init(&am,f->amask);
	
	bytes_per_pixel = (DWORD)f->bitcount / 8;
	
	for(y=0;y<high;y++)
	{
		DWORD x;
		
		for(x=0;x<wide;x++)
		{
			DWORD pixel;
			DWORD r;
			DWORD g;
			DWORD b;
			DWORD a;
			
			switch(bytes_per_pixel)
			{
				case 1:
					pixel = src[0];
					break;
					
				case 2:
					pixel = _dds_read16(src);
					break;
					
				case 3:
					pixel = _dds_read16(src) | ((DWORD)src[2] << 16);
					break;
					
				default:
					pixel = _dds_read32(src);
					break;
			}
			
			src += bytes_per_pixel;
			
			if (f->alpha_only)
			{
				r = 255;
				g = 255;
				b = 255;
				a = _dds_mask_get(&am,pixel);
			}
			else
			{
				r = _dds_mask_get(&rm,pixel);
				
				if (f->gray)
				{
					g = r;
					b = r;
				}
				else
				{
					g = _dds_mask_get(&gm,pixel);
					b = _dds_mask_get(&bm,pixel);
				}
				
				a = am.bits ? _dds_mask_get(&am,pixel) : 255;
			}
			
			dst[0] = (BYTE)r;
			dst[1] = (BYTE)g;
			dst[2] = (BYTE)b;
			dst[3] = (BYTE)a;
			dst += 4;
		}
	}
}

// 16 bit unorm and float formats.
static void _dds_decode_components(const BYTE *src,DWORD wide,DWORD high,const _dds_format_t *f,BYTE *dst)
{
	DWORD run;
	
	run = wide * high;
	
	while(run)
	{
		DWORD v[4];
		DWORD c;
		BYTE rgba[4];
		
		for(c=0;c<(DWORD)f->channels;c++)
		{
			if (f->component_size == 2)
			{
				v[c] = _dds_read16(src);
			}
			else
			{
				v[c] = _dds_read32(src);
			}
			
			src += f->component_size;
		}
		
		if (f->type == _DDS_TYPE_UNORM16)
		{
			for(c=0;c<(DWORD)f->channels;c++)
			{
				v[c] = (v[c] * 255 + 32767) / 65535;
				
				rgba[c] = (BYTE)v[c];
			}
		}
		else
		{
			for(c=0;c<(DWORD)f->channels;c++)
			{
				DWORD linear;
				
				if (f->component_size == 2)
				{
					linear = _dds_half_to_linear12(v[c]);
				}
				else
				{
					linear = _dds_float_to_linear12(v[c]);
				}
				
				if (c == 3)
				{
					rgba[c] = _dds_linear12_to_alpha8(linear);
				}
				else
				{
					rgba[c] = _dds_linear12_to_srgb8(linear);
				}
			}
		}
		
		switch(f->channels)
		{
			case 1:
				dst[0] = rgba[0];
				dst[1] = rgba[0];
				dst[2] = rgba[0];
				dst[3] = 255;
				break;
				
			case 2:
				dst[0] = rgba[0];
				dst[1] = rgba[1];
				dst[2] = 0;
				dst[3] = 255;
				break;
				
			case 3:
				dst[0] = rgba[0];
				dst[1] = rgba[1];
				dst[2] = rgba[2];
				dst[3] = 255;
				break;
				
			default:
				dst[0] = rgba[0];
				dst[1] = rgba[1];
				dst[2] = rgba[2];
				dst[3] = rgba[3];
				break;
		}
		
		dst += 4;
		run--;
	}
}

BYTE *dds_decode(const BYTE *data,SIZE_T size,DWORD *out_wide,DWORD *out_high,int *out_has_alpha)
{
	_dds_format_t f;
	DWORD wide;
	DWORD high;
	DWORD data_offset;
	ULONGLONG need;
	BYTE *pixels;
	int legacy_ret;
	
	if (size < _DDS_SIZE_BASE)
	{
		return NULL;
	}
	
	if (_dds_read32(data) != _DDS_MAGIC)
	{
		return NULL;
	}
	
	// header size and pixel format size must be valid.
	if (_dds_read32(data + _DDS_OFFSET_HEADER_SIZE) != 124)
	{
		return NULL;
	}
	
	if (_dds_read32(data + _DDS_OFFSET_PF_SIZE) != 32)
	{
		return NULL;
	}
	
	high = _dds_read32(data + _DDS_OFFSET_HEIGHT);
	wide = _dds_read32(data + _DDS_OFFSET_WIDTH);
	
	if ((wide == 0) || (high == 0) || (wide > _DDS_MAX_DIMENSION) || (high > _DDS_MAX_DIMENSION))
	{
		return NULL;
	}
	
	data_offset = _DDS_SIZE_BASE;
	
	// header starts after the magic number.
	legacy_ret = _dds_format_from_legacy(&f,data);
	
	if (legacy_ret == 2)
	{
		if (size < _DDS_SIZE_DX10)
		{
			return NULL;
		}
		
		if (!_dds_format_from_dxgi(&f,_dds_read32(data + _DDS_OFFSET_DX10)))
		{
			return NULL;
		}
		
		data_offset = _DDS_SIZE_DX10;
	}
	else
	if (legacy_ret != 1)
	{
		return NULL;
	}
	
	// make sure the first surface is all there.
	switch(f.type)
	{
		case _DDS_TYPE_MASKED:
			need = (ULONGLONG)((wide * (DWORD)f.bitcount + 7) / 8) * high;
			break;
			
		case _DDS_TYPE_UNORM16:
		case _DDS_TYPE_FLOAT:
			need = (ULONGLONG)wide * high * (DWORD)(f.channels * f.component_size);
			break;
			
		case _DDS_TYPE_BC1:
		case _DDS_TYPE_BC4:
		case _DDS_TYPE_BC4_SIGNED:
			need = (ULONGLONG)((wide + 3) / 4) * ((high + 3) / 4) * 8;
			break;
			
		default:
			need = (ULONGLONG)((wide + 3) / 4) * ((high + 3) / 4) * 16;
			break;
	}
	
	if ((ULONGLONG)data_offset + need > (ULONGLONG)size)
	{
		return NULL;
	}
	
	// use HeapAlloc directly. mem_alloc is fatal on failure and this could be a very large image.
	pixels = (BYTE *)HeapAlloc(GetProcessHeap(),0,(SIZE_T)((ULONGLONG)wide * high * 4));
	
	if (!pixels)
	{
		return NULL;
	}
	
	switch(f.type)
	{
		case _DDS_TYPE_MASKED:
			_dds_decode_masked(data + data_offset,wide,high,&f,pixels);
			break;
			
		case _DDS_TYPE_UNORM16:
		case _DDS_TYPE_FLOAT:
			_dds_decode_components(data + data_offset,wide,high,&f,pixels);
			break;
			
		default:
			_dds_decode_bc(data + data_offset,wide,high,f.type,pixels);
			break;
	}
	
	*out_wide = wide;
	*out_high = high;
	*out_has_alpha = _dds_format_has_alpha(&f);
	
	return pixels;
}

void dds_free(BYTE *pixels)
{
	if (pixels)
	{
		HeapFree(GetProcessHeap(),0,pixels);
	}
}

int dds_is_dds(IStream *stream)
{
	int ret;
	HGLOBAL hglobal;
	
	ret = 0;
	
	if (SUCCEEDED(GetHGlobalFromStream(stream,&hglobal)))
	{
		const BYTE *data_ptr;
		
		data_ptr = (const BYTE *)GlobalLock(hglobal);
		
		if (data_ptr)
		{
			if (GlobalSize(hglobal) >= 4)
			{
				if (_dds_read32(data_ptr) == _DDS_MAGIC)
				{
					ret = 1;
				}
			}
			
			GlobalUnlock(hglobal);
		}
	}
	
	return ret;
}

int dds_load(IStream *stream,void *user_data,int (*info_callback)(void *user_data,DWORD frame_count,DWORD wide,DWORD high,int has_alpha),int (*frame_callback)(void *user_data,BYTE *pixels,int delay))
{
	int ret;
	HGLOBAL hglobal;
	
	ret = 0;
	
	if (SUCCEEDED(GetHGlobalFromStream(stream,&hglobal)))
	{
		const BYTE *data_ptr;
		
		data_ptr = (const BYTE *)GlobalLock(hglobal);
		
		if (data_ptr)
		{
			BYTE *pixels;
			DWORD wide;
			DWORD high;
			int has_alpha;
			
			pixels = dds_decode(data_ptr,GlobalSize(hglobal),&wide,&high,&has_alpha);
			
			GlobalUnlock(hglobal);
			
			if (pixels)
			{
				if (info_callback(user_data,1,wide,high,has_alpha))
				{
					if (frame_callback(user_data,pixels,0))
					{
						ret = 1;
					}
				}
				
				dds_free(pixels);
			}
		}
	}
	
	return ret;
}

