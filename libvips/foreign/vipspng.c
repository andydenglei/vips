/* Load/save png image with libpng
 *
 * 28/11/03 JC
 *	- better no-overshoot on tile loop
 * 22/2/05
 *	- read non-interlaced PNG with a line buffer (thanks Michel Brabants)
 * 11/1/06
 * 	- read RGBA palette-ized images more robustly (thanks Tom)
 * 20/4/06
 * 	- auto convert to sRGB/mono (with optional alpha) for save
 * 1/5/06
 * 	- from vips_png.c
 * 8/5/06
 * 	- set RGB16/GREY16 if appropriate
 * 2/11/07
 * 	- use im_wbuffer() API for BG writes
 * 28/2/09
 * 	- small cleanups
 * 4/2/10
 * 	- gtkdoc
 * 	- fixed 16-bit save
 * 12/5/10
 * 	- lololo but broke 8-bit save, fixed again
 * 20/7/10 Tim Elliott
 * 	- added im_vips2bufpng()
 * 8/1/11
 * 	- get set png resolution (thanks Zhiyu Wu)
 * 17/3/11
 * 	- update for libpng-1.5 API changes
 * 	- better handling of palette and 1-bit images
 * 	- ... but we are now png 1.2.9 and later only :-( argh
 * 28/3/11
 * 	- argh gamma was wrong when viewed in firefox
 * 19/12/11
 * 	- rework as a set of fns ready for wrapping as a class
 * 7/2/12
 * 	- mild refactoring
 * 	- add support for sequential reads
 * 23/2/12
 * 	- add a longjmp() to our error handler to stop the default one running
 * 13/3/12
 * 	- add ICC profile read/write
 * 15/3/12
 * 	- better alpha handling
 * 	- sanity check pixel geometry before allowing read
 * 17/6/12
 * 	- more alpha fixes ... some images have no transparency chunk but
 * 	  still set color_type to alpha
 * 16/7/13
 * 	- more robust error handling from libpng
 * 9/8/14
 * 	- don't check profiles, helps with libpng >=1.6.11
 * 27/10/14 Lovell
 * 	- add @filter option 
 * 26/2/15
 * 	- close the read down early for a header read ... this saves an
 * 	  fd during file read, handy for large numbers of input images 
 * 31/7/16
 * 	- support --strip option
 * 17/1/17
 * 	- invalidate operation on read error
 * 27/2/17
 * 	- use dbuf for buffer output
 * 30/3/17
 * 	- better behaviour for truncated png files, thanks Yury
 * 26/4/17
 * 	- better @fail handling with truncated PNGs
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

/*
#define DEBUG
#define VIPS_DEBUG
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#ifdef HAVE_PNG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vips/vips.h>
#include <vips/internal.h>
#include <vips/debug.h>

#include "pforeign.h"

#include <png.h>
#include "lodepng.h"
#include "libimagequant.h"

#if PNG_LIBPNG_VER < 10003
#error "PNG library too old."
#endif

static void bytep_to_bytepp(const LodePNGColorMode* color, int width, int height, png_bytep in, png_bytepp row_pointer_out)
{
	int i,j;
	int span;
	int pos=0;
	int bpp= lodepng_get_bpp(color);

	for(i = 0; i< height; i++)
	{
		for(j = 0; j < width; j++)
		{
			if(bpp == 32)
			{
				row_pointer_out[i][4 * j + 0] = in[pos++];
				row_pointer_out[i][4 * j + 1] = in[pos++];
				row_pointer_out[i][4 * j + 2] = in[pos++];
				row_pointer_out[i][4 * j + 3] = in[pos++];
			}
			else if(bpp == 24)
			{
				row_pointer_out[i][3 * j + 0] = in[pos++];
				row_pointer_out[i][3 * j + 1] = in[pos++];
				row_pointer_out[i][3 * j + 2] = in[pos++];
			}
			else if(bpp == 8)
			{
				row_pointer_out[i][j] = in[pos++];
			}
			else if(bpp < 8)
			{
				span = 8/bpp;
				if(j%span == 0)
				{
					row_pointer_out[i][j / span] = in[pos++];
				}
			}
		}
	}
}

static void bytepp_to_bytep(const LodePNGColorMode* color, int width, int height, png_bytep out, png_bytepp row_pointer_in)
{
	int i, j;
	int pos = 0;
	int size;
	
	int channel = color->colortype == LCT_RGBA ? 4 : 3;
	size = width * channel;
	
	for(i = 0; i < height; i++)
	{
		for(j = 0; j < size; j++)
		{
			out[pos++] = row_pointer_in[i][j];
		}
	}
}

static png_bytep malloc_png_bytep(LodePNGColorMode* mode, int width, int height)
{
	png_bytep bytep;
	int channel = mode->colortype == LCT_RGBA ? 4 :3; 
	
	bytep = (png_bytep)malloc(sizeof(png_byte) * width * height * channel);
	return bytep;
}

static png_bytepp malloc_png_bytepp(LodePNGColorMode* mode, int width, int height)
{
	int i;
	png_bytepp bytepp;
	int bpp = lodepng_get_bpp(mode);
	bytepp =(png_bytepp) malloc(sizeof(png_bytep) * height);
	for (i = 0; i < height; i++)
	{
		bytepp[i] = (png_bytep)malloc(sizeof(png_byte) * (width * bpp + 7)/8);
	}
	return bytepp;
}

static void free_png_bytepp(int height, png_bytepp row_pointer)
{
	int i;
	if(row_pointer)
	{
		for(i = 0 ; i < height; i++)
		{
			if(row_pointer[i])
				free(row_pointer[i]);
		}
		free(row_pointer);
	}
}

static void rgb_to_rgba_callback(liq_color row_out[], int row_index, int width, void *user_info) 
{
	int i;
	unsigned char *rgb_row = ((unsigned char *)user_info) + 3 * width * row_index;

	for(i = 0; i < width; i++) 
	{
		row_out[i].r = rgb_row[i * 3 + 0];
		row_out[i].g = rgb_row[i * 3 + 1];
		row_out[i].b = rgb_row[i * 3 + 2];
		row_out[i].a = 255;
	}
}

static unsigned auto_convert_palette_data(LodePNGColorMode* mode_in, LodePNGColorMode* mode_out, int width, int height, png_bytep in, png_bytep* row_pointer_out)
{
	int i;
	unsigned liq_error = LIQ_OK;
	liq_result *quantization_result;
	unsigned char *raw_8bit_pixels;
	const liq_palette *palette;
	size_t pixels_size = width * height;
	liq_attr *handle = liq_attr_create();
	liq_image *input_image;
	if(mode_in->colortype == LCT_RGB)
	{
		input_image = liq_image_create_custom(handle, rgb_to_rgba_callback, in, width, height, 0);
	}
	else
	{
		input_image = liq_image_create_rgba(handle, in, width, height, 0);
	}

	// You could set more options here, like liq_set_quality
	liq_error = liq_image_quantize(input_image, handle, &quantization_result);
	if(liq_error) return liq_error;
	
	raw_8bit_pixels = (unsigned char *)malloc(pixels_size);
	liq_error = liq_set_dithering_level(quantization_result, 1.0);
	if(liq_error) return liq_error;
	liq_error = liq_write_remapped_image(quantization_result, input_image, raw_8bit_pixels, pixels_size);
	if(liq_error) return liq_error;
	palette = liq_get_palette(quantization_result);

	for(i = 0; i < palette->count; i++) 
	{
		lodepng_palette_add(mode_out, palette->entries[i].r, palette->entries[i].g, palette->entries[i].b, palette->entries[i].a);
	}

	bytep_to_bytepp(mode_out, width, height, raw_8bit_pixels, row_pointer_out);

	// Must be freed only after you're done using the palette
	liq_result_destroy(quantization_result);
	liq_image_destroy(input_image);
	liq_attr_destroy(handle);
	free(raw_8bit_pixels);
	
	return liq_error;
}

static unsigned auto_convert_data(LodePNGColorMode* mode_in, LodePNGColorMode* mode_out, int width, int height, png_bytep in, png_bytep* row_pointer_out)
{
   unsigned char* data= 0;/*uncompressed version of the IDAT chunk data*/
   unsigned char* converted;
   unsigned error = 0;
   int bpp = lodepng_get_bpp(mode_out);
   int linebits = ((width * bpp + 7) / 8) * 8;

   converted = (unsigned char*)malloc((height *width * bpp + 7) / 8);
   error = lodepng_convert(converted, in, mode_out, mode_in, width, height);
   if(error) return error;

   if(bpp < 8 && width * bpp != linebits)
   {
	   data  = (unsigned char*)malloc(height * ((width * bpp + 7) / 8));
	   lodepng_add_padding_bits(data, converted, linebits, width * bpp, height);
	   bytep_to_bytepp(mode_out, width, height, (png_bytep)data, row_pointer_out);
	   free(data);
   }
   else
   {
	   bytep_to_bytepp(mode_out, width, height, (png_bytep)converted, row_pointer_out);
   }

   free(converted);
   return error;
}

static void color_mode_init(LodePNGColorMode* mode, png_byte color_type, png_byte bit_depth)
{
	mode->bitdepth = bit_depth;
	switch(color_type)
	{
	case PNG_COLOR_TYPE_GRAY:
		mode->colortype = LCT_GREY;
		break;
	case PNG_COLOR_TYPE_RGB:
		mode->colortype = LCT_RGB;
		break;
	case PNG_COLOR_TYPE_PALETTE:
		mode->colortype = LCT_PALETTE;
		break;
	case PNG_COLOR_TYPE_GRAY_ALPHA:
		mode->colortype = LCT_GREY_ALPHA;
		break;
	case PNG_COLOR_TYPE_RGBA:
		mode->colortype = LCT_RGBA;
		break;
	}
}

static void SetPLTE(png_structp png_ptr, png_infop info_ptr, LodePNGColorMode* mode)
{
	int i;
	png_colorp palette=(png_colorp)malloc(sizeof(png_color)* mode->palettesize);
	for(i = 0; i < mode->palettesize; i++)
	{
		palette[i].red = mode->palette[4 * i + 0];
		palette[i].green = mode->palette[4 * i + 1];
		palette[i].blue = mode->palette[4 * i + 2];
	}
	png_set_PLTE(png_ptr, info_ptr, palette, mode->palettesize);
	free(palette);
}

static void
user_error_function( png_structp png_ptr, png_const_charp error_msg )
{
#ifdef DEBUG
	printf( "user_error_function: %s\n", error_msg );
#endif /*DEBUG*/

	g_warning( "%s", error_msg );

	/* This function must not return or the default error handler will be
	 * invoked.
	 */
	longjmp( png_jmpbuf( png_ptr ), -1 ); 
}

static void
user_warning_function( png_structp png_ptr, png_const_charp warning_msg )
{
#ifdef DEBUG
	printf( "user_warning_function: %s\n", warning_msg );
#endif /*DEBUG*/

	g_warning( "%s", warning_msg );
}

/* What we track during a PNG read.
 */
typedef struct {
	char *name;
	VipsImage *out;
	gboolean fail;

	int y_pos;
	png_structp pPng;
	png_infop pInfo;
	png_bytep *row_pointer;

	/* For FILE input.
	 */
	FILE *fp;

	/* For memory input.
	 */
	const void *buffer;
	size_t length;
	size_t read_pos;

} Read;

/* Can be called many times.
 */
static void
read_destroy( Read *read )
{
	VIPS_FREEF( fclose, read->fp );
	if( read->pPng )
		png_destroy_read_struct( &read->pPng, &read->pInfo, NULL );
	VIPS_FREE( read->row_pointer );
}

static void
read_close_cb( VipsImage *out, Read *read )
{
	read_destroy( read ); 
}

static Read *
read_new( VipsImage *out, gboolean fail )
{
	Read *read;

	if( !(read = VIPS_NEW( out, Read )) )
		return( NULL );

	read->name = NULL;
	read->fail = fail;
	read->out = out;
	read->y_pos = 0;
	read->pPng = NULL;
	read->pInfo = NULL;
	read->row_pointer = NULL;
	read->fp = NULL;
	read->buffer = NULL;
	read->length = 0;
	read->read_pos = 0;

	g_signal_connect( out, "close", 
		G_CALLBACK( read_close_cb ), read ); 

	if( !(read->pPng = png_create_read_struct( 
		PNG_LIBPNG_VER_STRING, NULL,
		user_error_function, user_warning_function )) ) 
		return( NULL );

#ifdef PNG_SKIP_sRGB_CHECK_PROFILE
	/* Prevent libpng (>=1.6.11) verifying sRGB profiles.
	 */
	png_set_option( read->pPng, 
		PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON );
#endif /*PNG_SKIP_sRGB_CHECK_PROFILE*/

	/* Catch PNG errors from png_create_info_struct().
	 */
	if( setjmp( png_jmpbuf( read->pPng ) ) ) 
		return( NULL );

	if( !(read->pInfo = png_create_info_struct( read->pPng )) ) 
		return( NULL );

	return( read );
}

static Read *
read_new_filename( VipsImage *out, const char *name, gboolean fail )
{
	Read *read;

	if( !(read = read_new( out, fail )) )
		return( NULL );

	read->name = vips_strdup( VIPS_OBJECT( out ), name );

        if( !(read->fp = vips__file_open_read( name, NULL, FALSE )) ) 
		return( NULL );

	/* Catch PNG errors from png_read_info().
	 */
	if( setjmp( png_jmpbuf( read->pPng ) ) ) 
		return( NULL );

	/* Read enough of the file that png_get_interlace_type() will start
	 * working.
	 */
	png_init_io( read->pPng, read->fp );
	png_read_info( read->pPng, read->pInfo );

	return( read );
}

/* Read a png header.
 */
static int
png2vips_header( Read *read, VipsImage *out )
{
	png_uint_32 width, height;
	int bit_depth, color_type;
	int interlace_type;

	png_uint_32 res_x, res_y;
	int unit_type;

	png_charp name;
	int compression_type;

	/* Well thank you, libpng.
	 */
#if PNG_LIBPNG_VER < 10400
	png_charp profile;
#else
	png_bytep profile;
#endif

	png_uint_32 proflen;

	int bands; 
	VipsInterpretation interpretation;
	double Xres, Yres;

	if( setjmp( png_jmpbuf( read->pPng ) ) ) 
		return( -1 );

	png_get_IHDR( read->pPng, read->pInfo, 
		&width, &height, &bit_depth, &color_type,
		&interlace_type, NULL, NULL );

	/* png_get_channels() gives us 1 band for palette images ... so look
	 * at colour_type for output bands.
	 *
	 * Ignore alpha, we detect that separately below.
	 */
	switch( color_type ) {
	case PNG_COLOR_TYPE_PALETTE: 
		bands = 3; 
		break;

	case PNG_COLOR_TYPE_GRAY_ALPHA: 
	case PNG_COLOR_TYPE_GRAY: 
		bands = 1; 
		break;

	case PNG_COLOR_TYPE_RGB: 
	case PNG_COLOR_TYPE_RGB_ALPHA: 
		bands = 3; 
		break;

	default:
		vips_error( "png2vips", "%s", _( "unsupported color type" ) );
		return( -1 );
	}

	if( bit_depth > 8 ) {
		if( bands < 3 )
			interpretation = VIPS_INTERPRETATION_GREY16;
		else
			interpretation = VIPS_INTERPRETATION_RGB16;
	}
	else {
		if( bands < 3 )
			interpretation = VIPS_INTERPRETATION_B_W;
		else
			interpretation = VIPS_INTERPRETATION_sRGB;
	}

	/* Expand palette images.
	 */
	if( color_type == PNG_COLOR_TYPE_PALETTE )
		png_set_palette_to_rgb( read->pPng );

	/* Expand transparency.
	 */
	if( png_get_valid( read->pPng, read->pInfo, PNG_INFO_tRNS ) ) {
		png_set_tRNS_to_alpha( read->pPng );
		bands += 1;
	}
	else if( color_type == PNG_COLOR_TYPE_GRAY_ALPHA || 
		color_type == PNG_COLOR_TYPE_RGB_ALPHA ) {
		/* Some images have no transparency chunk, but still set
		 * color_type to alpha.
		 */
		bands += 1;
	}

	/* Expand <8 bit images to full bytes.
	 */
	if( color_type == PNG_COLOR_TYPE_GRAY &&
		bit_depth < 8 ) 
		png_set_expand_gray_1_2_4_to_8( read->pPng );

	/* If we're an INTEL byte order machine and this is 16bits, we need
	 * to swap bytes.
	 */
	if( bit_depth > 8 && 
		!vips_amiMSBfirst() )
		png_set_swap( read->pPng );

	/* Get resolution. Default to 72 pixels per inch, the usual png value. 
	 */
	unit_type = PNG_RESOLUTION_METER;
	res_x = (72 / 2.54 * 100);
	res_y = (72 / 2.54 * 100);
	png_get_pHYs( read->pPng, read->pInfo, &res_x, &res_y, &unit_type );
	switch( unit_type ) {
	case PNG_RESOLUTION_METER:
		Xres = res_x / 1000.0;
		Yres = res_y / 1000.0;
		break;
	
	default:
		Xres = res_x;
		Yres = res_y;
		break;
	}

	/* Set VIPS header.
	 */
	vips_image_init_fields( out,
		width, height, bands,
		bit_depth > 8 ? 
			VIPS_FORMAT_USHORT : VIPS_FORMAT_UCHAR,
		VIPS_CODING_NONE, interpretation, 
		Xres, Yres );

	/* Uninterlaced images will be read in seq mode. Interlaced images are
	 * read via a huge memory buffer.
	 */
	if( interlace_type == PNG_INTERLACE_NONE ) {
		vips_image_set_area( out, VIPS_META_SEQUENTIAL, NULL, NULL ); 

		/* Sequential mode needs thinstrip to work with things like
		 * vips_shrink().
		 */
		vips_image_pipelinev( out, VIPS_DEMAND_STYLE_THINSTRIP, NULL );
	}
	else 
		vips_image_pipelinev( out, VIPS_DEMAND_STYLE_ANY, NULL );

	/* Fetch the ICC profile. @name is useless, something like "icc" or
	 * "ICC Profile" etc.  Ignore it.
	 *
	 * @profile was png_charpp in libpngs < 1.5, png_bytepp is the
	 * modern one. Ignore the warning, if any.
	 */
	if( png_get_iCCP( read->pPng, read->pInfo, 
		&name, &compression_type, &profile, &proflen ) ) {
		void *profile_copy;

#ifdef DEBUG
		printf( "png2vips_header: attaching %d bytes of ICC profile\n",
			proflen );
		printf( "png2vips_header: name = \"%s\"\n", name );
#endif /*DEBUG*/

		if( !(profile_copy = vips_malloc( NULL, proflen )) ) 
			return( -1 );
		memcpy( profile_copy, profile, proflen );
		vips_image_set_blob( out, VIPS_META_ICC_NAME, 
			(VipsCallbackFn) vips_free, profile_copy, proflen );
	}

	/* Sanity-check line size.
	 */
	png_read_update_info( read->pPng, read->pInfo );
	if( png_get_rowbytes( read->pPng, read->pInfo ) != 
		VIPS_IMAGE_SIZEOF_LINE( out ) ) {
		vips_error( "vipspng", 
			"%s", _( "unable to read PNG header" ) );
		return( -1 );
	}

	return( 0 );
}

/* Read a PNG file header into a VIPS header.
 */
int
vips__png_header( const char *name, VipsImage *out )
{
	Read *read;

	if( !(read = read_new_filename( out, name, TRUE )) ||
		png2vips_header( read, out ) ) 
		return( -1 );

	/* Just a header read: we can free the read early and save an fd.
	 */
	read_destroy( read );

	return( 0 );
}

/* Out is a huge "t" buffer we decompress to.
 */
static int
png2vips_interlace( Read *read, VipsImage *out )
{
	int y;

#ifdef DEBUG
	printf( "png2vips_interlace: reading whole image\n" ); 
#endif /*DEBUG*/

	if( vips_image_write_prepare( out ) )
		return( -1 );

	if( setjmp( png_jmpbuf( read->pPng ) ) ) 
		return( -1 );
 
	if( !(read->row_pointer = VIPS_ARRAY( NULL, out->Ysize, png_bytep )) )
		return( -1 );
	for( y = 0; y < out->Ysize; y++ )
		read->row_pointer[y] = VIPS_IMAGE_ADDR( out, 0, y );

	/* Some libpng warn you to call png_set_interlace_handling(); here, but
	 * that can actually break interlace. We have to live with the warning,
	 * unfortunately.
	 */

	png_read_image( read->pPng, read->row_pointer );

	png_read_end( read->pPng, NULL ); 

	read_destroy( read );

	return( 0 );
}

static int
png2vips_generate( VipsRegion *or, 
	void *seq, void *a, void *b, gboolean *stop )
{
        VipsRect *r = &or->valid;
	Read *read = (Read *) a;

	int y;

#ifdef DEBUG
	printf( "png2vips_generate: line %d, %d rows\n", r->top, r->height );
	printf( "png2vips_generate: y_top = %d\n", read->y_pos );
#endif /*DEBUG*/

	/* We're inside a tilecache where tiles are the full image width, so
	 * this should always be true.
	 */
	g_assert( r->left == 0 );
	g_assert( r->width == or->im->Xsize );
	g_assert( VIPS_RECT_BOTTOM( r ) <= or->im->Ysize );

	/* Tiles should always be a strip in height, unless it's the final
	 * strip.
	 */
	g_assert( r->height == VIPS_MIN( 8, or->im->Ysize - r->top ) ); 

	/* And check that y_pos is correct. It should be, since we are inside
	 * a vips_sequential().
	 */
	if( r->top != read->y_pos ) {
		vips_error( "vipspng", 
			_( "out of order read at line %d" ), read->y_pos );
		return( -1 );
	}

	for( y = 0; y < r->height; y++ ) {
		png_bytep q = (png_bytep) VIPS_REGION_ADDR( or, 0, r->top + y );

		/* We need to catch errors from read_row().
		 */
		if( !setjmp( png_jmpbuf( read->pPng ) ) ) 
			png_read_row( read->pPng, q, NULL );
		else { 
			/* We've failed to read some pixels. Knock this 
			 * operation out of cache. 
			 */
			vips_foreign_load_invalidate( read->out );

#ifdef DEBUG
			printf( "png2vips_generate: png_read_row() failed, "
				"line %d\n", r->top + y ); 
			printf( "png2vips_generate: file %s\n", read->name );
			printf( "png2vips_generate: thread %p\n", 
				g_thread_self() );
#endif /*DEBUG*/

			/* And bail if fail is on. We have to add an error
			 * message, since the handler we install just does
			 * g_warning().
			 */
			if( read->fail ) {
				vips_error( "vipspng", 
					"%s", _( "libpng read error" ) ); 
				return( -1 );
			}
		}

		read->y_pos += 1;
	}

	/* Catch errors from png_read_end(). This can fail on a truncated
	 * file. 
	 */
	if( setjmp( png_jmpbuf( read->pPng ) ) ) {
		if( read->fail ) {
			vips_error( "vipspng", "%s", _( "libpng read error" ) ); 
			return( -1 );
		}

		return( 0 );
	}

	/* We need to shut down the reader immediately at the end of read or
	 * we won't detach ready for the next image.
	 */
	if( read->y_pos >= read->out->Ysize ) {
		png_read_end( read->pPng, NULL ); 
		read_destroy( read );
	}

	return( 0 );
}

/* Interlaced PNGs need to be entirely decompressed into memory then can be
 * served partially from there. Non-interlaced PNGs may be read sequentially.
 */
gboolean
vips__png_isinterlaced( const char *filename )
{
	VipsImage *image;
	Read *read;
	int interlace_type;

	image = vips_image_new();
	if( !(read = read_new_filename( image, filename, TRUE )) ) {
		g_object_unref( image );
		return( -1 );
	}
	interlace_type = png_get_interlace_type( read->pPng, read->pInfo );
	g_object_unref( image );

	return( interlace_type != PNG_INTERLACE_NONE );
}

static int
png2vips_image( Read *read, VipsImage *out )
{
	int interlace_type = png_get_interlace_type( read->pPng, read->pInfo );
	VipsImage **t = (VipsImage **) 
		vips_object_local_array( VIPS_OBJECT( out ), 3 );

	if( interlace_type != PNG_INTERLACE_NONE ) { 
		/* Arg awful interlaced image. We have to load to a huge mem 
		 * buffer, then copy to out.
		 */
		t[0] = vips_image_new_memory();
		if( png2vips_header( read, t[0] ) ||
			png2vips_interlace( read, t[0] ) ||
			vips_image_write( t[0], out ) )
			return( -1 );
	}
	else {
		t[0] = vips_image_new();
		if( png2vips_header( read, t[0] ) ||
			vips_image_generate( t[0], 
				NULL, png2vips_generate, NULL, 
				read, NULL ) ||
			vips_sequential( t[0], &t[1], 
				"tile_height", 8,
				NULL ) ||
			vips_image_write( t[1], out ) )
			return( -1 );
	}

	return( 0 );
}

int
vips__png_read( const char *filename, VipsImage *out, gboolean fail )
{
	Read *read;

#ifdef DEBUG
	printf( "vips__png_read: reading \"%s\"\n", filename );
#endif /*DEBUG*/

	if( !(read = read_new_filename( out, filename, fail )) ||
		png2vips_image( read, out ) )
		return( -1 ); 

#ifdef DEBUG
	printf( "vips__png_read: done\n" );
#endif /*DEBUG*/

	return( 0 );
}

gboolean
vips__png_ispng_buffer( const void *buf, size_t len )
{
	if( len >= 8 &&
		!png_sig_cmp( (png_bytep) buf, 0, 8 ) )
		return( TRUE ); 

	return( FALSE ); 
}

int
vips__png_ispng( const char *filename )
{
	unsigned char buf[8];

	return( vips__get_bytes( filename, buf, 8 ) &&
		vips__png_ispng_buffer( buf, 8 ) ); 
}

static void
vips_png_read_buffer( png_structp pPng, png_bytep data, png_size_t length )
{
	Read *read = png_get_io_ptr( pPng ); 

#ifdef DEBUG
	printf( "vips_png_read_buffer: read %zd bytes\n", length ); 
#endif /*DEBUG*/

	if( read->read_pos + length > read->length )
		png_error( pPng, "not enough data in buffer" );

	memcpy( data, read->buffer + read->read_pos, length );
	read->read_pos += length;
}

static Read *
read_new_buffer( VipsImage *out, const void *buffer, size_t length, 
	gboolean fail )
{
	Read *read;

	if( !(read = read_new( out, fail )) )
		return( NULL );

	read->length = length;
	read->buffer = buffer;

	png_set_read_fn( read->pPng, read, vips_png_read_buffer ); 

	/* Catch PNG errors from png_read_info().
	 */
	if( setjmp( png_jmpbuf( read->pPng ) ) ) 
		return( NULL );

	/* Read enough of the file that png_get_interlace_type() will start
	 * working.
	 */
	png_read_info( read->pPng, read->pInfo );

	return( read );
}

int
vips__png_header_buffer( const void *buffer, size_t length, VipsImage *out )
{
	Read *read;

	if( !(read = read_new_buffer( out, buffer, length, TRUE )) ||
		png2vips_header( read, out ) ) 
		return( -1 );

	return( 0 );
}

int
vips__png_read_buffer( const void *buffer, size_t length, VipsImage *out, 
	gboolean fail )
{
	Read *read;

	if( !(read = read_new_buffer( out, buffer, length, fail )) ||
		png2vips_image( read, out ) )
		return( -1 ); 

	return( 0 );
}

/* Interlaced PNGs need to be entirely decompressed into memory then can be
 * served partially from there. Non-interlaced PNGs may be read sequentially.
 */
gboolean
vips__png_isinterlaced_buffer( const void *buffer, size_t length )
{
	VipsImage *image;
	Read *read;
	int interlace_type;

	image = vips_image_new();

	if( !(read = read_new_buffer( image, buffer, length, TRUE )) ) { 
		g_object_unref( image );
		return( -1 );
	}
	interlace_type = png_get_interlace_type( read->pPng, read->pInfo );
	g_object_unref( image );

	return( interlace_type != PNG_INTERLACE_NONE );
}

const char *vips__png_suffs[] = { ".png", NULL };

/* What we track during a PNG write.
 */
typedef struct {
	VipsImage *in;
	VipsImage *memory;

	FILE *fp;
	VipsDbuf dbuf;

	png_structp pPng;
	png_infop pInfo;
	png_bytep *row_pointer;
} Write;

static void
write_finish( Write *write )
{
	VIPS_FREEF( fclose, write->fp );
	VIPS_UNREF( write->memory );
	vips_dbuf_destroy( &write->dbuf );
	if( write->pPng )
		png_destroy_write_struct( &write->pPng, &write->pInfo );
}

static void
write_destroy( VipsImage *out, Write *write )
{
	write_finish( write ); 
}

static Write *
write_new( VipsImage *in )
{
	Write *write;

	if( !(write = VIPS_NEW( in, Write )) )
		return( NULL );
	memset( write, 0, sizeof( Write ) );
	write->in = in;
	write->memory = NULL;
	write->fp = NULL;
	vips_dbuf_init( &write->dbuf );
	g_signal_connect( in, "close", 
		G_CALLBACK( write_destroy ), write ); 

	if( !(write->row_pointer = VIPS_ARRAY( in, in->Ysize, png_bytep )) )
		return( NULL );
	if( !(write->pPng = png_create_write_struct( 
		PNG_LIBPNG_VER_STRING, NULL,
		user_error_function, user_warning_function )) ) 
		return( NULL );

#ifdef PNG_SKIP_sRGB_CHECK_PROFILE
	/* Prevent libpng (>=1.6.11) verifying sRGB profiles.
	 */
	png_set_option( write->pPng, 
		PNG_SKIP_sRGB_CHECK_PROFILE, PNG_OPTION_ON );
#endif /*PNG_SKIP_sRGB_CHECK_PROFILE*/

	/* Catch PNG errors from png_create_info_struct().
	 */
	if( setjmp( png_jmpbuf( write->pPng ) ) ) 
		return( NULL );

	if( !(write->pInfo = png_create_info_struct( write->pPng )) ) 
		return( NULL );

	return( write );
}

static int
write_png_block( VipsRegion *region, VipsRect *area, void *a )
{
	Write *write = (Write *) a;

	int i;

	/* The area to write is always a set of complete scanlines.
	 */
	g_assert( area->left == 0 );
	g_assert( area->width == region->im->Xsize );
	g_assert( area->top + area->height <= region->im->Ysize );

	/* Catch PNG errors. Yuk.
	 */
	if( setjmp( png_jmpbuf( write->pPng ) ) ) 
		return( -1 );

	for( i = 0; i < area->height; i++ ) 
		write->row_pointer[i] = (png_bytep)
			VIPS_REGION_ADDR( region, 0, area->top + i );

	png_write_rows( write->pPng, write->row_pointer, area->height );

	return( 0 );
}

/* Write a VIPS image to PNG.
 */
static int
write_vips( Write *write, 
	int compress, int interlace, const char *profile,
	VipsForeignPngFilter filter, gboolean strip )
{
	VipsImage *in = write->in;

	int bit_depth;
	int color_type;
	int interlace_type;
	int i, j, nb_passes;
	
	png_byte* image = NULL;
	png_bytep* row_pointer_in = NULL;
	png_bytep* row_pointer_out = NULL;
	gboolean is_rgb_or_rgba = FALSE;
	gboolean auto_converted = FALSE;
	VipsRegion *region = NULL;
	LodePNGColorMode* mode_out;
	LodePNGColorMode* mode_in;

        g_assert( in->BandFmt == VIPS_FORMAT_UCHAR || 
		in->BandFmt == VIPS_FORMAT_USHORT );
	g_assert( in->Coding == VIPS_CODING_NONE );
        g_assert( in->Bands > 0 && in->Bands < 5 );

	/* Catch PNG errors.
	 */
	if( setjmp( png_jmpbuf( write->pPng ) ) ) 
		return( -1 );

	/* Check input image. If we are writing interlaced, we need to make 7
	 * passes over the image. We advertise ourselves as seq, so to ensure
	 * we only suck once from upstream, switch to WIO. 
	 */
	if( interlace ) {
		if( !(write->memory = vips_image_copy_memory( in )) )
			return( -1 );
		in = write->memory;
	}
	else {
		if( vips_image_pio_input( in ) )
			return( -1 );
	}
	if( compress < 0 || compress > 9 ) {
		vips_error( "vips2png", 
			"%s", _( "compress should be in [0,9]" ) );
		return( -1 );
	}

	/* Set compression parameters.
	 */
	png_set_compression_level( write->pPng, compress );

	bit_depth = in->BandFmt == VIPS_FORMAT_UCHAR ? 8 : 16;

	switch( in->Bands ) {
	case 1: color_type = PNG_COLOR_TYPE_GRAY; break;
	case 2: color_type = PNG_COLOR_TYPE_GRAY_ALPHA; break;
	case 3: color_type = PNG_COLOR_TYPE_RGB; break;
	case 4: color_type = PNG_COLOR_TYPE_RGB_ALPHA; break;

	default:
		vips_error( "vips2png", 
			_( "can't save %d band image as png" ), in->Bands );
		return( -1 );
	}

	interlace_type = interlace ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE;
	
	if((color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_RGB_ALPHA) && bit_depth == 8)
	{
		is_rgb_or_rgba = TRUE;
		
		unsigned error = 0;
		mode_in = (LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
		lodepng_color_mode_init(mode_in);
		mode_out = (LodePNGColorMode*)malloc(sizeof(LodePNGColorMode));
		lodepng_color_mode_init(mode_out);
		color_mode_init(mode_in, color_type, bit_depth);
		lodepng_color_mode_copy(mode_out, mode_in);
		
		//get row_pointer from source image
		row_pointer_in = (png_bytepp)malloc(sizeof(png_bytep) * (in->Ysize));
		region = vips_region_new(in);
		VipsRect r = {0, 0, in->Xsize, in->Ysize};
		if( vips_region_prepare(region, &r))
			return (-1);
		for(i = 0; i < in->Ysize; i++)
			row_pointer_in[i] = (png_bytep)VIPS_REGION_ADDR(region, 0, i );
		
		image = malloc_png_bytep(mode_in, in->Xsize, in->Ysize);
		bytepp_to_bytep(mode_in, in->Xsize, in->Ysize, image, row_pointer_in);
		free(row_pointer_in);
		
		error = lodepng_auto_choose_color(mode_out, (unsigned char*)image, in->Xsize, in->Ysize, mode_in);
		if(error) return error;
			
		if((mode_out->colortype == LCT_RGB || mode_out->colortype == LCT_RGBA) && mode_out->bitdepth == 8)
		{
			lodepng_color_mode_cleanup(mode_out);
			color_mode_init(mode_out, LCT_PALETTE, 8);
			row_pointer_out = malloc_png_bytepp(mode_out, in->Xsize, in->Ysize);
			error = auto_convert_palette_data(mode_in, mode_out, in->Xsize, in->Ysize, image, row_pointer_out);
		}
		else
		{
			row_pointer_out = malloc_png_bytepp(mode_out, in->Xsize, in->Ysize);
			error = auto_convert_data(mode_in, mode_out, in->Xsize, in->Ysize, image, row_pointer_out);
		}
		
		if(!error)
			auto_converted = TRUE;
		
		//free mode_in and image
		lodepng_color_mode_cleanup(mode_in);
		free(mode_in);
		free(image);
	}
	
	if(auto_converted)
	{
		// Ignore interlace_type, alway wirte PNG files with PNG_INTERLACE_NONE if it can convert,
		// as PNG size is smaller with PNG_INTERLACE_NONE.
		png_set_IHDR( write->pPng, write->pInfo, 
			in->Xsize, in->Ysize, mode_out->bitdepth, mode_out->colortype, PNG_INTERLACE_NONE, 
			PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT );
	}
	else
	{
		png_set_filter( write->pPng, 0, filter );
		png_set_IHDR( write->pPng, write->pInfo, 
			in->Xsize, in->Ysize, bit_depth, color_type, interlace_type, 
			PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT );
	}
	
	/* Set resolution. libpng uses pixels per meter.
	 */
	png_set_pHYs( write->pPng, write->pInfo, 
		VIPS_RINT( in->Xres * 1000 ), VIPS_RINT( in->Yres * 1000 ), 
		PNG_RESOLUTION_METER );

	/* Set ICC Profile.
	 */
	if( profile && 
		!strip ) {
		if( strcmp( profile, "none" ) != 0 ) { 
			void *data;
			size_t length;

			if( !(data = vips__file_read_name( profile, 
				vips__icc_dir(), &length )) ) 
				return( -1 );

#ifdef DEBUG
			printf( "write_vips: "
				"attaching %zd bytes of ICC profile\n",
				length );
#endif /*DEBUG*/

			png_set_iCCP( write->pPng, write->pInfo, "icc", 
				PNG_COMPRESSION_TYPE_BASE, data, length );
		}
	}
	else if( vips_image_get_typeof( in, VIPS_META_ICC_NAME ) &&
		!strip ) {
		void *data;
		size_t length;

		if( vips_image_get_blob( in, VIPS_META_ICC_NAME, 
			&data, &length ) ) 
			return( -1 ); 

#ifdef DEBUG
		printf( "write_vips: attaching %zd bytes of ICC profile\n",
			length );
#endif /*DEBUG*/

		png_set_iCCP( write->pPng, write->pInfo, "icc", 
			PNG_COMPRESSION_TYPE_BASE, data, length );
	}

	if( auto_converted && mode_out->colortype == LCT_PALETTE)
		SetPLTE( write->pPng, write->pInfo, mode_out);
	
	png_write_info( write->pPng, write->pInfo ); 
	
	if( auto_converted )
	{
		/* Write data.
		*/
		png_write_rows(write->pPng, row_pointer_out, in->Ysize);
		free_png_bytepp(in->Ysize, row_pointer_out);
	}
	else
	{
		/* If we're an intel byte order CPU and this is a 16bit image, we need
		* to swap bytes.
		*/
		if( bit_depth > 8 && !vips_amiMSBfirst() ) 
			png_set_swap( write->pPng ); 

		if( interlace )	
			nb_passes = png_set_interlace_handling( write->pPng );
		else
			nb_passes = 1;
		
		/* Write data.
		*/
		for( i = 0; i < nb_passes; i++ ) 
		{
			if( vips_sink_disc( in, write_png_block, write ) )
				return( -1 );
		}
	}
	/* The setjmp() was held by our background writer: reset it.
	 */
	if( setjmp( png_jmpbuf( write->pPng ) ) ) 
		return( -1 );

	png_write_end( write->pPng, write->pInfo );

	if(is_rgb_or_rgba)
	{
		lodepng_color_mode_cleanup(mode_out);
		free(mode_out);
	}
	
	return( 0 );
}

int
vips__png_write( VipsImage *in, const char *filename, 
	int compress, int interlace, const char *profile,
	VipsForeignPngFilter filter, gboolean strip )
{
	Write *write;

#ifdef DEBUG
	printf( "vips__png_write: writing \"%s\"\n", filename );
#endif /*DEBUG*/

	if( !(write = write_new( in )) )
		return( -1 );

	/* Make output.
	 */
        if( !(write->fp = vips__file_open_write( filename, FALSE )) ) 
		return( -1 );
	png_init_io( write->pPng, write->fp );

	/* Convert it!
	 */
	if( write_vips( write, 
		compress, interlace, profile, filter, strip ) ) {
		vips_error( "vips2png", 
			_( "unable to write \"%s\"" ), filename );

		return( -1 );
	}

	write_finish( write );

#ifdef DEBUG
	printf( "vips__png_write: done\n" ); 
#endif /*DEBUG*/

	return( 0 );
}

static void
user_write_data( png_structp png_ptr, png_bytep data, png_size_t length )
{
	Write *write = (Write *) png_get_io_ptr( png_ptr );

	vips_dbuf_write( &write->dbuf, data, length ); 
}

int
vips__png_write_buf( VipsImage *in, 
	void **obuf, size_t *olen, int compression, int interlace,
	const char *profile, VipsForeignPngFilter filter, gboolean strip )
{
	Write *write;

	if( !(write = write_new( in )) ) 
		return( -1 );

	png_set_write_fn( write->pPng, write, user_write_data, NULL );

	/* Convert it!
	 */
	if( write_vips( write, 
		compression, interlace, profile, filter, strip ) ) {
		vips_error( "vips2png", 
			"%s", _( "unable to write to buffer" ) );
	      
		return( -1 );
	}

	*obuf = vips_dbuf_steal( &write->dbuf, olen );

	write_finish( write );

	return( 0 );
}

#endif /*HAVE_PNG*/
