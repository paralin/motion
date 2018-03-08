/*
 * mmalcam.c
 *
 *    Raspberry Pi camera module using MMAL API.
 *
 *    Built upon functionality from the Raspberry Pi userland utility raspivid.
 *
 *    Copyright 2013 by Nicholas Tuckett
 *    This software is distributed under the GNU public license version 2
 *    See also the file 'COPYING'.
 *
 */

#include <time.h>
#include "bcm_host.h"
#include "interface/vcos/vcos.h"
#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/mmal_port.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "raspicam/RaspiCamControl.h"

#include "motion.h"
#include "rotate.h"

#define MMALCAM_OK		0
#define MMALCAM_ERROR	-1

#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2
#define VIDEO_FRAME_RATE_NUM 30
#define VIDEO_FRAME_RATE_DEN 1
#define VIDEO_OUTPUT_BUFFERS_NUM 3

#define STILL_CAPTURE_MIN_DELAY_MS 5000
#define STILL_PREVIEW_WIDTH 320
#define STILL_PREVIEW_HEIGHT 240
#define STILL_FRAME_RATE_NUM 3
#define STILL_FRAME_RATE_DEN 1
#define PREVIEW_FRAME_RATE_NUM 30
#define PREVIEW_FRAME_RATE_DEN 1

const int MAX_BITRATE = 30000000; // 30Mbits/s

enum
{
    CAPTURE_MODE_VIDEO  = 1,
    CAPTURE_MODE_STILL  = 2,
};

struct timespec timespec_diff(struct timespec start, struct timespec end)
{
    struct timespec temp;
    if ((end.tv_nsec-start.tv_nsec)<0) {
        temp.tv_sec = end.tv_sec-start.tv_sec-1;
        temp.tv_nsec = 1000000000+end.tv_nsec-start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec-start.tv_sec;
        temp.tv_nsec = end.tv_nsec-start.tv_nsec;
    }
    return temp;
}

static int get_elapsed_time_ms()
{
    static int base_set = 0;
    static struct timespec base_tspec;
    struct timespec tspec;

    if (base_set == 0)
    {
        base_set = 1;
        clock_gettime(CLOCK_REALTIME, &base_tspec);
    }

    clock_gettime(CLOCK_REALTIME, &tspec);
    struct timespec diff = timespec_diff(base_tspec, tspec);

    return (diff.tv_nsec / 1000000) + (diff.tv_sec * 1000);
}

static void parse_camera_control_params(const char *control_params_str, RASPICAM_CAMERA_PARAMETERS *camera_params)
{
    char *control_params_tok = alloca(strlen(control_params_str) + 1);
    strcpy(control_params_tok, control_params_str);

    char *next_param = strtok(control_params_tok, " ");

    while (next_param != NULL) {
        char *param_val = strtok(NULL, " ");
        if (raspicamcontrol_parse_cmdline(camera_params, next_param + 1, param_val) < 2) {
            next_param = param_val;
        } else {
            next_param = strtok(NULL, " ");
        }
    }
}

static void check_disable_port(MMAL_PORT_T *port)
{
    if (port && port->is_enabled) {
        mmal_port_disable(port);
    }
}

static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    if (buffer->cmd != MMAL_EVENT_PARAMETER_CHANGED) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "Received unexpected camera control callback event, 0x%08x",
                buffer->cmd);
    }

    mmal_buffer_header_release(buffer);
}

static void camera_buffer_video_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    mmalcam_context_ptr mmalcam = (mmalcam_context_ptr) port->userdata;
    mmal_queue_put(mmalcam->camera_buffer_queue, buffer);
}

static int last_still_capture_time_ms = 0;

static void camera_buffer_still_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    mmalcam_context_ptr mmalcam = (mmalcam_context_ptr) port->userdata;
    mmal_queue_put(mmalcam->camera_buffer_queue, buffer);

    int curr_time = get_elapsed_time_ms();
    int capture_time_delta = curr_time - last_still_capture_time_ms;
    if (capture_time_delta < STILL_CAPTURE_MIN_DELAY_MS)
    {
        vcos_sleep(STILL_CAPTURE_MIN_DELAY_MS - capture_time_delta);
    }

    if (mmal_port_parameter_set_boolean(mmalcam->camera_capture_port, MMAL_PARAMETER_CAPTURE, 1)
            != MMAL_SUCCESS) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "MMAL camera capture start failed");
    }

    last_still_capture_time_ms = curr_time;
}

static void set_port_format(int width, int height, MMAL_ES_FORMAT_T *format)
{
    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;
    format->es->video.width = width;
    format->es->video.height = height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = width;
    format->es->video.crop.height = height;
}

static void set_video_port_format(mmalcam_context_ptr mmalcam, MMAL_ES_FORMAT_T *format)
{
    set_port_format(mmalcam->width, mmalcam->height, format);
    format->es->video.frame_rate.num = mmalcam->framerate;
    format->es->video.frame_rate.den = VIDEO_FRAME_RATE_DEN;
}

static int create_camera_component(mmalcam_context_ptr mmalcam, const char *mmalcam_name, int capture_mode)
{
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T *camera_component;
    MMAL_PORT_T *capture_port = NULL;

    status = mmal_component_create(mmalcam_name, &camera_component);

    if (status != MMAL_SUCCESS) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "Failed to create MMAL camera component %s", mmalcam_name);
        goto error;
    }

    if (camera_component->output_num == 0) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "MMAL camera %s doesn't have output ports", mmalcam_name);
        goto error;
    }

    status = mmal_port_enable(camera_component->control, camera_control_callback);

    if (status) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "Unable to enable control port : error %d", status);
        goto error;
    }

    //  set up the camera configuration
    {
        MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
                { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config) },
                .max_stills_w = mmalcam->width,
                .max_stills_h = mmalcam->height,
                .stills_yuv422 = 0,
                .one_shot_stills = 0,
                .max_preview_video_w = mmalcam->width,
                .max_preview_video_h = mmalcam->height,
                .num_preview_video_frames = 3,
                .stills_capture_circular_buffer_height = 0,
                .fast_preview_resume = 0,
                .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC };
        mmal_port_parameter_set(camera_component->control, &cam_config.hdr);
    }

    switch(capture_mode)
    {
        case CAPTURE_MODE_VIDEO:
        {
            capture_port = camera_component->output[MMAL_CAMERA_VIDEO_PORT];
            mmalcam->camera_buffer_callback = camera_buffer_video_callback;
            set_video_port_format(mmalcam, capture_port->format);
            capture_port->format->encoding = MMAL_ENCODING_I420;
            break;
        }

        case CAPTURE_MODE_STILL:
        {
            capture_port = camera_component->output[MMAL_CAMERA_CAPTURE_PORT];
            mmalcam->camera_buffer_callback = camera_buffer_video_callback;
            set_port_format(mmalcam->width, mmalcam->height, capture_port->format);
            capture_port->format->encoding = MMAL_ENCODING_I420;
            capture_port->format->es->video.frame_rate.num = STILL_FRAME_RATE_NUM;
            capture_port->format->es->video.frame_rate.num = STILL_FRAME_RATE_DEN;

            MMAL_PORT_T *preview_port = camera_component->output[MMAL_CAMERA_PREVIEW_PORT];
            set_port_format(STILL_PREVIEW_WIDTH, STILL_PREVIEW_HEIGHT, preview_port->format);
            preview_port->format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_NUM;
            preview_port->format->es->video.frame_rate.num = PREVIEW_FRAME_RATE_DEN;
            if (mmal_port_format_commit(preview_port)) {
                MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "still camera setup couldn't configure preview");
                goto error;
            }
            break;
        }
    }

    status = mmal_port_format_commit(capture_port);

    if (status) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "camera video format couldn't be set");
        goto error;
    }

    // Ensure there are enough buffers to avoid dropping frames
    if (capture_port->buffer_num < VIDEO_OUTPUT_BUFFERS_NUM) {
        capture_port->buffer_num = VIDEO_OUTPUT_BUFFERS_NUM;
    }

    status = mmal_component_enable(camera_component);

    if (status) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "camera component couldn't be enabled");
        goto error;
    }

    raspicamcontrol_set_all_parameters(camera_component, mmalcam->camera_parameters);
    mmalcam->camera_component = camera_component;
    mmalcam->camera_capture_port = capture_port;
    mmalcam->camera_capture_port->userdata = (struct MMAL_PORT_USERDATA_T*) mmalcam;
    MOTION_LOG(NTC, TYPE_VIDEO, NO_ERRNO, "MMAL camera component created");
    return MMALCAM_OK;

    error: if (mmalcam->camera_component != NULL ) {
        mmal_component_destroy(camera_component);
        mmalcam->camera_component = NULL;
    }

    return MMALCAM_ERROR;
}

static void destroy_camera_component(mmalcam_context_ptr mmalcam)
{
    if (mmalcam->camera_component) {
        mmal_component_destroy(mmalcam->camera_component);
        mmalcam->camera_component = NULL;
    }
}

static int create_camera_buffer_structures(mmalcam_context_ptr mmalcam)
{
    mmalcam->camera_buffer_pool = mmal_pool_create(mmalcam->camera_capture_port->buffer_num,
            mmalcam->camera_capture_port->buffer_size);
    if (mmalcam->camera_buffer_pool == NULL ) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "MMAL camera buffer pool creation failed");
        return MMALCAM_ERROR;
    }

    mmalcam->camera_buffer_queue = mmal_queue_create();
    if (mmalcam->camera_buffer_queue == NULL ) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "MMAL camera buffer queue creation failed");
        return MMALCAM_ERROR;
    }

    return MMALCAM_OK;
}

static int send_pooled_buffers_to_port(MMAL_POOL_T *pool, MMAL_PORT_T *port)
{
    int num = mmal_queue_length(pool->queue);

    for (int i = 0; i < num; i++) {
        MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(pool->queue);

        if (!buffer) {
            MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "Unable to get a required buffer %d from pool queue", i);
            return MMALCAM_ERROR;
        }

        if (mmal_port_send_buffer(port, buffer) != MMAL_SUCCESS) {
            MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "Unable to send a buffer to port (%d)", i);
            return MMALCAM_ERROR;
        }
    }

    return MMALCAM_OK;
}

static void destroy_camera_buffer_structures(mmalcam_context_ptr mmalcam)
{
    if (mmalcam->camera_buffer_queue != NULL ) {
        mmal_queue_destroy(mmalcam->camera_buffer_queue);
        mmalcam->camera_buffer_queue = NULL;
    }

    if (mmalcam->camera_buffer_pool != NULL ) {
        mmal_pool_destroy(mmalcam->camera_buffer_pool);
        mmalcam->camera_buffer_pool = NULL;
    }
}

/**
 * mmalcam_start
 *
 *      This routine is called from the main motion thread.  It's job is
 *      to open up the requested camera device via MMAL and do any required
 *      initialisation.
 *
 * Parameters:
 *
 *      cnt     Pointer to the motion context structure for this device.
 *
 * Returns:     0 on success
 *              -1 on any failure
 */

int mmalcam_start(struct context *cnt)
{
    mmalcam_context_ptr mmalcam;

    cnt->mmalcam = (mmalcam_context*) mymalloc(sizeof(struct mmalcam_context));
    memset(cnt->mmalcam, 0, sizeof(mmalcam_context));
    mmalcam = cnt->mmalcam;
    mmalcam->cnt = cnt;

    MOTION_LOG(ALR, TYPE_VIDEO, NO_ERRNO,
            "%s: MMAL Camera thread starting... for camera (%s) of %d x %d at %d fps",
            cnt->conf.mmalcam_name, cnt->conf.width, cnt->conf.height, cnt->conf.frame_limit);

    mmalcam->camera_parameters = (RASPICAM_CAMERA_PARAMETERS*)malloc(sizeof(RASPICAM_CAMERA_PARAMETERS));
    if (mmalcam->camera_parameters == NULL) {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "camera params couldn't be allocated");
        return MMALCAM_ERROR;
    }

    raspicamcontrol_set_defaults(mmalcam->camera_parameters);
    mmalcam->width = cnt->conf.width;
    mmalcam->height = cnt->conf.height;
    mmalcam->framerate = cnt->conf.frame_limit;

    if (cnt->conf.mmalcam_control_params) {
        parse_camera_control_params(cnt->conf.mmalcam_control_params, mmalcam->camera_parameters);
    }

    int capture_mode;

    if (cnt->conf.mmalcam_use_still) {
        MOTION_LOG(ALR, TYPE_VIDEO, NO_ERRNO, "%s: MMAL Camera using still capture");
        capture_mode = CAPTURE_MODE_STILL;
    }
    else {
        MOTION_LOG(ALR, TYPE_VIDEO, NO_ERRNO, "%s: MMAL Camera using video capture");
        capture_mode = CAPTURE_MODE_VIDEO;
    }

    cnt->imgs.width = mmalcam->width;
    cnt->imgs.height = mmalcam->height;
    cnt->imgs.size = (mmalcam->width * mmalcam->height * 3) / 2;
    cnt->imgs.motionsize = mmalcam->width * mmalcam->height;
    cnt->imgs.type = VIDEO_PALETTE_YUV420P;

    int retval = create_camera_component(mmalcam, cnt->conf.mmalcam_name, capture_mode);

    if (retval == 0) {
        retval = create_camera_buffer_structures(mmalcam);
    }

    if (retval == 0) {
        if (mmal_port_enable(mmalcam->camera_capture_port, mmalcam->camera_buffer_callback)) {
            MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "MMAL camera capture port enabling failed");
            retval = MMALCAM_ERROR;
        }
    }

    if (retval == 0) {
        if (mmal_port_parameter_set_boolean(mmalcam->camera_capture_port, MMAL_PARAMETER_CAPTURE, 1)
                != MMAL_SUCCESS) {
            MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "MMAL camera capture start failed");
            retval = MMALCAM_ERROR;
        }
        last_still_capture_time_ms = get_elapsed_time_ms();
    }

    if (retval == 0) {
        retval = send_pooled_buffers_to_port(mmalcam->camera_buffer_pool, mmalcam->camera_capture_port);
    }

    return retval;
}

/**
 * mmalcam_cleanup
 *
 *      This routine shuts down any MMAL resources, then releases any allocated data
 *      within the mmalcam context and frees the context itself.
 *      This function is also called from motion_init if first time connection
 *      fails and we start retrying until we get a valid first frame from the
 *      camera.
 *
 * Parameters:
 *
 *      mmalcam          Pointer to a mmalcam context
 *
 * Returns:              Nothing.
 *
 */
void mmalcam_cleanup(struct mmalcam_context *mmalcam)
{
    MOTION_LOG(ALR, TYPE_VIDEO, NO_ERRNO, "MMAL Camera cleanup");

    if (mmalcam != NULL ) {
        if (mmalcam->camera_component) {
            check_disable_port(mmalcam->camera_capture_port);
            mmal_component_disable(mmalcam->camera_component);
            destroy_camera_buffer_structures(mmalcam);
            destroy_camera_component(mmalcam);
        }

        if (mmalcam->camera_parameters) {
            free(mmalcam->camera_parameters);
        }

        free(mmalcam);
    }
}

/**
 * mmalcam_next
 *
 *      This routine is called when the main 'motion' thread wants a new
 *      frame of video.  It fetches the most recent frame available from
 *      the Pi camera already in YUV420P, and returns it to motion.
 *
 * Parameters:
 *      cnt             Pointer to the context for this thread
 *      image           Pointer to a buffer for the returned image
 *
 * Returns:             Error code
 */
int mmalcam_next(struct context *cnt, unsigned char *map)
{
    mmalcam_context_ptr mmalcam;

    if ((!cnt) || (!cnt->mmalcam))
        return NETCAM_FATAL_ERROR;

    mmalcam = cnt->mmalcam;

    MMAL_BUFFER_HEADER_T *camera_buffer = mmal_queue_wait(mmalcam->camera_buffer_queue);

    if (camera_buffer->cmd == 0 && (camera_buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
            && camera_buffer->length == cnt->imgs.size) {
        mmal_buffer_header_mem_lock(camera_buffer);
        memcpy(map, camera_buffer->data, cnt->imgs.size);
        mmal_buffer_header_mem_unlock(camera_buffer);
    } else {
        MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "%s: cmd %d flags %08x size %d/%d at %08x",
                camera_buffer->cmd, camera_buffer->flags, camera_buffer->length, camera_buffer->alloc_size, camera_buffer->data);
    }

    mmal_buffer_header_release(camera_buffer);

    if (mmalcam->camera_capture_port->is_enabled) {
        MMAL_STATUS_T status;
        MMAL_BUFFER_HEADER_T *new_buffer = mmal_queue_get(mmalcam->camera_buffer_pool->queue);

        if (new_buffer) {
            status = mmal_port_send_buffer(mmalcam->camera_capture_port, new_buffer);
        }

        if (!new_buffer || status != MMAL_SUCCESS)
            MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "Unable to return a buffer to the camera capture port");

        if (mmalcam->cnt->conf.mmalcam_use_still) {
            int curr_time = get_elapsed_time_ms();
            int capture_time_delta = curr_time - last_still_capture_time_ms;
            if (capture_time_delta < STILL_CAPTURE_MIN_DELAY_MS)
            {
                vcos_sleep(STILL_CAPTURE_MIN_DELAY_MS - capture_time_delta);
            }

            if (mmal_port_parameter_set_boolean(mmalcam->camera_capture_port, MMAL_PARAMETER_CAPTURE, 1)
                    != MMAL_SUCCESS) {
                MOTION_LOG(ERR, TYPE_VIDEO, NO_ERRNO, "MMAL camera capture start failed");
            }

            last_still_capture_time_ms = curr_time;
        }
    }

    if (cnt->rotate_data.degrees > 0)
        rotate_map(cnt, map);

    return 0;
}
