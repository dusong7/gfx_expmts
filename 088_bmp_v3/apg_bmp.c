/*
BMP File Reader/Writer Implementation
Anton Gerdelan
Version: 3
Licence: see apg_bmp.h
C99
*/

#include "apg_bmp.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _BMP_FILE_HDR_SZ 14
#define _BMP_MIN_DIB_HDR_SZ 40
#define _BMP_MIN_HDR_SZ ( _BMP_FILE_HDR_SZ + _BMP_MIN_DIB_HDR_SZ )

#pragma pack( push, 1 ) // supported on GCC in addition to individual packing attribs
/* all BMP files, regardless of type, start with this file header */
typedef struct _bmp_file_header_t {
  char file_type[2];
  uint32_t file_sz;
  uint16_t reserved1;
  uint16_t reserved2;
  uint32_t image_data_offset;
} _bmp_file_header_t;

/* following the file header is the BMP type header. this is the most commonly used format */
typedef struct _bmp_dib_BITMAPINFOHEADER_t {
  uint32_t this_header_sz;
  int32_t w; // in older headers w & h these are shorts and may be unsigned
  int32_t h;
  uint16_t n_planes; // must be 1
  uint16_t bpp;
  uint32_t compression_method; // 16 and 32-bit images must have a value of 3 here
  uint32_t image_uncompressed_sz;
  int32_t horiz_pixels_per_meter;
  int32_t vert_pixels_per_meter;
  uint32_t n_colours_in_palette;
  uint32_t n_important_colours;
  /* NOTE(Anton) a DIB header may end here at 40-bytes. be careful using sizeof() */
  /* if 'compression' value, above, is set to 3 ie the image is 16 or 32-bit, then these colour channel masks follow the headers.
  these are big-endian order bit masks to assign bits of each pixel to different colours. bits used must be contiguous and not overlap. */
  uint32_t bitmask_r;
  uint32_t bitmask_g;
  uint32_t bitmask_b;
} _bmp_dib_BITMAPINFOHEADER_t;
#pragma pack( pop )

typedef enum _bmp_compression_t {
  BI_RGB            = 0,
  BI_RLE8           = 1,
  BI_RLE4           = 2,
  BI_BITFIELDS      = 3,
  BI_JPEG           = 4,
  BI_PNG            = 5,
  BI_ALPHABITFIELDS = 6,
  BI_CMYK           = 11,
  BI_CMYKRLE8       = 12,
  BI_CMYRLE4        = 13
} _bmp_compression_t;

/* convenience struct and file->memory function */
typedef struct _entire_file_t {
  void* data;
  size_t sz;
} _entire_file_t;

/*
RETURNS
- true on success. record->data is allocated memory and must be freed by the caller.
- false on any error. Any allocated memory is freed if false is returned */
static bool _read_entire_file( const char* filename, _entire_file_t* record ) {
  FILE* fp = fopen( filename, "rb" );
  if ( !fp ) { return false; }
  fseek( fp, 0L, SEEK_END );
  record->sz   = (size_t)ftell( fp );
  record->data = malloc( record->sz );
  if ( !record->data ) {
    fclose( fp );
    return false;
  }
  rewind( fp );
  size_t nr = fread( record->data, record->sz, 1, fp );
  fclose( fp );
  if ( 1 != nr ) { return false; } // TODO memleak
  return true;
}

static bool _validate_file_hdr( _bmp_file_header_t* file_hdr_ptr, size_t file_sz ) {
  if ( !file_hdr_ptr ) { return NULL; }
  if ( file_hdr_ptr->file_type[0] != 'B' || file_hdr_ptr->file_type[1] != 'M' ) { return false; }
  if ( file_hdr_ptr->image_data_offset > file_sz ) { return false; }
  return true;
}

static bool _validate_dib_hdr( _bmp_dib_BITMAPINFOHEADER_t* dib_hdr_ptr, size_t file_sz ) {
  if ( !dib_hdr_ptr ) { return NULL; }
  if ( _BMP_FILE_HDR_SZ + dib_hdr_ptr->this_header_sz > file_sz ) { return false; }
  // TODO(Anton) -- check for invalid bpp: if ( dib_hdr_ptr->bpp != 32 && dib_hdr_ptr->bpp != 24 && dib_hdr_ptr->bpp != 16 && dib_hdr_ptr->bpp != ) { return
  // false; }
  if ( ( 32 == dib_hdr_ptr->bpp || 16 == dib_hdr_ptr->bpp ) && ( BI_BITFIELDS != dib_hdr_ptr->compression_method && BI_ALPHABITFIELDS != dib_hdr_ptr->compression_method ) ) {
    return false;
  }
  if ( BI_RGB != dib_hdr_ptr->compression_method && BI_BITFIELDS != dib_hdr_ptr->compression_method && BI_ALPHABITFIELDS != dib_hdr_ptr->compression_method ) {
    return false;
  }
  // 8k * 16 seems like a reasonable 'hey this is probably an accident' size
  uint32_t absw = abs( dib_hdr_ptr->w ); // NOTE(Anton) if i put the abs() expression directly into the if-clause it doesn't work as I expected. not sure why...
  uint32_t absh = abs( dib_hdr_ptr->h );
  if ( 0 == dib_hdr_ptr->w || 0 == dib_hdr_ptr->h || absw > 7182 * 10 || absh > 7182 * 16 ) { return false; }

  /* NOTE(Anton) if images reliably used n_colours_in_palette we could have done a palette/file size integrity check here.
  because some always set 0 then we have to check every palette indexing as we read them */
  return true;
}

/* NOTE(Anton) this could have ifdef branches on different compilers for the intrinsics versions for perf */
static uint32_t _bitscan( uint32_t dword ) {
  for ( uint32_t i = 0; i < 32; i++ ) {
    if ( 1 & dword ) { return i; }
    dword = dword >> 1;
  }
  return -1;
}

unsigned char* apg_bmp_read( const char* filename, int* w, int* h, int* n_chans ) {
  if ( !filename || !w || !h || !n_chans ) { return NULL; }

  // read in the whole file into memory first - much faster than parsing on-the-fly
  _entire_file_t record;
  if ( !_read_entire_file( filename, &record ) ) { return NULL; }
  if ( record.sz < _BMP_MIN_HDR_SZ ) {
    free( record.data );
    return NULL;
  }

  // grab and validate the first, file, header
  _bmp_file_header_t* file_hdr_ptr = (_bmp_file_header_t*)record.data;
  if ( !_validate_file_hdr( file_hdr_ptr, record.sz ) ) {
    free( record.data );
    return NULL;
  }

  // grad and validate the second, DIB, header
  _bmp_dib_BITMAPINFOHEADER_t* dib_hdr_ptr = (_bmp_dib_BITMAPINFOHEADER_t*)( (uint8_t*)record.data + _BMP_FILE_HDR_SZ );
  if ( !_validate_dib_hdr( dib_hdr_ptr, record.sz ) ) {
    free( record.data );
    return NULL;
  }

  // bitmaps can have negative dims to indicate the image should be flipped
  uint32_t width = *w = abs( dib_hdr_ptr->w );
  uint32_t height = *h = abs( dib_hdr_ptr->h );

  // TODO(Anton) flip image memory at the end if this is true. because doing it per row was making me write bugs.
  // bool vertically_flip = dib_hdr_ptr->h > 0 ? false : true;

  // channel count and palette are not well defined in the header so we make a good guess here
  int n_dst_chans = 3, n_src_chans = 3;
  bool has_palette = false;
  switch ( dib_hdr_ptr->bpp ) {
  case 32: n_dst_chans = n_src_chans = 4; break; // technically can be RGB but not supported
  case 24: n_dst_chans = n_src_chans = 3; break; // technically can be RGBA but not supported
  case 8:                                        // seems to always use a BGR0 palette, even for greyscale
    n_dst_chans = 3;
    has_palette = true;
    n_src_chans = 1;
    break;
  case 4: // always has a palette - needed for a MS-saved BMP
    n_dst_chans = 3;
    has_palette = true;
    n_src_chans = 1;
    break;
  case 1: // 1-bpp means the palette has 3 colour channels with 2 colours i.e. monochrome but not always black & white
    n_dst_chans = 3;
    has_palette = true;
    n_src_chans = 1;
    break;
  default: // this includes 2bpp and 16bpp
    free( record.data );
    return NULL;
  } // endswitch
  *n_chans = n_dst_chans;
  // NOTE(Anton) some image formats are not allowed a palette - could check for a bad header spec here also
  if ( dib_hdr_ptr->n_colours_in_palette > 0 ) { has_palette = true; }

	printf("db: w = %u h = %u n_src_chans = %u, n_dst_chans = %u \n", *w, *h, n_src_chans, n_dst_chans );

  uint32_t palette_offset = _BMP_FILE_HDR_SZ + dib_hdr_ptr->this_header_sz;
  bool has_bitmasks       = false;
  if ( BI_BITFIELDS == dib_hdr_ptr->compression_method || BI_ALPHABITFIELDS == dib_hdr_ptr->compression_method ) {
    has_bitmasks = true;
    palette_offset += 12;
  }
  if ( palette_offset > record.sz ) {
    free( record.data );
    return NULL;
  }

  // work out if any padding how much to skip at end of each row
  uint32_t unpadded_row_sz = width * n_src_chans;
  // bit-encoded palette indices have different padding properties
  if ( 4 == dib_hdr_ptr->bpp ) {
    unpadded_row_sz = width % 2 > 0 ? width / 2 + 1 : width / 2; // find how many whole bytes required for this bit width
  }
  if ( 1 == dib_hdr_ptr->bpp ) {
    unpadded_row_sz = width % 8 > 0 ? width / 8 + 1 : width / 8; // find how many whole bytes required for this bit width
  }
  uint32_t row_padding_sz = 0 == unpadded_row_sz % 4 ? 0 : 4 - ( unpadded_row_sz % 4 ); // NOTE(Anton) didn't expect operator precedence of - over %

  // another file size integrity check: partially validate source image data size
  // 'image_data_offset' is by row padded to 4 bytes and is either colour data or palette indices.
  if ( file_hdr_ptr->image_data_offset + ( unpadded_row_sz + row_padding_sz ) * height > record.sz ) {
    free( record.data );
    return NULL;
  }
  // TODO(Anton) include palette size in the above

  // find which bit number each colour channel starts at, so we can separate colours out
  int bitshift_rgba[4] = { 0, 0, 0, 0 };
  uint32_t bitmask_a   = 0;
  if ( has_bitmasks ) {
    bitmask_a        = ~( dib_hdr_ptr->bitmask_r | dib_hdr_ptr->bitmask_g | dib_hdr_ptr->bitmask_b );
    bitshift_rgba[0] = _bitscan( dib_hdr_ptr->bitmask_r );
    bitshift_rgba[1] = _bitscan( dib_hdr_ptr->bitmask_g );
    bitshift_rgba[2] = _bitscan( dib_hdr_ptr->bitmask_b );
    bitshift_rgba[3] = _bitscan( bitmask_a );
    if ( bitshift_rgba[0] < 0 || bitshift_rgba[1] < 0 || bitshift_rgba[2] < 0 || bitshift_rgba[3] < 0 ) {
      free( record.data );
      return NULL;
    }
  }

  // allocate memory for the output pixels block
  unsigned char* dst_img_ptr = malloc( width * height * n_dst_chans );
  if ( !dst_img_ptr ) {
    free( record.data );
    return NULL;
  }

  uint8_t* palette_data_ptr = (uint8_t*)record.data + palette_offset;
  uint8_t* src_img_ptr      = (uint8_t*)record.data + file_hdr_ptr->image_data_offset;
  int dst_stride_sz         = width * n_dst_chans;

  //   == 32-bpp -> 32-bit RGBA. == 32-bit and 16-bit require bitmasks
  if ( 32 == dib_hdr_ptr->bpp ) {
    // check source image has enough data in it to read from
    if ( file_hdr_ptr->image_data_offset + height * width * 4 > record.sz ) {
      free( record.data );
      free( dst_img_ptr );
      return NULL;
    }
    uint32_t src_byte_idx = 0;
    for ( uint32_t r = 0; r < height; r++ ) {
      int dst_pixels_idx = r * dst_stride_sz;
      for ( uint32_t c = 0; c < width; c++ ) {
        uint32_t pixel;
        memcpy( &pixel, &src_img_ptr[src_byte_idx], 4 );
        // NOTE(Anton) the below assumes 32-bits is always RGBA 1 byte per channel. 10,10,10 RGB exists though and isn't handled.
        dst_img_ptr[dst_pixels_idx++] = ( uint8_t )( ( pixel & dib_hdr_ptr->bitmask_r ) >> bitshift_rgba[0] );
        dst_img_ptr[dst_pixels_idx++] = ( uint8_t )( ( pixel & dib_hdr_ptr->bitmask_g ) >> bitshift_rgba[1] );
        dst_img_ptr[dst_pixels_idx++] = ( uint8_t )( ( pixel & dib_hdr_ptr->bitmask_b ) >> bitshift_rgba[2] );
        dst_img_ptr[dst_pixels_idx++] = ( uint8_t )( ( pixel & bitmask_a ) >> bitshift_rgba[3] );
        src_byte_idx += 4;
      }
      src_byte_idx += row_padding_sz;
    }

    // == 8-bpp -> 24-bit RGB ==
  } else if ( 8 == dib_hdr_ptr->bpp && has_palette ) {
    // validate palette fits in file. palette is always RGBA 4 byte aligned it looks like. should come before image_data_offset
    if ( palette_offset + height * width * 4 > record.sz || palette_offset + height * width * 4 > file_hdr_ptr->image_data_offset ) {
      free( record.data );
      free( dst_img_ptr );
      return NULL;
    }
    // validate indices (body of image data) fits in file
    if ( file_hdr_ptr->image_data_offset + height * width > record.sz ) {
      free( record.data );
      free( dst_img_ptr );
      return NULL;
    }
    uint32_t src_byte_idx = 0;
    for ( uint32_t r = 0; r < height; r++ ) {
      int dst_pixels_idx = ( height - 1 - r ) * dst_stride_sz;
      for ( uint32_t c = 0; c < width; c++ ) {
        // "most palettes are 4 bytes in RGB0 order but 3 for..." - it was actually BRG0 in old images -- Anton
        uint8_t index = src_img_ptr[src_byte_idx]; // 8-bit index value per pixel

        if ( palette_offset + index * 4 + 2 >= record.sz ) {
          free( record.data );
          return dst_img_ptr;
        }
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[index * 4 + 2];
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[index * 4 + 1]; // TODO HEAP OVERFLOW
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[index * 4 + 0];
        src_byte_idx++;
      }
      src_byte_idx += row_padding_sz;
    }

    // == 4-bpp (16-colour) -> 24-bit RGB ==
  } else if ( 4 == dib_hdr_ptr->bpp && has_palette ) {
    uint32_t src_byte_idx = 0;
    for ( uint32_t r = 0; r < height; r++ ) {
      int dst_pixels_idx = ( height - 1 - r ) * dst_stride_sz;
      for ( uint32_t c = 0; c < width; c++ ) { // Note: w/2 because 2 pixels out per column in
        if ( file_hdr_ptr->image_data_offset + src_byte_idx > record.sz ) {
          free( record.data );
          free( dst_img_ptr );
          return NULL;
        }
        // handle 2 pixels at a time
        uint8_t pixel_duo = src_img_ptr[src_byte_idx];
        uint8_t a_index   = ( 0xFF & pixel_duo ) >> 4;
        uint8_t b_index   = 0xF & pixel_duo;

        if ( palette_offset + a_index * 4 + 2 >= record.sz ) {
          free( record.data );
          return dst_img_ptr;
        }
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[a_index * 4 + 2];
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[a_index * 4 + 1];
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[a_index * 4 + 0];
        if ( ++c >= width ) { // advance a column
          c = 0;
          r++;
          dst_pixels_idx = ( height - 1 - r ) * dst_stride_sz;
        }

        if ( palette_offset + b_index * 4 + 2 >= record.sz ) {
          free( record.data );
          return dst_img_ptr;
        }
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[b_index * 4 + 2];
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[b_index * 4 + 1];
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[b_index * 4 + 0];
        src_byte_idx++;
      }
      src_byte_idx += row_padding_sz;
    }

    // == 1-bpp -> 24-bit RGB ==
  } else if ( 1 == dib_hdr_ptr->bpp && has_palette ) {
    /* encoding method for monochrome is not well documented.
    a 2x2 pixel image is stored as 4 1-bit palette indexes
    the palette is stored as any 2 RGB0 colours (not necessarily B&W)
    so for an image with indexes like so:
    1 1
    0 1
    it is bit-encoded as follows, starting at MSB:
    01000000 00000000 00000000 00000000 (first byte val  64)
    11000000 00000000 00000000 00000000 (first byte val 192)
    data is still split by row and each row padded to 4 byte multiples
     */
    uint32_t src_byte_idx = 0;
    for ( uint32_t r = 0; r < height; r++ ) {
      uint8_t bit_idx    = 0; // used in monochrome
      int dst_pixels_idx = ( height - 1 - r ) * dst_stride_sz;
      for ( uint32_t c = 0; c < width; c++ ) {
        if ( 8 == bit_idx ) { // start reading from the next byte
          src_byte_idx++;
          bit_idx = 0;
        }
        if ( file_hdr_ptr->image_data_offset + src_byte_idx > record.sz ) {
          free( record.data );
          return dst_img_ptr;
        }
        uint8_t pixel_oct   = src_img_ptr[src_byte_idx];
        uint8_t bit         = 128 >> bit_idx;
        uint8_t masked      = pixel_oct & bit;
        uint8_t palette_idx = masked > 0 ? 1 : 0;

        if ( palette_offset + palette_idx * 4 + 2 >= record.sz ) {
          free( record.data );
          return dst_img_ptr;
        }
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[palette_idx * 4 + 2];
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[palette_idx * 4 + 1];
        dst_img_ptr[dst_pixels_idx++] = palette_data_ptr[palette_idx * 4 + 0];
        bit_idx++;
      }
      src_byte_idx += row_padding_sz;
    }

    // == 24-bpp -> 24-bit RGB == (but also should handle some other n_chans cases)
  } else {
    // NOTE(Anton) this only supports 1 byte per channel
    if ( file_hdr_ptr->image_data_offset + height * width * n_dst_chans > record.sz ) {
      free( record.data );
      free( dst_img_ptr );
      return NULL;
    }
    uint32_t src_byte_idx = 0;
    for ( uint32_t r = 0; r < height; r++ ) {
      uint32_t dst_pixels_idx = ( height - 1 - r ) * dst_stride_sz;
      for ( uint32_t c = 0; c < width; c++ ) {
        // re-orders from BGR to RGB
        if ( n_dst_chans > 3 ) { dst_img_ptr[dst_pixels_idx++] = src_img_ptr[src_byte_idx + 3]; }
        if ( n_dst_chans > 2 ) { dst_img_ptr[dst_pixels_idx++] = src_img_ptr[src_byte_idx + 2]; }
        if ( n_dst_chans > 1 ) {
          if ( dst_pixels_idx >= width * height * n_dst_chans ) {
            printf( "width=%u height=%u, n_dst_chans=%u\n", width, height, n_dst_chans );
            fprintf( stderr, "ERROR: dist_pixel_idx %u at row,col %u,%u is >= max image bytes %u\n", dst_pixels_idx, r, c, width * height * n_dst_chans );
          }
          assert( dst_pixels_idx < width * height * n_dst_chans ); // assert fail here
          assert( src_byte_idx + 1 < width * height * n_dst_chans );
          dst_img_ptr[dst_pixels_idx++] = src_img_ptr[src_byte_idx + 1];
        }
        dst_img_ptr[dst_pixels_idx++] = src_img_ptr[src_byte_idx];
        src_byte_idx += n_src_chans;
      }
      src_byte_idx += row_padding_sz;
    }
  } // endif bpp

  free( record.data );
  return dst_img_ptr;
}
