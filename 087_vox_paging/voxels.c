#include "voxels.h"
#define APG_TGA_IMPLEMENTATION
#include "../common/include/apg_tga.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../common/include/stb/stb_image.h"
#include "camera.h"
#include "diamond_square.h"
#include "gl_utils.h"
#include "glcontext.h" // some GL calls/data types not encapsulated by gl_utils yet
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TODO / rough plan
* program maintains a chunks table
* chunks list has
-chunk (x,y) name/coords
-if it's loaded into memory
-if it's loadable from a file
* when loading a chunk
- is it already in memory?
- if not is it loadable from a file?
- if so then load it from within given chunks file by going to appropriate offset
- if not then generate chunk

chunks binary file:
31 -- number of chunks in file
-- table of chunk names and ~their memory offsets and size in the file
0,0 10 128
0,1 138 26
0,2 164 10
1,1
1,2 -1,0 -1,1 ...
-- first chunk's data
-- second chunk's data
...
*/

// dimensions of chunk in voxels
#define CHUNK_X 16  // 32
#define CHUNK_Y 256 // 256
#define CHUNK_Z 16  // 32

#define VOXEL_FACE_VERTS 6
#define VOXEL_VP_COMPS 3
#define VOXEL_VT_COMPS 2
#define VOXEL_VN_COMPS 4
#define VOXEL_VPICKING_COMPS 3
#define VOXEL_VPALIDX_COMPS 1
#define VOXEL_FACE_VP_FLOATS ( VOXEL_FACE_VERTS * VOXEL_VP_COMPS )
#define VOXEL_FACE_VP_BYTES ( VOXEL_FACE_VP_FLOATS * sizeof( float ) )
#define VOXEL_FACE_VT_FLOATS ( VOXEL_FACE_VERTS * VOXEL_VT_COMPS )
#define VOXEL_FACE_VT_BYTES ( VOXEL_FACE_VT_FLOATS * sizeof( float ) )
#define VOXEL_FACE_VN_FLOATS ( VOXEL_FACE_VERTS * VOXEL_VN_COMPS )
#define VOXEL_FACE_VN_BYTES ( VOXEL_FACE_VN_FLOATS * sizeof( float ) )
#define VOXEL_FACE_VPICKING_FLOATS ( VOXEL_FACE_VERTS * VOXEL_VPICKING_COMPS )
#define VOXEL_FACE_VPICKING_BYTES ( VOXEL_FACE_VPICKING_FLOATS * sizeof( float ) )
#define VOXEL_FACE_VPALIDX_UINTS ( VOXEL_FACE_VERTS * VOXEL_VPALIDX_COMPS )
#define VOXEL_FACE_VPALIDX_BYTES ( VOXEL_FACE_VPALIDX_UINTS * sizeof( uint32_t ) )
#define VOXEL_CUBE_VP_FLOATS ( VOXEL_FACE_VP_FLOATS * 6 )
#define VOXEL_CUBE_VP_BYTES ( VOXEL_FACE_VP_BYTES * 6 )
#define VOXEL_CUBE_VT_FLOATS ( VOXEL_FACE_VT_FLOATS * 6 )
#define VOXEL_CUBE_VT_BYTES ( VOXEL_FACE_VT_BYTES * 6 )
#define VOXEL_CUBE_VN_FLOATS ( VOXEL_FACE_VN_FLOATS * 6 )
#define VOXEL_CUBE_VN_BYTES ( VOXEL_CUBE_VN_FLOATS * sizeof( float ) )
#define VOXEL_CUBE_VPICKING_FLOATS ( VOXEL_FACE_VPICKING_FLOATS * 6 )
#define VOXEL_CUBE_VPICKING_BYTES ( VOXEL_FACE_VPICKING_BYTES * 6 )
#define VOXEL_CUBE_VPALIDX_UINTS ( VOXEL_FACE_VPALIDX_UINTS * 6 )
#define VOXEL_CUBE_VPALIDX_BYTES ( VOXEL_FACE_VPALIDX_BYTES * 6 )

#pragma pack( push, 1 )
typedef struct voxel_t {
  uint8_t type;
} voxel_t;

typedef struct chunk_t {
  voxel_t* voxels;
  int heightmap[CHUNK_X * CHUNK_Z];
  uint32_t n_non_air_voxels;
} chunk_t;
#pragma pack( pop )

typedef struct chunk_vertex_data_t {
  float* positions_ptr;
  float* texcoords_ptr;
  float* picking_ptr; // only needs 3 bytes
  float* normals_ptr;
  uint32_t* palette_indices_ptr;
  size_t n_vertices;
  size_t n_vp_comps;
  size_t n_vt_comps;
  size_t n_vpalidx_comps;
  size_t n_vpicking_comps;
  size_t n_vn_comps;
  size_t vp_buffer_sz;
  size_t vt_buffer_sz;
  size_t vn_buffer_sz;
  size_t vpalidx_buffer_sz;
  size_t vpicking_buffer_sz;
} chunk_vertex_data_t;

// clang-format off
//                                     x   y   z   x   y   z   x   y   z | x   y   z   x   y   z   x   y   z
static const float _west_face[]   = { -1,  1, -1, -1, -1, -1, -1,  1,  1, -1,  1,  1, -1, -1, -1, -1, -1,  1 };
static const float _east_face[]   = {  1,  1,  1,  1, -1,  1,  1,  1, -1,  1,  1, -1,  1, -1,  1,  1, -1, -1 };
static const float _bottom_face[] = { -1, -1,  1, -1, -1, -1,  1, -1,  1,  1, -1,  1, -1, -1, -1,  1, -1, -1 };
static const float _top_face[]    = { -1,  1, -1, -1,  1,  1,  1,  1, -1,  1,  1, -1, -1,  1,  1,  1,  1,  1 };
static const float _north_face[]  = {  1,  1, -1,  1, -1, -1, -1,  1, -1, -1,  1, -1,  1, -1, -1, -1, -1, -1 };
static const float _south_face[]  = { -1,  1,  1, -1, -1,  1,  1,  1,  1,  1,  1,  1, -1, -1,  1,  1, -1,  1 };
// clang-format on

static const int palette_grass = 0;
static const int palette_stone = 1;
static const int palette_dirt  = 2;
static const int palette_crust = 3;

// might be handy to keep as a reference
#if 0
/* creates vertex data for a cube made from 6 individual faces */
static chunk_vertex_data_t _test_cube_from_faces() {
  chunk_vertex_data_t cube = ( chunk_vertex_data_t ){
    .n_vp_comps = VOXEL_VP_COMPS, .n_vt_comps = VOXEL_VT_COMPS, .n_vpalidx_comps = VOXEL_VPALIDX_COMPS, .n_vn_comps = VOXEL_VN_COMPS, .n_vpicking_comps = VOXEL_VPICKING_COMPS
  };
  cube.positions_ptr = malloc( VOXEL_CUBE_VP_BYTES );
  if ( !cube.positions_ptr ) { return cube; }
  cube.palette_indices_ptr = malloc( VOXEL_CUBE_VPALIDX_BYTES );
  if ( !cube.palette_indices_ptr ) {
    free( cube.positions_ptr );
    cube.positions_ptr = NULL;
    return cube;
  }

  int float_idx                     = 0;
  int palette_indices_idx           = 0;
  const float* faces[6]             = { _west_face, _east_face, _bottom_face, _top_face, _north_face, _south_face };
  const uint32_t palette_indices[6] = { 0, 1, 2, 3, 4, 5 };
  for ( int face_idx = 0; face_idx < 6; face_idx++ ) {
    memcpy( &cube.positions_ptr[float_idx], faces[face_idx], VOXEL_FACE_VP_BYTES );
    for ( int i = 0; i < VOXEL_FACE_VERTS; i++ ) {
      memcpy( &cube.palette_indices_ptr[palette_indices_idx + i], &palette_indices[face_idx], sizeof( uint32_t ) );
    }
    float_idx += VOXEL_FACE_VP_FLOATS;
    palette_indices_idx += VOXEL_FACE_VERTS;
    cube.n_vertices += VOXEL_FACE_VERTS;
  }

  return cube;
}
#endif

static bool _set_block_type_in_chunk( chunk_t* chunk, int x, int y, int z, block_type_t type ) {
  assert( chunk && chunk->voxels );

  bool changed = false;

  if ( x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z ) { return changed; }

  int idx = CHUNK_X * CHUNK_Z * y + CHUNK_X * z + x;

  block_type_t prev_type = chunk->voxels[idx].type;
  if ( prev_type == type ) { return changed; } // no change!
  if ( prev_type == BLOCK_TYPE_AIR && type != BLOCK_TYPE_AIR ) { chunk->n_non_air_voxels++; }
  if ( prev_type != BLOCK_TYPE_AIR && type == BLOCK_TYPE_AIR ) {
    assert( chunk->n_non_air_voxels > 0 );
    chunk->n_non_air_voxels--;
  }
  chunk->voxels[idx].type = type;

  int prev_height = chunk->heightmap[CHUNK_X * z + x];
  if ( y > prev_height && type != BLOCK_TYPE_AIR ) { // higher than before
    chunk->heightmap[CHUNK_X * z + x] = y;
  } else if ( y == prev_height && type == BLOCK_TYPE_AIR ) { // lowering
    for ( int yy = y; yy >= 0; yy-- ) {
      int idx = CHUNK_X * CHUNK_Z * yy + CHUNK_X * z + x;
      if ( chunk->voxels[idx].type != BLOCK_TYPE_AIR ) {
        chunk->heightmap[CHUNK_X * z + x] = yy;
        break;
      }
    }
  }

  assert( chunk->n_non_air_voxels <= CHUNK_X * CHUNK_Y * CHUNK_Z );

  changed = true;
  return changed;
}

static bool _get_block_type_in_chunk( const chunk_t* chunk, int x, int y, int z, block_type_t* block_type ) {
  assert( chunk && chunk->voxels && block_type );
  if ( x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z ) { return false; }

  int idx     = CHUNK_X * CHUNK_Z * y + CHUNK_X * z + x;
  *block_type = chunk->voxels[idx].type;
  return true;
}

static bool _is_voxel_above_surface( const chunk_t* chunk, int x, int y, int z ) {
  assert( chunk && chunk->voxels );
  // edges of chunk will be lit by sunlight
  if ( x < 0 || x >= CHUNK_X || y < 0 || y >= CHUNK_Y || z < 0 || z >= CHUNK_Z ) { return true; }

  int idx    = CHUNK_X * z + x;
  int height = chunk->heightmap[idx];
  if ( y > height ) { return true; }
  return false;
}

static bool _is_voxel_face_exposed_to_sun( const chunk_t* chunk, int x, int y, int z, int face_idx ) {
  assert( chunk && chunk->voxels );

  switch ( face_idx ) {
  case 0: return _is_voxel_above_surface( chunk, x - 1, y, z );
  case 1: return _is_voxel_above_surface( chunk, x + 1, y, z );
  case 2: return _is_voxel_above_surface( chunk, x, y - 1, z );
  case 3: return _is_voxel_above_surface( chunk, x, y + 1, z );
  case 4: return _is_voxel_above_surface( chunk, x, y, z - 1 );
  case 5: return _is_voxel_above_surface( chunk, x, y, z + 1 );
  default: assert( false ); break;
  }
  return false;
}

// TODO ifdef write_heightmap img
// PARAMS
// - hm_dims - square heightmap so use width or height in pixels here
static chunk_t _chunk_generate( const uint8_t* heightmap, int hm_dims, int x_offset, int z_offset ) {
  assert( heightmap );

  chunk_t chunk;
  memset( &chunk, 0, sizeof( chunk_t ) );
  chunk.voxels = calloc( CHUNK_X * CHUNK_Y * CHUNK_Z, sizeof( voxel_t ) );
  assert( chunk.voxels );

  for ( int z = 0; z < CHUNK_Z; z++ ) {
    for ( int x = 0; x < CHUNK_X; x++ ) {
      // uint8_t height = _noisei( x, z, CHUNK_Y - 1 ); // TODO add position of chunk to x and y for continuous/infinite map

      int xx                       = x_offset + x;
      int zz                       = z_offset + z;
      int idx                      = hm_dims * zz + xx; // uses offsets and width/height of map
      const int underground_height = 16;
      const int heightmap_sample   = heightmap[idx]; // uint8_t to int to avoid overflow when adding a hm sample of 255 to underground height > 0

      uint8_t height = CLAMP( heightmap_sample + underground_height, 1, CHUNK_Y - 1 ); // -1 because chunk_y is 256 which would wrap around to 0 in a uint8

      _set_block_type_in_chunk( &chunk, x, height, z, BLOCK_TYPE_GRASS );
      _set_block_type_in_chunk( &chunk, x, height - 1, z, BLOCK_TYPE_DIRT );
      for ( int y = height - 2; y > 0; y-- ) { _set_block_type_in_chunk( &chunk, x, y, z, BLOCK_TYPE_STONE ); }
      _set_block_type_in_chunk( &chunk, x, 0, z, BLOCK_TYPE_CRUST );
    } // x
  }   // z

  return chunk;
}

#if 0
static chunk_t _chunk_generate_flat() {
  chunk_t chunk;

  memset( &chunk, 0, sizeof( chunk_t ) );
  chunk.voxels = calloc( CHUNK_X * CHUNK_Y * CHUNK_Z, sizeof( voxel_t ) );
  assert( chunk.voxels );

  for ( int z = 0; z < CHUNK_Z; z++ ) {
    for ( int x = 0; x < CHUNK_X; x++ ) {
      uint8_t height = 0;
      _set_block_type_in_chunk( &chunk, x, height, z, BLOCK_TYPE_CRUST );
    } // x
  }   // z

  return chunk;
}

static bool chunk_write_heightmap( const char* filename, const chunk_t* chunk ) {
  assert( filename && chunk && chunk->voxels );
  uint8_t* img = malloc( CHUNK_X * CHUNK_Z * 3 );
  for ( int z = 0; z < CHUNK_Z; z++ ) {
    for ( int x = 0; x < CHUNK_X; x++ ) {
      img[( CHUNK_X * z + x ) * 3] = img[( CHUNK_X * z + x ) * 3 + 1] = img[( CHUNK_X * z + x ) * 3 + 2] = chunk->heightmap[CHUNK_X * z + x];
    }
  }
  bool result = (bool)apg_tga_write_file( filename, img, CHUNK_X, CHUNK_Z, 3 );
  free( img );
  return result;
}
#endif

static void chunk_free( chunk_t* chunk ) {
  assert( chunk && chunk->voxels );

  free( chunk->voxels );
  memset( chunk, 0, sizeof( chunk_t ) );
}

static void _memcpy_face_palidx( block_type_t block_type, uint32_t* dest ) {
  assert( dest );

  uint32_t palidx = 0;
  switch ( block_type ) {
  case BLOCK_TYPE_CRUST: {
    palidx = palette_crust;
  } break;
  case BLOCK_TYPE_GRASS: {
    palidx = palette_grass;
  } break;
  case BLOCK_TYPE_DIRT: {
    palidx = palette_dirt;
  } break;
  case BLOCK_TYPE_STONE: {
    palidx = palette_stone;
  } break;
  default: {
    assert( false );
  } break;
  }
  // per the 6 vertices in this face
  for ( int v = 0; v < VOXEL_FACE_VERTS; v++ ) { *( dest + v ) = palidx; }
}

static void _memcpy_face_picking( int x, int y, int z, int face_idx, float* dest, size_t curr_float, size_t max_floats ) {
  uint32_t a    = z * CHUNK_X + x; // up 0-255 (16*16). to retrieve: z= a/CHUNK_X, x=a%CHUNK_X (integer rounding maths)
  uint32_t b    = y;               // up 0-255
  uint32_t c    = face_idx;        // up 0-5
  float buff[3] = { a / 255.0f, b / 255.0f, c / 255.0f };
  // per the 6 vertices in this face
  assert( curr_float + VOXEL_VPICKING_COMPS * VOXEL_FACE_VERTS < max_floats );
  for ( int v = 0; v < VOXEL_FACE_VERTS; v++ ) { memcpy( &dest[curr_float + v * VOXEL_VPICKING_COMPS], buff, sizeof( float ) * VOXEL_VPICKING_COMPS ); }
}

static void _memcpy_face_texcoords( const chunk_t* chunk, float* dest, size_t curr_float, size_t max_floats ) {
  assert( chunk );

  const float buff[] = { 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0 };
  assert( curr_float + VOXEL_VT_COMPS * VOXEL_FACE_VERTS < max_floats );
  memcpy( &dest[curr_float], buff, sizeof( float ) * VOXEL_VT_COMPS * VOXEL_FACE_VERTS );
}

static void _memcpy_face_normals( const chunk_t* chunk, int x, int y, int z, int face_idx, float* dest, size_t curr_float, size_t max_floats ) {
  assert( chunk );

  float buff[4] = { 0, 0, 0, 0 };
  switch ( face_idx ) {
  case 0: buff[0] = -1.0f; break;
  case 1: buff[0] = 1.0f; break;
  case 2: buff[1] = -1.0f; break;
  case 3: buff[1] = 1.0f; break;
  case 4: buff[2] = -1.0f; break;
  case 5: buff[2] = 1.0f; break;
  default: assert( false ); break;
  }
  bool sunlit = _is_voxel_face_exposed_to_sun( chunk, x, y, z, face_idx );
  if ( sunlit ) { buff[3] = 1.0f; }
  assert( curr_float + VOXEL_VN_COMPS * VOXEL_FACE_VERTS < max_floats );
  for ( int v = 0; v < VOXEL_FACE_VERTS; v++ ) { memcpy( &dest[curr_float + v * VOXEL_VN_COMPS], buff, sizeof( float ) * VOXEL_VN_COMPS ); }
}

static chunk_vertex_data_t chunk_gen_vertex_data( const chunk_t* chunk, int from_y_inclusive, int to_y_exclusive ) {
  assert( chunk );
  assert( from_y_inclusive >= 0 && to_y_exclusive <= CHUNK_Y );

  chunk_vertex_data_t data = ( chunk_vertex_data_t ){
    .n_vp_comps = VOXEL_VP_COMPS, .n_vt_comps = VOXEL_VT_COMPS, .n_vpicking_comps = VOXEL_VPICKING_COMPS, .n_vn_comps = VOXEL_VN_COMPS, .n_vpalidx_comps = VOXEL_VPALIDX_COMPS
  };
  data.vp_buffer_sz       = VOXEL_CUBE_VP_BYTES * chunk->n_non_air_voxels;
  data.vn_buffer_sz       = VOXEL_CUBE_VN_BYTES * chunk->n_non_air_voxels;
  data.vt_buffer_sz       = VOXEL_CUBE_VT_BYTES * chunk->n_non_air_voxels;
  data.vpicking_buffer_sz = VOXEL_CUBE_VPICKING_BYTES * chunk->n_non_air_voxels;
  data.vpalidx_buffer_sz  = VOXEL_CUBE_VPALIDX_BYTES * chunk->n_non_air_voxels;
  data.positions_ptr      = malloc( data.vp_buffer_sz );
  assert( data.positions_ptr );
  data.palette_indices_ptr = malloc( data.vpalidx_buffer_sz );
  assert( data.palette_indices_ptr );
  data.picking_ptr = malloc( data.vpicking_buffer_sz );
  assert( data.picking_ptr );
  data.texcoords_ptr = malloc( data.vt_buffer_sz );
  assert( data.texcoords_ptr );
  data.normals_ptr = malloc( data.vn_buffer_sz );
  assert( data.normals_ptr );

  uint32_t total_vp_floats  = 0;
  uint32_t total_vt_floats  = 0;
  uint32_t total_vn_floats  = 0;
  uint32_t total_palindices = 0;

  for ( int y = from_y_inclusive; y < to_y_exclusive; y++ ) {
    for ( int z = 0; z < CHUNK_Z; z++ ) {
      for ( int x = 0; x < CHUNK_X; x++ ) {
        block_type_t our_block_type = 0;
        bool ret                    = _get_block_type_in_chunk( chunk, x, y, z, &our_block_type );
        int floats_added_this_block = 0;
        assert( ret );
        if ( our_block_type == BLOCK_TYPE_AIR ) { continue; }
        assert( total_vp_floats < VOXEL_CUBE_VP_FLOATS * chunk->n_non_air_voxels );

        {
          // clang-format off
          int xs[6]             = { -1,  1,  0,  0,  0,  0 };
          int ys[6]             = {  0,  0, -1,  1,  0,  0 };
          int zs[6]             = {  0,  0,  0,  0, -1,  1 };
          // clang-format on
          const float* faces[6] = { _west_face, _east_face, _bottom_face, _top_face, _north_face, _south_face };
          block_type_t neighbour_block_type;
          for ( int face_idx = 0; face_idx < 6; face_idx++ ) {
            bool ret = _get_block_type_in_chunk( chunk, x + xs[face_idx], y + ys[face_idx], z + zs[face_idx], &neighbour_block_type );
            // if face is valid then add one face's worth of vertex data to the buffer
            if ( !ret || neighbour_block_type == BLOCK_TYPE_AIR ) {
              memcpy( &data.positions_ptr[total_vp_floats + floats_added_this_block], faces[face_idx], VOXEL_FACE_VP_BYTES );
              _memcpy_face_palidx( our_block_type, &data.palette_indices_ptr[total_palindices] );
              _memcpy_face_picking( x, y, z, face_idx, data.picking_ptr, total_vp_floats + floats_added_this_block, VOXEL_CUBE_VPICKING_FLOATS * chunk->n_non_air_voxels );
              _memcpy_face_texcoords( chunk, data.texcoords_ptr, total_vt_floats, VOXEL_CUBE_VT_FLOATS * chunk->n_non_air_voxels );
              _memcpy_face_normals( chunk, x, y, z, face_idx, data.normals_ptr, total_vn_floats, VOXEL_CUBE_VN_FLOATS * chunk->n_non_air_voxels );
              floats_added_this_block += VOXEL_FACE_VP_FLOATS;
              total_vt_floats += VOXEL_FACE_VT_FLOATS;
              total_vn_floats += VOXEL_FACE_VN_FLOATS;
              total_palindices += VOXEL_FACE_VPALIDX_UINTS;
            }
          }
        }

        // assumes xyz positions are first three bytes per vertex
        for ( int i = 0; i < floats_added_this_block; i += 3 ) {
          data.positions_ptr[total_vp_floats + i + 0] += x * 2.0f;
          data.positions_ptr[total_vp_floats + i + 1] += y * 2.0f;
          data.positions_ptr[total_vp_floats + i + 2] += z * 2.0f;
        }
        total_vp_floats += floats_added_this_block;
      } // endfor x
    }   // endfor z
  }     // endfor y

  data.n_vp_comps         = VOXEL_VP_COMPS;
  data.n_vertices         = total_vp_floats / VOXEL_VP_COMPS;
  data.vp_buffer_sz       = total_vp_floats * sizeof( float );
  data.vt_buffer_sz       = total_vt_floats * sizeof( float );
  data.vn_buffer_sz       = total_vn_floats * sizeof( float );
  data.vpicking_buffer_sz = total_vp_floats * sizeof( float ); // NOTE(Anton) same as vp
  data.vpalidx_buffer_sz  = total_palindices * sizeof( uint32_t );
  // TODO(Anton) add a tmp var to check for realloc success for each
  data.positions_ptr = realloc( data.positions_ptr, data.vp_buffer_sz );
  assert( data.positions_ptr );
  data.palette_indices_ptr = realloc( data.palette_indices_ptr, data.vpalidx_buffer_sz );
  assert( data.palette_indices_ptr );
  data.picking_ptr = realloc( data.picking_ptr, data.vpicking_buffer_sz );
  assert( data.picking_ptr );
  data.texcoords_ptr = realloc( data.texcoords_ptr, data.vt_buffer_sz );
  assert( data.texcoords_ptr );
  data.normals_ptr = realloc( data.normals_ptr, data.vn_buffer_sz );
  assert( data.normals_ptr );

  return data;
}

static void chunk_free_vertex_data( chunk_vertex_data_t* chunk_vertex_data ) {
  assert( chunk_vertex_data && chunk_vertex_data->positions_ptr );

  free( chunk_vertex_data->positions_ptr );
  free( chunk_vertex_data->palette_indices_ptr );
  free( chunk_vertex_data->picking_ptr );
  free( chunk_vertex_data->texcoords_ptr );
  free( chunk_vertex_data->normals_ptr );
  memset( chunk_vertex_data, 0, sizeof( chunk_vertex_data_t ) );
}

/*-------------------------------------------------CHUNKS ORGANISATION-----------------------------------------------------*/

// total number of chunks
#define CHUNKS_N 256
#define VOXEL_SCALE 0.2f

// generated graphics stuff that doesn't persist between save/load
static bool _dirty_chunks[CHUNKS_N];
static mesh_t _chunk_meshes[CHUNKS_N];
static mat4 _chunks_M[CHUNKS_N];
static shader_t _voxel_shader;
static shader_t _colour_picking_shader;
static texture_t _array_texture;

// struct of world state that would be saved/loaded from a file
typedef struct chunks_world_t {
  chunk_t _chunks[CHUNKS_N];
  int _chunks_w, _chunks_h;
  uint32_t seed;
  bool chunks_created;
  bool slice_view_mode;
} chunks_world_t;

static chunks_world_t _g_chunks_world;

bool chunks_create( uint32_t seed, uint32_t chunks_wide, uint32_t chunks_deep ) {
  assert( chunks_wide == chunks_deep ); // current limitation

  if ( _g_chunks_world.chunks_created ) { return false; } // free first

  _g_chunks_world._chunks_w = chunks_wide;
  _g_chunks_world._chunks_h = chunks_deep;
  _g_chunks_world.seed      = seed;

  dsquare_heightmap_t dshm;
  {
    srand( seed );
    assert( CHUNK_X == CHUNK_Z );
    const int default_height     = 63;
    const int noise_scale        = 64;
    const int feature_spread     = 64;
    const int feature_max_height = 64;
    dshm                         = dsquare_heightmap_alloc( CHUNK_X * _g_chunks_world._chunks_w, default_height );
    dsquare_heightmap_gen( &dshm, noise_scale, feature_spread, feature_max_height );
  }

  for ( int cz = 0; cz < _g_chunks_world._chunks_h; cz++ ) {
    for ( int cx = 0; cx < _g_chunks_world._chunks_w; cx++ ) {
      const int idx                = cz * _g_chunks_world._chunks_w + cx;
      _g_chunks_world._chunks[idx] = _chunk_generate( dshm.filtered_heightmap, dshm.w, cx * CHUNK_X, cz * CHUNK_Z );
      {
        chunk_vertex_data_t vertex_data = chunk_gen_vertex_data( &_g_chunks_world._chunks[idx], 0, CHUNK_Y );
        _chunk_meshes[idx]              = create_mesh_from_mem( vertex_data.positions_ptr, vertex_data.n_vp_comps, vertex_data.palette_indices_ptr,
          vertex_data.n_vpalidx_comps, vertex_data.picking_ptr, vertex_data.n_vpicking_comps, vertex_data.texcoords_ptr, vertex_data.n_vt_comps,
          vertex_data.normals_ptr, vertex_data.n_vn_comps, NULL, 0, vertex_data.n_vertices );
        chunk_free_vertex_data( &vertex_data );
      }
      _chunks_M[idx] = translate_mat4( ( vec3 ){ .x = cx * CHUNK_X * VOXEL_SCALE, .z = cz * CHUNK_Z * VOXEL_SCALE } );
    }
  }

  {
    const char vert_shader_str[] = {
      "#version 410\n"
      "in vec3 a_vp;\n"
      "in vec2 a_vt;\n"
      "in vec4 a_vn;\n"
      "in uint a_vpal_idx;\n"
      "uniform mat4 u_P, u_V, u_M;\n"
      "out vec2 v_st;\n"
      "out vec4 v_n;\n"
      "out vec3 v_p_eye;\n"
      "flat out uint v_vpal_idx;\n"
      "void main () {\n"
      "  v_vpal_idx = a_vpal_idx;\n"
      "  v_st = a_vt;\n"
      "  v_n.xyz = (u_M * vec4( a_vn.xyz, 0.0 )).xyz;\n"
      "  v_n.w = a_vn.w;\n"
      "  vec4 p_wor = u_M * vec4( a_vp * 0.1, 1.0 );\n"
      "  v_p_eye =  ( u_V * p_wor ).xyz;\n"
      "  gl_Position = u_P * vec4( v_p_eye, 1.0 );\n"
      // "  gl_ClipDistance[0] = dot( p_wor, vec4( 0.0, -1.0, 0.0, 5.0 ) );\n" // okay if below 10
      // "  gl_ClipDistance[1] = dot( p_wor, vec4( 0.0, 1.0, 0.0, -2.0 ) );\n" // okay if above 2
      "}\n"
    };
    // heightmap 0 or 1 factor is stored in normal's w channel. used to disable sunlight for rooms/caves/overhangs (assumes sun is always _directly_ overhead,
    // pointing down)
    const char frag_shader_str[] = {
      "#version 410\n"
      "in vec2 v_st;\n"
      "in vec4 v_n;\n"
      "in vec3 v_p_eye;\n"
      "flat in uint v_vpal_idx;\n"
      "uniform sampler2DArray u_palette_texture;\n"
      "uniform vec3 u_fwd;\n"
      "out vec4 o_frag_colour;\n"
      "vec3 sun_rgb = vec3( 1.0, 1.0, 1.0 );\n"
      "vec3 fwd_rgb = vec3( 1.0, 1.0, 1.0 );\n"
      "vec3 fog_rgb = vec3( 0.5, 0.5, 0.9 );\n"
      "void main () {\n"
      "  vec3 texel_rgb    = texture( u_palette_texture, vec3( v_st.s, 1.0 - v_st.t, v_vpal_idx ) ).rgb;\n"
      "  float fog_fac      = clamp( v_p_eye.z * v_p_eye.z / 500.0, 0.0, 1.0 );\n"
      "  vec3 col           = pow( texel_rgb, vec3( 2.2 ) ); \n" // tga image load is linear colour space already w/o gamma
      "  float sun_dp       = clamp( dot( normalize( v_n.xyz ), normalize( -vec3( -0.3, -1.0, 0.2 ) ) ), 0.0 , 1.0 );\n"
      "  float fwd_dp       = clamp( dot( normalize( v_n.xyz ), -u_fwd ), 0.0, 1.0 );\n"
      "  float outdoors_fac = v_n.w;\n"
      "  o_frag_colour      = vec4(sun_rgb * col * sun_dp * outdoors_fac * 0.9 + (fwd_dp * 0.75 + 0.25) * col * 0.1, 1.0f );\n"
      "  o_frag_colour.rgb  = pow( o_frag_colour.rgb, vec3( 1.0 / 2.2 ) );\n"
      "  o_frag_colour.rgb  = mix(o_frag_colour.rgb, fog_rgb, fog_fac);\n"
      "}\n"
    };
    // NOTE(Anton) fog calc after rgb because colour is trying to match gl clear colour
    _voxel_shader = create_shader_program_from_strings( vert_shader_str, frag_shader_str );
  }

  {
    const char vert_shader_str[] = {
      "#version 410\n"
      "in vec3 a_vp;\n"
      "in vec3 a_vpicking;\n"
      "uniform mat4 u_P, u_V, u_M;\n"
      "out vec3 v_vpicking;\n"
      "void main () {\n"
      "  v_vpicking = a_vpicking;\n"
      "  gl_Position = u_P * u_V * u_M * vec4( a_vp * 0.1, 1.0 );\n"
      "}\n"
    };
    const char frag_shader_str[] = {
      "#version 410\n"
      "in vec3 v_vpicking;\n"
      "uniform float u_chunk_id;\n"
      "out vec4 o_frag_colour;\n"
      "void main () {\n"
      "  o_frag_colour = vec4( v_vpicking, u_chunk_id );\n"
      "}\n"
    };
    _colour_picking_shader = create_shader_program_from_strings( vert_shader_str, frag_shader_str );
  }

  {
    const char images[16][256] = { "textures/grass.png", "textures/slab.png", "textures/side_grass.png", "textures/hersk-export.png" };
    GLsizei layerCount         = 4;
    GLsizei mipLevelCount      = 5;

    _array_texture = ( texture_t ){ .handle_gl = 0, .w = 16, .h = 16, .n_channels = 3, .srgb = false, .is_depth = false, .is_array = true };

    glGenTextures( 1, &_array_texture.handle_gl );
    glBindTexture( GL_TEXTURE_2D_ARRAY, _array_texture.handle_gl );
    // allocate memory for all layers
    glTexStorage3D( GL_TEXTURE_2D_ARRAY, mipLevelCount, GL_RGB8, _array_texture.w, _array_texture.h, layerCount ); // 8?
    glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
    for ( int i = 0; i < layerCount; i++ ) {
      int w, h, n;
      uint8_t* img = stbi_load( images[i], &w, &h, &n, 3 );
      if ( !img ) { return false; }
      assert( img );
      assert( w == _array_texture.w && h == _array_texture.h );

      glTexSubImage3D( GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, w, h, 1, GL_RGB, GL_UNSIGNED_BYTE, img ); // note that 'depth' param must be 1
      free( img );
    }
    // once all textures loaded at mip 0 - generate other mipmaps
    glTexParameteri( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR ); // nearest but with 2-linear mipmap blend
    glTexParameterf( GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, 4.0f );
    glGenerateMipmap( GL_TEXTURE_2D_ARRAY );
    glBindTexture( GL_TEXTURE_2D_ARRAY, 0 );
  }

  dsquare_heightmap_free( &dshm );
  _g_chunks_world.chunks_created = true;

  return true;
}

bool chunks_free() {
  assert( _g_chunks_world.chunks_created );
  if ( !_g_chunks_world.chunks_created ) { return false; }

  delete_shader_program( &_voxel_shader );
  delete_shader_program( &_colour_picking_shader );
  delete_texture( &_array_texture );

  for ( int i = 0; i < CHUNKS_N; i++ ) {
    chunk_free( &_g_chunks_world._chunks[i] );
    delete_mesh( &_chunk_meshes[i] );
  }
  _g_chunks_world.chunks_created = false;

  return true;
}

typedef struct chunk_queue_item_t {
  float sqdist;    // used as key in sorting
  int idx;         // index into chunks arrays
  vec3 mins, maxs; // used in frustum culling
} chunk_queue_item_t;

static chunk_queue_item_t _chunk_draw_queue[CHUNKS_N];

static int _chunk_cmp( const void* a, const void* b ) {
  chunk_queue_item_t* ptr_a = (chunk_queue_item_t*)a;
  chunk_queue_item_t* ptr_b = (chunk_queue_item_t*)b;
  return (int)( ptr_a->sqdist - ptr_b->sqdist );
}

void chunks_sort_draw_queue( vec3 cam_pos ) {
  // sort chunks by distance from camera - render closest first
  for ( int i = 0; i < CHUNKS_N; i++ ) {
    _chunk_draw_queue[i].idx = i;
    int chunk_x              = i % _g_chunks_world._chunks_w;
    int chunk_z              = i / _g_chunks_world._chunks_w;
    vec2 cam_centre          = ( vec2 ){ .x = cam_pos.x, .y = cam_pos.z };
    vec2 chunk_centre =
      ( vec2 ){ .x = chunk_x * CHUNK_X * VOXEL_SCALE + CHUNK_X * VOXEL_SCALE * 0.5f, .y = chunk_z * CHUNK_Z * VOXEL_SCALE + CHUNK_Z * VOXEL_SCALE * 0.5f };
    _chunk_draw_queue[i].sqdist = length2_vec2( sub_vec2_vec2( cam_centre, chunk_centre ) );
    _chunk_draw_queue[i].mins   = ( vec3 ){ chunk_centre.x - CHUNK_X * VOXEL_SCALE * 0.5f, 0.0f, chunk_centre.y - CHUNK_Z * VOXEL_SCALE * 0.5f };
    _chunk_draw_queue[i].maxs = ( vec3 ){ chunk_centre.x + CHUNK_X * VOXEL_SCALE * 0.5f, CHUNK_Y * VOXEL_SCALE, chunk_centre.y + CHUNK_Z * VOXEL_SCALE * 0.5f };
  }
  qsort( _chunk_draw_queue, CHUNKS_N, sizeof( chunk_queue_item_t ), _chunk_cmp );
}

static int _chunks_drawn;
static int _chunks_max_drawn        = 128;
static int _chunks_max_visible_dist = 10;

int chunks_get_drawn_count() { return _chunks_drawn; }

void chunks_draw( vec3 cam_fwd, mat4 P, mat4 V ) {
  assert( _g_chunks_world.chunks_created );
  if ( !_g_chunks_world.chunks_created ) { return; }

  _chunks_drawn        = 0;
  const float max_dist = (float)_chunks_max_visible_dist * 16 * VOXEL_SCALE;

  uniform3f( _voxel_shader, _voxel_shader.u_fwd, cam_fwd.x, cam_fwd.y, cam_fwd.z );
  for ( int i = 0; i < CHUNKS_N; i++ ) {
    int idx = _chunk_draw_queue[i].idx;
    if ( _chunk_draw_queue[i].sqdist > max_dist * max_dist ) { return; }
    if ( !is_aabb_in_frustum( _chunk_draw_queue[i].mins, _chunk_draw_queue[i].maxs ) ) { continue; }
    draw_mesh( _voxel_shader, P, V, _chunks_M[idx], _chunk_meshes[idx].vao, _chunk_meshes[idx].n_vertices, &_array_texture, 1 );
    _chunks_drawn++;
    if ( _chunks_drawn >= _chunks_max_drawn ) { return; }
  }
}

void chunks_draw_colour_picking( mat4 offcentre_P, mat4 V ) {
  assert( _g_chunks_world.chunks_created );
  if ( !_g_chunks_world.chunks_created ) { return; }

  int local_chunks_drawn = 0; // needs to be a separate var from the one in chunks_draw
  const float max_dist   = (float)_chunks_max_visible_dist * 16 * VOXEL_SCALE;

  for ( uint32_t i = 0; i < CHUNKS_N; i++ ) {
    int idx = _chunk_draw_queue[i].idx;
    if ( _chunk_draw_queue[i].sqdist > max_dist * max_dist ) { return; }
    if ( !is_aabb_in_frustum( _chunk_draw_queue[i].mins, _chunk_draw_queue[i].maxs ) ) { continue; }
    uniform1f( _colour_picking_shader, _colour_picking_shader.u_chunk_id, (float)idx / 255.0f );
    draw_mesh( _colour_picking_shader, offcentre_P, V, _chunks_M[idx], _chunk_meshes[idx].vao, _chunk_meshes[idx].n_vertices, NULL, 0 );
    if ( local_chunks_drawn >= _chunks_max_drawn ) { return; }
    local_chunks_drawn++;
  }
}

bool chunks_picked_colour_to_voxel_idx( uint8_t r, uint8_t g, uint8_t b, uint8_t a, int* x, int* y, int* z, int* face, int* chunk_id ) {
  if ( b > 5 ) { return false; }
  *x        = (int)r % CHUNK_X;
  *y        = (int)g;
  *z        = (int)r / CHUNK_X;
  *face     = (int)b;
  *chunk_id = (int)a;
  assert( *chunk_id >= 0 && *chunk_id < CHUNKS_N );
  return true;
}

bool chunks_get_block_type_in_chunk( int chunk_id, int x, int y, int z, block_type_t* block_type ) {
  assert( chunk_id >= 0 && chunk_id < CHUNKS_N );

  bool ret = _get_block_type_in_chunk( &_g_chunks_world._chunks[chunk_id], x, y, z, block_type );
  return ret;
}

bool chunks_set_block_type_in_chunk( int chunk_id, int x, int y, int z, block_type_t block_type ) {
  assert( chunk_id >= 0 && chunk_id < CHUNKS_N );

  bool ret = _set_block_type_in_chunk( &_g_chunks_world._chunks[chunk_id], x, y, z, block_type );
  return ret;
}

bool chunks_create_block_on_face( int picked_chunk_id, int picked_x, int picked_y, int picked_z, int picked_face, block_type_t type ) {
  assert( picked_chunk_id >= 0 && picked_chunk_id < CHUNKS_N );

  int xx = picked_x, yy = picked_y, zz = picked_z;
  switch ( picked_face ) {
  case 0: xx--; break;
  case 1: xx++; break;
  case 2: yy--; break;
  case 3: yy++; break;
  case 4: zz--; break;
  case 5: zz++; break;
  default: assert( false ); break;
  }

  int chunk_x = picked_chunk_id % _g_chunks_world._chunks_w;
  int chunk_z = picked_chunk_id / _g_chunks_world._chunks_w;

  if ( xx < 0 ) {
    assert( chunk_x > 0 );
    xx = 15;
    chunk_x--;
  }
  if ( xx > 15 ) {
    assert( chunk_x < _g_chunks_world._chunks_w );
    xx = 0;
    chunk_x++;
  }
  if ( zz < 0 ) {
    assert( chunk_z > 0 );
    zz = 15;
    chunk_z--;
  }
  if ( zz > 15 ) {
    assert( chunk_z < _g_chunks_world._chunks_h );
    zz = 0;
    chunk_z++;
  }
  const int chunk_id_to_modify = chunk_z * _g_chunks_world._chunks_w + chunk_x;
  bool changed                 = chunks_set_block_type_in_chunk( chunk_id_to_modify, xx, yy, zz, type );
  if ( changed ) { _dirty_chunks[chunk_id_to_modify] = true; }
  return changed;
}

void chunks_update_chunk_mesh( int chunk_id ) {
  assert( chunk_id >= 0 && chunk_id < CHUNKS_N );

  // TODO(Anton) reuse a scratch buffer to avoid mallocs
  chunk_vertex_data_t vertex_data = chunk_gen_vertex_data( &_g_chunks_world._chunks[chunk_id], 0, CHUNK_Y ); // TODO(Anton) also only load the changed slices
  // TODO(Anton) and reuse the previous VBOs
  delete_mesh( &_chunk_meshes[chunk_id] );
  _chunk_meshes[chunk_id] = create_mesh_from_mem( vertex_data.positions_ptr, vertex_data.n_vp_comps, vertex_data.palette_indices_ptr,
    vertex_data.n_vpalidx_comps, vertex_data.picking_ptr, vertex_data.n_vpicking_comps, vertex_data.texcoords_ptr, vertex_data.n_vt_comps,
    vertex_data.normals_ptr, vertex_data.n_vn_comps, NULL, 0, vertex_data.n_vertices );
  chunk_free_vertex_data( &vertex_data );

  _dirty_chunks[chunk_id] = false;
}

void chunks_update_dirty_chunk_meshes() {
  for ( int i = 0; i < CHUNKS_N; i++ ) {
    if ( _dirty_chunks[i] ) { chunks_update_chunk_mesh( i ); }
  }
}

void chunks_slice_view_mode( bool enable ) { _g_chunks_world.slice_view_mode = enable; }
