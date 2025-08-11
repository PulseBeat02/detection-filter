/*****************************************************************************
* facedetection.cpp : Face detection video filter
 *****************************************************************************
 * Copyright (C) 2025 VideoLAN
 *
 * Authors: Brandon Li <brandonli2006ma@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <cctype>
#include <climits>
#include <cmath>
#include <memory>

#include <vlc_common.h>
#include <vlc_configuration.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

#include "facedetectcnn.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void       Close  ( filter_t *p_filter );
static int        Open   ( filter_t *p_filter );
static picture_t* Filter ( filter_t *p_filter, picture_t *p_pic );

static void draw_rectangle( uint8_t *p_bgr,
    unsigned int i_x, unsigned int i_y, unsigned int i_w, unsigned int i_h,
    unsigned int i_width, unsigned int i_height, unsigned int i_stride,
    unsigned int i_downscale_width, unsigned int i_downscale_height,
    const unsigned int *p_border_color, unsigned int p_thickness );

static constexpr vlc_filter_operations filter_ops =
{
    .filter_video = Filter,
    .drain_audio = nullptr,
    .flush = nullptr,
    .change_viewpoint = nullptr,
    .video_mouse = nullptr,
    .close = Close
};

#define MAX_FACES 0x9000
#define MAX_FACES_TRACKED 128

typedef struct
{
    unsigned int s_x, s_y, s_w, s_h;
    unsigned int i_confidence;
} face_detection_t;

typedef struct
{
    // config options
    unsigned int p_border_color[3]      = {};
    unsigned int i_downscale_width      = 320;
    unsigned int i_downscale_height     = 240;
    unsigned int i_downscale_pixels     = 0;
    unsigned int i_downscale_stride     = 0;
    unsigned int i_border_thickness     = 5;
    unsigned int i_detection_frequency  = 4;
    unsigned int i_confidence_threshold = 50;

    // face detection
    face_detection_t p_previous_faces[MAX_FACES_TRACKED] = {};
    unsigned int i_frame_count = 0;
    unsigned int i_face_count  = 0;
    bool b_has_faces           = false;

    // reused buffers
    unsigned char p_buffer[MAX_FACES] = {};
    uint8_t* p_scaled                 = {};
} filter_sys_t;

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin()
    set_description( N_( "Face detection filter" ) )
    set_shortname( N_( "facedetection" ) )
    set_subcategory( SUBCAT_VIDEO_VFILTER )
    add_shortcut( "facedetection" )
    add_string( "border-color", "#00FF00",
        N_( "border-color" ), N_( "Hexadecimal color of borders highlighting detected faces. Must be a valid RGB hexadecimal color." ) )
    add_integer( "border-thickness", 5,
        N_( "border-thickness" ), N_( "Thickness of boarders highlighting detected faces. Must be greater than 0." ) )
    add_integer( "confidence-threshold", 50,
        N_( "confidence-threshold" ), N_( "Minimum confidence to draw a box. Must be between 0 and 100." ) )
    add_integer( "detection-frequency", 4,
        N_( "detection-frequency" ), N_( "Frequency of face detection operations (every N frames). Must be greater than 0." ) )
    add_integer( "downscale-width", 320,
        N_( "downscale-width" ), N_( "Width of the downscaled image for face detection. Must be greater than 0." ) )
    add_integer( "downscale-height", 240,
        N_( "downscale-height" ), N_( "Height of the downscaled image for face detection. Must be greater than 0." ) )
    set_callback_video_filter( Open )
vlc_module_end()

/*****************************************************************************
 * Open: Create the dither filter, creating cache table
 *****************************************************************************/
static int Open( filter_t *p_filter )
{

    // libfacedetection requires BGR24
    if ( p_filter->fmt_in.video.i_chroma != VLC_CODEC_BGR24 )
    {
        msg_Err( p_filter, "Chroma unsupported" );
        return VLC_EGENERIC;
    }

    // argument sanitization

    // border thickness
    const int i_border_thickness = var_InheritInteger( p_filter, "border-thickness" );
    if ( i_border_thickness < 1 )
    {
        msg_Err( p_filter, "Invalid border thickness, must be greater than 0" );
        return VLC_EGENERIC;
    }


    // confidence threshold
    const int i_confidence_threshold = var_InheritInteger( p_filter, "confidence-threshold" );
    if ( i_confidence_threshold < 0 || i_confidence_threshold > 100 )
    {
        msg_Err( p_filter, "Invalid confidence threshold, must be between 0 and 100" );
        return VLC_EGENERIC;
    }


    // detection frequency
    const int i_detection_frequency = var_InheritInteger( p_filter, "detection-frequency" );
    if ( i_detection_frequency < 1 )
    {
        msg_Err( p_filter, "Invalid detection frequency, must be greater than 0" );
        return VLC_EGENERIC;
    }


    // downscale width
    const int i_downscale_width = var_InheritInteger( p_filter, "downscale-width" );
    if ( i_downscale_width < 1 )
    {
        msg_Err( p_filter, "Invalid downscale width, must be greater than or equal to 1" );
        return VLC_EGENERIC;
    }


    // downscale height
    const int i_downscale_height = var_InheritInteger( p_filter, "downscale-height" );
    if ( i_downscale_height < 1 )
    {
        msg_Err( p_filter, "Invalid downscale height, must be greater than or equal to 1" );
        return VLC_EGENERIC;
    }


    // border color
    char* p_border_color = var_InheritString( p_filter, "border-color" );
    if ( !p_border_color )
    {
        msg_Err( p_filter, "No border color specified" );
        return VLC_EGENERIC;
    }

    // remove leading #
    const char* p_curr_char = p_border_color;
    if ( p_curr_char[0] == '#' ) {
        p_curr_char++;
    }

    if ( strlen( p_curr_char ) != 6 ) {
        msg_Err( p_filter, "Invalid hexadecimal color, must be 6 characters" );
        free( p_border_color );
        return VLC_EGENERIC;
    }

    unsigned int p_rgb[3];
    char p_hex[3] = {};

    // red
    p_hex[0] = p_curr_char[0];
    p_hex[1] = p_curr_char[1];
    p_rgb[0] = strtoul( p_hex, nullptr, 16 );

    // green
    p_hex[0] = p_curr_char[2];
    p_hex[1] = p_curr_char[3];
    p_rgb[1] = strtoul( p_hex, nullptr, 16 );

    // blue
    p_hex[0] = p_curr_char[4];
    p_hex[1] = p_curr_char[5];
    p_rgb[2] = strtoul( p_hex, nullptr, 16 );

    free( p_border_color );

    filter_sys_t* p_sys;
    try
    {
        p_sys = new filter_sys_t;
    } catch (std::bad_alloc&)
    {
        return VLC_ENOMEM;
    }

    const unsigned int i_pixels   = i_downscale_width * i_downscale_height;
    const unsigned int i_buffer   = i_pixels * 3;
    p_sys->i_border_thickness     = i_border_thickness;
    p_sys->i_confidence_threshold = i_confidence_threshold;
    p_sys->i_detection_frequency  = i_detection_frequency;
    p_sys->i_downscale_width      = i_downscale_width;
    p_sys->i_downscale_height     = i_downscale_height;
    p_sys->i_downscale_pixels     = i_pixels;
    p_sys->i_downscale_stride     = i_downscale_width * 3;
    p_sys->p_border_color[0]      = p_rgb[0];
    p_sys->p_border_color[1]      = p_rgb[1];
    p_sys->p_border_color[2]      = p_rgb[2];

    try
    {
        p_sys->p_scaled = new uint8_t[ i_buffer ];
    } catch (std::bad_alloc&)
    {
        delete p_sys;
        return VLC_ENOMEM;
    }

    p_filter->p_sys = p_sys;
    p_filter->ops = &filter_ops;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: Releases all memory
 *****************************************************************************/
static void Close( filter_t *p_filter )
{
    if ( const auto *p_sys = static_cast<filter_sys_t*> ( p_filter->p_sys ) ) {
        delete[] p_sys->p_scaled;
        delete p_sys;
    }
}

/*****************************************************************************
 * Filter: Runs the face detection algorithm per N frames, downscaling the image
 * first and then drawing rectangles around detected faces.
 *****************************************************************************/
static picture_t* Filter( filter_t *p_filter, picture_t *p_pic )
{
    auto *p_sys = static_cast<filter_sys_t*> ( p_filter->p_sys );

    // detection frequency
    const bool b_run_detection = p_sys->i_frame_count % p_sys->i_detection_frequency == 0;
    p_sys->i_frame_count++;

    uint8_t *scaled_buffer = p_sys->p_scaled;
    uint8_t *p_bgr = p_pic->p[0].p_pixels;

    const int i_width    = p_pic->p[0].i_visible_pitch / 3;
    const int i_height   = p_pic->p[0].i_visible_lines;
    const int i_stride   = p_pic->p[0].i_pitch;

    const int i_downscale_width  = p_sys->i_downscale_width;
    const int i_downscale_height = p_sys->i_downscale_height;
    const int i_downscale_stride = p_sys->i_downscale_stride;

    int *p_results = nullptr;
    if ( b_run_detection ) {

        // Downscale the image and convert to grayscale for faster processing
        for ( int i_y = 0; i_y < i_downscale_height; i_y++ ) {
            const float f_fy = i_y * ( static_cast<float> ( i_height ) / i_downscale_height );
            const int i_ys  = VLC_CLIP( int( f_fy ), 0, i_height - 1 );
            uint8_t* p_dst_row = scaled_buffer + i_y * i_downscale_stride;
            for ( int i_x = 0; i_x < i_downscale_width; i_x++ ) {
                const float f_fx = i_x * ( static_cast<float> ( i_width ) / i_downscale_width );
                const int i_xs = VLC_CLIP( int( f_fx ), 0, i_width  - 1 );
                const uint8_t* p_src = p_bgr + i_ys * i_stride + i_xs * 3;
                uint8_t* p_dst = p_dst_row + i_x * 3;
                const uint8_t gray = static_cast<uint8_t> (
                    0.114f * p_src[0] +
                    0.587f * p_src[1] +
                    0.299f * p_src[2]
                );
                p_dst[0] = gray; // B
                p_dst[1] = gray; // G
                p_dst[2] = gray;
            }
        }

        // (╯°□°)╯︵ ┻━┻ magic
        memset( p_sys->p_buffer, 0, sizeof ( p_sys->p_buffer ) );
        p_results = facedetect_cnn(
            p_sys->p_buffer,
            p_sys->p_scaled,
            i_downscale_width,
            i_downscale_height,
            i_downscale_stride
        );

        // store faces (if found)
        if ( p_results && p_results[0] > 0 ) {
            p_sys->b_has_faces = true;
            p_sys->i_face_count = p_results[0];
            for ( unsigned int i_i = 0; i_i < p_sys->i_face_count && i_i < MAX_FACES_TRACKED; i_i++ ) {
                const short *p = reinterpret_cast<short*> ( p_results + 1 ) + 16 * i_i;
                p_sys->p_previous_faces[ i_i ].i_confidence = p[0];
                p_sys->p_previous_faces[ i_i ].s_x          = p[1];
                p_sys->p_previous_faces[ i_i ].s_y          = p[2];
                p_sys->p_previous_faces[ i_i ].s_w          = p[3];
                p_sys->p_previous_faces[ i_i ].s_h          = p[4];
            }
        }
    }

    const unsigned int i_confidence_threshold = p_sys->i_confidence_threshold;
    const unsigned int* i_border_color = p_sys->p_border_color;
    const unsigned int i_border_thickness = p_sys->i_border_thickness;
    if ( !b_run_detection && p_sys->b_has_faces ) { // draw previous faces
        for ( unsigned int i_i = 0; i_i < p_sys->i_face_count; i_i++ ) {
            if ( const unsigned int i_confidence = p_sys->p_previous_faces[ i_i ].i_confidence;
                i_confidence < i_confidence_threshold )
            {
                continue; // skip ones below threshold
            }
            const unsigned int i_x = p_sys->p_previous_faces[ i_i ].s_x;
            const unsigned int i_y = p_sys->p_previous_faces[ i_i ].s_y;
            const unsigned int i_w = p_sys->p_previous_faces[ i_i ].s_w;
            const unsigned int i_h = p_sys->p_previous_faces[ i_i ].s_h;
            draw_rectangle( p_bgr,
                i_x, i_y, i_w, i_h,
                i_width, i_height, i_stride,
                i_downscale_width, i_downscale_height,
                i_border_color, i_border_thickness );
        }
    } else if ( p_results && p_results[0] > 0 ) { // draw current faces
        const int i_face_count = p_results[0];
        for ( int i_i = 0; i_i < i_face_count; i_i++ ) {
            const short *p_face = reinterpret_cast<short*> ( p_results + 1 ) + 16 * i_i;
            const unsigned int i_confidence = p_face[0];
            const int i_x = p_face[1];
            const int i_y = p_face[2];
            const int i_w = p_face[3];
            const int i_h = p_face[4];
            if ( i_confidence < i_confidence_threshold )
            {
                continue;
            }
            draw_rectangle( p_bgr,
                i_x, i_y, i_w, i_h,
                i_width, i_height, i_stride,
                i_downscale_width, i_downscale_height,
                i_border_color, i_border_thickness );
        }
    }

    return p_pic;
}

static inline void draw_rectangle( uint8_t* __restrict p_bgr,
    const unsigned int i_x, const unsigned int i_y, const unsigned int i_w, const unsigned int i_h,
    const unsigned int i_width, const unsigned int i_height, const unsigned int i_stride,
    const unsigned int i_downscale_width, const unsigned int i_downscale_height,
    const unsigned int *p_border_color, const unsigned int p_thickness )
{

    // scale back to original resolution
    const float f_scale_x = static_cast<float> ( i_width ) / i_downscale_width;
    const float f_scale_y = static_cast<float> ( i_height ) / i_downscale_height;
    const unsigned int i_scaled_x = static_cast<int> ( static_cast<float> ( i_x ) * f_scale_x );
    const unsigned int i_scaled_y = static_cast<int> ( static_cast<float> ( i_y ) * f_scale_y );
    const unsigned int i_scaled_w = static_cast<int> ( static_cast<float> ( i_w ) * f_scale_x );
    const unsigned int i_scaled_h = static_cast<int> ( static_cast<float> ( i_h ) * f_scale_y );

    // clamp
    const unsigned int i_final_x = VLC_CLIP( i_scaled_x, 0, i_width  - 1        );
    const unsigned int i_final_y = VLC_CLIP( i_scaled_y, 0, i_height - 1        );
    const unsigned int i_final_w = VLC_CLIP( i_scaled_w, 0, i_width  - i_final_x);
    const unsigned int i_final_h = VLC_CLIP( i_scaled_h, 0, i_height - i_final_y);

    // draw borders
    const uint8_t i_r = p_border_color[0];
    const uint8_t i_g = p_border_color[1];
    const uint8_t i_b = p_border_color[2];
    for ( unsigned int i_line = 0; i_line < p_thickness; i_line++ ) {
        const int i_y_top = VLC_CLIP( i_final_y + i_line, 0, i_height - 1 );
        const int i_y_bottom = VLC_CLIP( i_final_y + i_final_h - 1 - i_line, 0, i_height - 1 );
        for ( unsigned int i_i = i_final_x; i_i < i_final_x + i_final_w; i_i++ ) {
            p_bgr[ ( i_y_top    * i_stride ) + ( i_i * 3 ) + 0 ] = i_b;
            p_bgr[ ( i_y_top    * i_stride ) + ( i_i * 3 ) + 1 ] = i_g;
            p_bgr[ ( i_y_top    * i_stride ) + ( i_i * 3 ) + 2 ] = i_r;
            p_bgr[ ( i_y_bottom * i_stride ) + ( i_i * 3 ) + 0 ] = i_b;
            p_bgr[ ( i_y_bottom * i_stride ) + ( i_i * 3 ) + 1 ] = i_g;
            p_bgr[ ( i_y_bottom * i_stride ) + ( i_i * 3 ) + 2 ] = i_r;
        }
    }

    for ( unsigned int i_line = 0; i_line < p_thickness; i_line++ ) {
        const int i_x_left = VLC_CLIP( i_final_x + i_line, 0, i_width - 1 );
        const int i_x_right = VLC_CLIP( i_final_x + i_final_w - 1 - i_line, 0, i_width - 1 );
        for ( unsigned int j_j = i_final_y; j_j < i_final_y + i_final_h; j_j++ ) {
            p_bgr[ ( j_j * i_stride ) + ( i_x_left  * 3 ) + 0 ] = i_b;
            p_bgr[ ( j_j * i_stride ) + ( i_x_left  * 3 ) + 1 ] = i_g;
            p_bgr[ ( j_j * i_stride ) + ( i_x_left  * 3 ) + 2 ] = i_r;
            p_bgr[ ( j_j * i_stride ) + ( i_x_right * 3 ) + 0 ] = i_b;
            p_bgr[ ( j_j * i_stride ) + ( i_x_right * 3 ) + 1 ] = i_g;
            p_bgr[ ( j_j * i_stride ) + ( i_x_right * 3 ) + 2 ] = i_r;
        }
    }
}