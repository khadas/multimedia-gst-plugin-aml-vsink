/* GStreamer
 * Copyright (C) 2020 Amlogic, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free SoftwareFoundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */
#ifndef AML_DRIVER_H__
#define AML_DRIVER_H__

#include <stdint.h>

#define V4L2_CONFIG_PARM_DECODE_CFGINFO (1 << 0)
#define V4L2_CONFIG_PARM_DECODE_PSINFO  (1 << 1)
#define V4L2_CONFIG_PARM_DECODE_HDRINFO (1 << 2)
#define V4L2_CONFIG_PARM_DECODE_CNTINFO (1 << 3)

struct aml_vdec_cfg_infos {
    uint32_t double_write_mode;
    uint32_t init_width;
    uint32_t init_height;
    uint32_t ref_buf_margin;
    uint32_t canvas_mem_mode;
    uint32_t canvas_mem_endian;
    uint32_t low_latency_mode;
    uint32_t uvm_hook_type;
    /*
     * bit 16 : force progressive output flag.
     * bit 15 : enable nr.
     * bit 14 : enable di local buff.
     * bit 13 : report downscale yuv buffer size flag.
     * bit 12 : for second field pts mode.
     * bit 1  : Non-standard dv flag.
     * bit 0  : dv two layer flag.
     */
    uint32_t metadata_config_flag; // for metadata config flag
    uint32_t data[5];
};

#define SEI_PicTiming         1
#define SEI_MasteringDisplayColorVolume 137
#define SEI_ContentLightLevel 144
struct vframe_content_light_level_s {
    uint32_t present_flag;
    uint32_t max_content;
    uint32_t max_pic_average;
}; /* content_light_level from SEI */

struct vframe_master_display_colour_s {
    uint32_t present_flag;
    uint32_t primaries[3][2];
    uint32_t white_point[2];
    uint32_t luminance[2];
    struct vframe_content_light_level_s
        content_light_level;
}; /* master_display_colour_info_volume from SEI */

struct aml_vdec_hdr_infos {
    /*
     * bit 29   : present_flag
     * bit 28-26: video_format "component", "PAL", "NTSC", "SECAM", "MAC", "unspecified"
     * bit 25   : range "limited", "full_range"
     * bit 24   : color_description_present_flag
     * bit 23-16: color_primaries "unknown", "bt709", "undef", "bt601",
     *            "bt470m", "bt470bg", "smpte170m", "smpte240m", "film", "bt2020"
     * bit 15-8 : transfer_characteristic unknown", "bt709", "undef", "bt601",
     *            "bt470m", "bt470bg", "smpte170m", "smpte240m",
     *            "linear", "log100", "log316", "iec61966-2-4",
     *            "bt1361e", "iec61966-2-1", "bt2020-10", "bt2020-12",
     *            "smpte-st-2084", "smpte-st-428"
     * bit 7-0  : matrix_coefficient "GBR", "bt709", "undef", "bt601",
     *            "fcc", "bt470bg", "smpte170m", "smpte240m",
     *            "YCgCo", "bt2020nc", "bt2020c"
     */
    uint32_t signal_type;
    struct vframe_master_display_colour_s color_parms;
};

struct aml_vdec_ps_infos {
    uint32_t visible_width;
    uint32_t visible_height;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t profile;
    uint32_t mb_width;
    uint32_t mb_height;
    uint32_t dpb_size;
    uint32_t ref_frames;
    uint32_t dpb_frames;
    uint32_t dpb_margin;
    uint32_t field;
    uint32_t data[3];
};

struct aml_vdec_cnt_infos {
    uint32_t bit_rate;
    uint32_t frame_count;
    uint32_t error_frame_count;
    uint32_t drop_frame_count;
    uint32_t total_data;
};

struct aml_dec_params {
    /* one of V4L2_CONFIG_PARM_DECODE_xxx */
    uint32_t parms_status;
    struct aml_vdec_cfg_infos   cfg;
    struct aml_vdec_ps_infos    ps;
    struct aml_vdec_hdr_infos   hdr;
    struct aml_vdec_cnt_infos   cnt;
};

#define V4L2_CID_USER_AMLOGIC_BASE (V4L2_CID_USER_BASE + 0x1100)
#define AML_V4L2_SET_DRMMODE (V4L2_CID_USER_AMLOGIC_BASE + 0)
#define AML_V4L2_DEC_PARMS_CONFIG (V4L2_CID_USER_AMLOGIC_BASE + 7)

#endif
