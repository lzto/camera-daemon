/*
 * raspberry pi camera daemon
 * 2019 Tong Zhang<ztong0001@gmail.com>
 */
#include <stdio.h>
#include <stdlib.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>

#include <pthread.h>

#include <http_parser.h>
#include <cJSON.h>

#include "rtpworker.h"

#include <bcm_host.h>
#include <interface/vcos/vcos.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/util/mmal_util_params.h>

#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_STILL_PORT 2


#define SNAPSHOT_WIDTH 1920
#define SNAPSHOT_HEIGHT 1080

#if 1
#define VIDEO_FPS 30
#define VIDEO_WIDTH 1920
#define VIDEO_HEIGHT 1080
#else
#define VIDEO_FPS 10 
#define VIDEO_WIDTH 640
#define VIDEO_HEIGHT 360
#endif

#define IMAGE_BUFFER_SIZE 3*VIDEO_WIDTH*VIDEO_HEIGHT

#define PORT 7777

typedef struct {
    int width;
    int height;
    MMAL_COMPONENT_T *camera;
    MMAL_COMPONENT_T *splitter_component;
    MMAL_COMPONENT_T *preview;
    MMAL_COMPONENT_T *video_encoder;
    MMAL_COMPONENT_T *jpeg_component;
    MMAL_COMPONENT_T *resize_component;

    MMAL_CONNECTION_T *splitter_connection;
    MMAL_CONNECTION_T *resize_connection;
    MMAL_CONNECTION_T *video_encoder_connection;
    MMAL_CONNECTION_T *jpeg_connection;

    MMAL_POOL_T* video_encoder_output_pool;
    MMAL_POOL_T* jpeg_encoder_output_pool;

    //mmal_output camera_output;
    //mmal_output secondary_output;

    //producer consumer
    pthread_mutex_t img_lock;
    pthread_cond_t img_cond;
    size_t snapshot_request_n;//number of pending request for image
    uint8_t* stream_header;
    size_t stream_header_size;
    uint8_t *image_buffer;
    size_t image_max_size;
    size_t image_size;
    uint8_t have_active_client;
    uint8_t have_active_srtp_receiver;
    int client_fd;

    //used by http parser
    char* last_url;
    size_t last_url_size;
    float fps;
} PORT_USERDATA;

PORT_USERDATA userdata;

static void jpeg_encoder_output_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T* buffer) {
    PORT_USERDATA *userdata = (PORT_USERDATA *) port->userdata;
    MMAL_POOL_T *pool = userdata->jpeg_encoder_output_pool;

    static int already_acquired_lock = 0;
    static int state = 0;
    //1 - start
    //0 - not start

    //TODO: send snapshot
    if (!userdata->snapshot_request_n)
        goto end;
    if (!already_acquired_lock)
        if (pthread_mutex_trylock(&userdata->img_lock))
            already_acquired_lock = 1;
    if (already_acquired_lock)
    {
        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
        {
            //this is the end of previous frame,
            //we start from next frame
            if (state == 0)
            {
                userdata->image_size = 0;
                state = 1;
                goto end;
            }
        }
        if (state)
        {
            uint8_t* local_image_buffer = userdata->image_buffer;
            memcpy(&local_image_buffer[userdata->image_size],
                    buffer->data, buffer->length);
            userdata->image_size += buffer->length;
        }
        if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END)
        {
            if (state==1)
            {
                userdata->snapshot_request_n = 0;
                pthread_cond_signal(&userdata->img_cond);
                pthread_mutex_unlock(&userdata->img_lock);
                already_acquired_lock = 0;
            }
            state = 0;
        }
    }
end:
    mmal_buffer_header_release(buffer);
    if (port->is_enabled) {
        MMAL_BUFFER_HEADER_T *new_buffer = mmal_queue_get(pool->queue);
        if (new_buffer)
            mmal_port_send_buffer(port,new_buffer);
    }
}

static void video_encoder_output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    MMAL_BUFFER_HEADER_T *new_buffer;
    PORT_USERDATA *userdata = (PORT_USERDATA *) port->userdata;
    MMAL_POOL_T *pool = userdata->video_encoder_output_pool;
    //fprintf(stderr, "INFO:%s\n", __func__);

    if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CONFIG)
    {
        //this is video header, save this field and send this to incoming connections
        assert(userdata->stream_header==NULL);
        userdata->stream_header = malloc(buffer->length);
        userdata->stream_header_size = buffer->length;
        mmal_buffer_header_mem_lock(buffer);
        memcpy(userdata->stream_header, buffer->data, buffer->length);
        mmal_buffer_header_mem_unlock(buffer);
    }else if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_CODECSIDEINFO)
    {
    }else
    {
        static int srtp_frame_start = -1;

        //TODO: send data to connected clients
        //fwrite(buffer->data, 1, buffer->length, stdout);
        if (userdata->have_active_client)
        {
            //need to send key frame first
            static int frame_start = -1;
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
                frame_start = 1;
            if (frame_start==-1)
                goto next;

            mmal_buffer_header_mem_lock(buffer);
            if (write(userdata->client_fd, buffer->data, buffer->length)<0)
            {
                frame_start = -1;
                userdata->have_active_client = 0;
                printf("connection closed\n");
            }
            mmal_buffer_header_mem_unlock(buffer);
        }
next:
        if (userdata->have_active_srtp_receiver)
        {
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_KEYFRAME)
                srtp_frame_start = 1;
            if (srtp_frame_start==-1)
                goto end;

            mmal_buffer_header_mem_lock(buffer);
            srtp_sender_callback(buffer->data, buffer->length);
            mmal_buffer_header_mem_unlock(buffer);
        }else
        {
            srtp_frame_start = -1;
        }
    }
end:

    mmal_buffer_header_release(buffer);
    if (port->is_enabled) {
        MMAL_STATUS_T status;

        new_buffer = mmal_queue_get(pool->queue);

        if (new_buffer) {
            status = mmal_port_send_buffer(port, new_buffer);
        }

        if (!new_buffer || status != MMAL_SUCCESS) {
            fprintf(stderr, "Unable to return a buffer to the video port\n");
        }
    }
}

int fill_port_buffer(MMAL_PORT_T *port, MMAL_POOL_T *pool) {
    int q;
    int num = mmal_queue_length(pool->queue);

    for (q = 0; q < num; q++) {
        MMAL_BUFFER_HEADER_T *buffer = mmal_queue_get(pool->queue);
        if (!buffer) {
            fprintf(stderr, "Unable to get a required buffer %d from pool queue\n", q);
        }

        if (mmal_port_send_buffer(port, buffer) != MMAL_SUCCESS) {
            fprintf(stderr, "Unable to send a buffer to port (%d)\n", q);
        }
    }
}

static MMAL_STATUS_T connect_ports(MMAL_PORT_T *source_port,
        MMAL_PORT_T *sink_port, MMAL_CONNECTION_T **connection)
{
    MMAL_STATUS_T status;

    status =  mmal_connection_create(connection, source_port,
            sink_port, MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT);

    if (status == MMAL_SUCCESS) {
        status =  mmal_connection_enable(*connection);
        if (status != MMAL_SUCCESS) {
            fprintf(stderr,"%s: Unable to enable connection: error %d", status);
            mmal_connection_destroy(*connection);
        }
    }
    else {
        fprintf(stderr, "%s: Unable to create connection: error %d", status);
    }

    return status;
}

int setup_camera(PORT_USERDATA *userdata) {
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T *camera = 0;
    MMAL_ES_FORMAT_T *format;
    MMAL_PORT_T * camera_video_port;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: create camera %x\n", status);
        return -1;
    }
    userdata->camera = camera;
    camera_video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];

    {
        MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
            { MMAL_PARAMETER_CAMERA_CONFIG, sizeof (cam_config)},
            .max_stills_w = VIDEO_WIDTH,
            .max_stills_h = VIDEO_HEIGHT,
            .stills_yuv422 = 0,
            .one_shot_stills = 0,
            .max_preview_video_w = VIDEO_WIDTH,
            .max_preview_video_h = VIDEO_HEIGHT,
            .num_preview_video_frames = 3,
            .stills_capture_circular_buffer_height = 0,
            .fast_preview_resume = 0,
            .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
        };
        mmal_port_parameter_set(camera->control, &cam_config.hdr);
    }

    // Setup camera video port format 
    format = camera_video_port->format;
    format->encoding = MMAL_ENCODING_I420;
    format->encoding_variant = MMAL_ENCODING_I420;
    format->es->video.width = VIDEO_WIDTH;
    format->es->video.height = VIDEO_HEIGHT;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = VIDEO_WIDTH;
    format->es->video.crop.height = VIDEO_HEIGHT;
    format->es->video.frame_rate.num = VIDEO_FPS;
    format->es->video.frame_rate.den = 1;

    camera_video_port->buffer_size = camera_video_port->buffer_size_recommended;
    camera_video_port->buffer_num = camera_video_port->buffer_num_recommended;

    fprintf(stderr, "INFO:camera video buffer_size = %d\n", camera_video_port->buffer_size);
    fprintf(stderr, "INFO:camera video buffer_num = %d\n", camera_video_port->buffer_num);

    status = mmal_port_format_commit(camera_video_port);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to commit camera video port format (%u)\n", status);
        return -1;
    }

    camera_video_port->userdata = (struct MMAL_PORT_USERDATA_T *) userdata;

    if (mmal_component_enable(camera) != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to enable camera (%u)\n", status);
        return -1;
    }

    fprintf(stderr, "INFO: camera created\n");
    return 0;
}

int setup_splitter(PORT_USERDATA *userdata) {
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T* splitter_component;
    MMAL_PORT_T *input_port;
    MMAL_PORT_T *source_port = userdata->camera->output[MMAL_CAMERA_VIDEO_PORT];

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER, &splitter_component);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "%s: Failed to create splitter component");
        goto error;
    }

    input_port = splitter_component->input[0];
    mmal_format_copy(input_port->format, source_port->format);
    input_port->buffer_num = 3;
    status = mmal_port_format_commit(input_port);
    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "Couldn't set splitter input port format : error %d\n", status);
        goto error;
    }

    for(int i = 0; i < splitter_component->output_num; i++)
    {
        printf("setting up splitter output format:%d\n", i);
        MMAL_PORT_T *output_port = splitter_component->output[i];
        output_port->buffer_num = 3;
        mmal_format_copy(output_port->format,input_port->format);
        status = mmal_port_format_commit(output_port);
        if (status != MMAL_SUCCESS)
        {
            fprintf(stderr,"Couldn't set splitter output port format : error %d\n", status);
            goto error;
        }
    }

    userdata->splitter_component = splitter_component;

    if (connect_ports(source_port,
            splitter_component->input[0],
            &userdata->splitter_connection)!=MMAL_SUCCESS)
    {
        fprintf(stderr,"can't connect splitter ports\n");
        goto error;
    }

    return 0;

error:
    if (splitter_component) {
        mmal_component_destroy(splitter_component);
    }
    return -1;


}

int setup_resizer(PORT_USERDATA *userdata, int width, int height) {
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T *resize_component;
    MMAL_PORT_T *input_port;

    MMAL_PORT_T *source_port = userdata->splitter_component->output[1];

    status = mmal_component_create("vc.ril.resize", &resize_component);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Failed to create resize component\n");
        goto error;
    }
    userdata->resize_component = resize_component;

    input_port = resize_component->input[0];
    mmal_format_copy(input_port->format, source_port->format);
    input_port->buffer_num = 3;
    status = mmal_port_format_commit(input_port);
    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "Couldn't set resize input port format : error %d\n", status);
        goto error;
    }

    MMAL_PORT_T *output_port = resize_component->output[0];
    output_port->buffer_num = 3;
    mmal_format_copy(output_port->format,input_port->format);
    output_port->format->es->video.width = width;
    output_port->format->es->video.height = height;
    output_port->format->es->video.crop.x = 0;
    output_port->format->es->video.crop.y = 0;
    output_port->format->es->video.crop.width = width;
    output_port->format->es->video.crop.height = height;

    status = mmal_port_format_commit(output_port);
    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "Couldn't set resize output port format : error %d\n", status);
        goto error;
    }
    if (connect_ports(source_port,
            resize_component->input[0],
            &userdata->resize_connection)!=MMAL_SUCCESS)
    {
        fprintf(stderr,"can't connect resize ports\n");
        goto error;
    }

    return 0;

error:
    if (resize_component) {
        mmal_component_destroy(resize_component);
    }
    return -1;
}

int setup_video_encoder(PORT_USERDATA *userdata) {
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *source_port = userdata->splitter_component->output[0];

    MMAL_PORT_T *encoder_input_port = NULL, *encoder_output_port = NULL;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to create preview (%u)\n", status);
        return -1;
    }
    userdata->video_encoder = encoder;

    encoder_input_port = encoder->input[0];
    encoder_output_port = encoder->output[0];

    mmal_format_copy(encoder_input_port->format, source_port->format);
    mmal_format_copy(encoder_output_port->format, encoder_input_port->format);

    encoder_output_port->buffer_size = encoder_output_port->buffer_size_recommended;
    encoder_output_port->buffer_num = 2;
    // Commit the port changes to the input port 
    status = mmal_port_format_commit(encoder_input_port);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to commit encoder input port format (%u)\n", status);
        return -1;
    }

    // Only supporting H264 at the moment
    encoder_output_port->format->encoding = MMAL_ENCODING_H264;
    encoder_output_port->format->bitrate = 300000;
    encoder_output_port->format->es->video.crop.width = VIDEO_WIDTH;
    encoder_output_port->format->es->video.crop.height = VIDEO_HEIGHT;
    encoder_output_port->format->es->video.frame_rate.num = VIDEO_FPS;
    encoder_output_port->format->es->video.frame_rate.den = 1;

    encoder_output_port->buffer_size = encoder_output_port->buffer_size_recommended;

    if (encoder_output_port->buffer_size < encoder_output_port->buffer_size_min) {
        encoder_output_port->buffer_size = encoder_output_port->buffer_size_min;
    }

    encoder_output_port->buffer_num = encoder_output_port->buffer_num_recommended;

    if (encoder_output_port->buffer_num < encoder_output_port->buffer_num_min) {
        encoder_output_port->buffer_num = encoder_output_port->buffer_num_min;
    }


    // Commit the port changes to the output port    
    status = mmal_port_format_commit(encoder_output_port);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to commit encoder output port format (%u)\n", status);
        return -1;
    }

    //configure H264 profile
    MMAL_PARAMETER_VIDEO_PROFILE_T  param;
    param.hdr.id = MMAL_PARAMETER_PROFILE;
    param.hdr.size = sizeof(param);

    //param.profile[0].profile = MMAL_VIDEO_PROFILE_H264_MAIN;
    param.profile[0].profile = MMAL_VIDEO_PROFILE_H264_HIGH;
    param.profile[0].level = MMAL_VIDEO_LEVEL_H264_4; // This is the only value supported

    status = mmal_port_parameter_set(encoder_output_port, &param.hdr);
    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr,"Unable to set H264 profile\n");
    }

    fprintf(stderr, " encoder input buffer_size = %d\n", encoder_input_port->buffer_size);
    fprintf(stderr, " encoder input buffer_num = %d\n", encoder_input_port->buffer_num);

    fprintf(stderr, " encoder output buffer_size = %d\n", encoder_output_port->buffer_size);
    fprintf(stderr, " encoder output buffer_num = %d\n", encoder_output_port->buffer_num);

    encoder_input_port->userdata = (struct MMAL_PORT_USERDATA_T *) userdata;

    if (mmal_component_enable(encoder) != MMAL_SUCCESS)
    {
        fprintf(stderr,"can't enable encoder component\n");
        return -1;
    }

    connect_ports(source_port,
            encoder_input_port, &userdata->video_encoder_connection);

    userdata->video_encoder_output_pool
        = (MMAL_POOL_T *) mmal_port_pool_create(encoder_output_port,
                encoder_output_port->buffer_num, encoder_output_port->buffer_size);
    encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *) userdata;

    status = mmal_port_enable(encoder_output_port, video_encoder_output_callback);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to enable encoder output port (%u)\n", status);
        return -1;
    }
    fill_port_buffer(encoder_output_port, userdata->video_encoder_output_pool);
    fprintf(stderr, "INFO:Encoder has been created\n");
    return 0;
}

int setup_jpeg_encoder(PORT_USERDATA *userdata) {
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T *jpeg_component = 0;
    MMAL_PORT_T *source_port = userdata->resize_component->output[0];
    MMAL_PORT_T *input_port, *output_port;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &jpeg_component);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr,"can't create jpeg component\n");
        goto error;
    }
    userdata->jpeg_component = jpeg_component;

    input_port = jpeg_component->input[0];
    output_port = jpeg_component->output[0];
    mmal_format_copy(output_port->format, input_port->format);

    output_port->format->encoding = MMAL_ENCODING_JPEG;
    output_port->buffer_size = output_port->buffer_size_recommended;
    output_port->buffer_num = output_port->buffer_num_recommended;

    if (output_port->buffer_size < output_port->buffer_size_min)
    {
        output_port->buffer_size = output_port->buffer_size_min;
    }
    if (output_port->buffer_num < output_port->buffer_num_min)
    {
        output_port->buffer_num = output_port->buffer_num_min;
    }

    status = mmal_port_format_commit(output_port);
    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "%s:Couldn't set jpeg output port format : error %d", status);
        goto error;
    }

    status = mmal_port_parameter_set_uint32(output_port,
            MMAL_PARAMETER_JPEG_Q_FACTOR, 100);
    if (status != MMAL_SUCCESS)
    {
        fprintf(stderr, "%s:Couldn't set jpeg quality : error %d", status);
        goto error;
    }

    userdata->jpeg_encoder_output_pool
        = (MMAL_POOL_T *) mmal_port_pool_create(output_port,
                output_port->buffer_num, output_port->buffer_size);
    output_port->userdata = (struct MMAL_PORT_USERDATA_T *) userdata;

    connect_ports(source_port,
            jpeg_component->input[0], &userdata->jpeg_connection);

    if (mmal_port_enable(output_port, jpeg_encoder_output_buffer_callback)!=MMAL_SUCCESS)
    {
        fprintf(stderr,"can't enable jpeg encoder output port");
        goto error;
    }

    fill_port_buffer(output_port, userdata->jpeg_encoder_output_pool);

    return 0;

error:
    if (jpeg_component) {
        mmal_component_destroy(jpeg_component);
    }
    return -1;

}

void send_html_response(int fd, const char* body)
{
    char* http_header = calloc(1024, sizeof(char));

    int header_len = snprintf(http_header, 1024,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: image/jpeg\r\n"
            "Content-Length: %d\r\n"
            "Connection: keep-alive\r\n\r\n", strlen(body));

    write(fd, http_header, header_len);
    write(fd, body, strlen(body));

    free(http_header);
}

int server_on_url(http_parser *parser, const char *data, size_t length)
{
    int filedes = *(int*)parser->data;

    if (length>=userdata.last_url_size)
    {
        char* p = realloc(userdata.last_url, length+1);
        if (p!=NULL)
        {
            userdata.last_url =  p;
            userdata.last_url_size = length+1;
        }else
            return -1;//failed to allocate memory
    }
    memcpy(userdata.last_url, data, length);
    userdata.last_url[length] = 0;//trailing zero


    if (parser->method == HTTP_GET) {
        if (!strncmp(data, "/snapshot", length)) {
            printf("request /snapshot\n");
            //reply with image data
            pthread_mutex_lock(&userdata.img_lock);
            userdata.snapshot_request_n = 1;

            while(userdata.snapshot_request_n)
                pthread_cond_wait(&userdata.img_cond, &userdata.img_lock);

            char* http_header = calloc(1024, sizeof(char));

            int header_len = snprintf(http_header, 1024,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: image/jpeg\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: keep-alive\r\n\r\n", userdata.image_size);

            write(filedes, http_header, header_len);
            write(filedes, userdata.image_buffer, userdata.image_size);

            free(http_header);

            pthread_mutex_unlock(&userdata.img_lock);
        }else if (!strncmp(data, "/live", length)) {
            //TODO: stream live video
            printf("request /live\n");

            char* http_header = calloc(1024, sizeof(char));

            int header_len = snprintf(http_header, 1024,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: video/h264\r\n"
                    "Connection: keep-alive\r\n\r\n");

            write(filedes, http_header, header_len);

            free(http_header);
            write(filedes, userdata.stream_header, userdata.stream_header_size);

            userdata.client_fd = filedes;
            userdata.have_active_client = 1;
        }else if (!strncmp(data, "/stop_srtp", length)) {
            printf("request /stop_srtp\n");
            userdata.have_active_srtp_receiver = 0;
            send_html_response(filedes, "OK");
        }
    }
    return 0;
}

int server_on_body(http_parser *parser, const char* data, size_t length)
{
    int filedes = *(int*)parser->data;
    if (parser->method == HTTP_POST)
    {
        int url_len = strlen(userdata.last_url);
        if (!strncmp(userdata.last_url, "/srtp_cast_to", url_len))
        {
            //prepare srtp and cast to address
            send_html_response(filedes, "OK");
            //initialize srtp backend
            //need remote address, port, ssrc, key, video header, video header length
            char* body = malloc(length+1);
            memcpy(body, data, length);
            body[length] = 0;
            cJSON* srtp_cfg = cJSON_Parse(body);

            //prepare_srtp_sender(remote_address, port, ssrc, key,
            //                        userdata.video_header, userdata.video_header_length);
            const cJSON* json_addr = cJSON_GetObjectItemCaseSensitive(srtp_cfg, "addr");
            const char* remote_address = json_addr->valuestring;
            assert(cJSON_IsString(json_addr));

            const cJSON* json_port = cJSON_GetObjectItemCaseSensitive(srtp_cfg, "port");
            assert(cJSON_IsNumber(json_port));
            const int port = json_port->valueint;

            const cJSON* json_ssrc = cJSON_GetObjectItemCaseSensitive(srtp_cfg, "ssrc");
            assert(cJSON_IsNumber(json_ssrc));
            const int ssrc = json_ssrc->valueint;

            const cJSON* json_key = cJSON_GetObjectItemCaseSensitive(srtp_cfg, "key");
            assert(cJSON_IsString(json_key));
            const char* key = json_key->valuestring;

            prepare_srtp_sender(remote_address,
                    port, 
                    ssrc,
                    key);
            cJSON_Delete(srtp_cfg);
            free(body);

            srtp_sender_callback(userdata.stream_header, userdata.stream_header_size);
            userdata.have_active_srtp_receiver = 1;
        }else
        {
            send_html_response(filedes, "unknown command");
        }
    }
}

static http_parser_settings site_setting = {
    .on_url = server_on_url,
    .on_body = server_on_body,
    //    .on_message_complete = server_on_message_complete,
};

#define MAXMSG 512

int handle_request(int filedes)
{

    char buffer[MAXMSG];
    int nbytes;

    nbytes = read (filedes, buffer, MAXMSG);
    if (nbytes < 0)
    {
        /* Read error. */
        perror ("read");
        //exit (EXIT_FAILURE);
        return -1;
    }
    else if (nbytes == 0)
    {
        /* End-of-file. */
        return -1;
    } else
    {
        /* Data read. */
        //fprintf (stderr, "Server: got message: `%s'\n", buffer);
        http_parser parser;
        parser.data = &filedes;
        http_parser_init(&parser, HTTP_REQUEST);
        int len = strlen(buffer);
        http_parser_execute(&parser, &site_setting, buffer, len>MAXMSG ? MAXMSG : len);

        return 0;
    }

}

void create_server()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    fd_set active_fd_set;
    int max_fd;
    int flag = 1;
    struct sockaddr_in serv_addr;
    struct sockaddr_in clientname;


    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    //serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serv_addr.sin_port = htons(PORT);

    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    bind(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(sock, 10);

    FD_ZERO(&active_fd_set);
    FD_SET(sock, &active_fd_set);

    while(1) {
        fd_set read_fd_set;
        struct timeval timeout = { 1, 0 }; /* 1 second timeout */

        memcpy(&read_fd_set, &active_fd_set, sizeof(read_fd_set));
        if (select (FD_SETSIZE, &read_fd_set, NULL, NULL, &timeout) < 0)
        {
            perror("select");
            exit(EXIT_FAILURE);
        }

        /* Service all the sockets with input pending. */
        for (int i = 0; i < FD_SETSIZE; ++i)
        {
            if (FD_ISSET (i, &read_fd_set))
            {
                if (i == sock)
                {
                    /* Connection request on original socket. */
                    int new;
                    size_t size = sizeof (clientname);
                    new = accept (sock,
                            (struct sockaddr *) &clientname,
                            &size);
                    if (new < 0)
                    {
                        perror ("accept");
                        exit (EXIT_FAILURE);
                    }
                    fprintf (stderr,
                            "Server: connect from host %s, port %d.\n",
                            inet_ntoa (clientname.sin_addr),
                            ntohs (clientname.sin_port));
                    FD_SET (new, &active_fd_set);
                }
                else
                {
                    /* Data arriving on an already-connected socket. */
                    if (handle_request (i) < 0)
                    {
                        close (i);
                        FD_CLR (i, &active_fd_set);
                    }
                }
            }
        }
    }
}

int main(int argc, char** argv) {

    MMAL_STATUS_T status;

    srtp_backend_init();

    memset(&userdata, 0, sizeof (PORT_USERDATA));

    userdata.width = VIDEO_WIDTH;
    userdata.height = VIDEO_HEIGHT;
    //userdata.fps = 0.0;
    userdata.have_active_client = 0;

    userdata.last_url = calloc(1, 16);
    userdata.last_url_size = 16;

    //uncompressed image
    userdata.image_max_size = IMAGE_BUFFER_SIZE;
    userdata.image_buffer = calloc(IMAGE_BUFFER_SIZE, sizeof(uint8_t));

    pthread_mutex_init(&userdata.img_lock, NULL);
    pthread_cond_init(&userdata.img_cond, NULL);
    userdata.snapshot_request_n = 0;

    fprintf(stderr, "VIDEO_WIDTH : %i\n", userdata.width );
    fprintf(stderr, "VIDEO_HEIGHT: %i\n", userdata.height );
    fprintf(stderr, "VIDEO_FPS   : %i\n",  VIDEO_FPS);
    fprintf(stderr, "Running...\n");

    bcm_host_init();


    //setup camera
    printf("creat camera\n");
    if (setup_camera(&userdata)) {
        fprintf(stderr, "Error: setup camera %x\n", status);
        return -1;
    }
    printf("creat splitter\n");
    //setup splitter and connect camera output to splitter
    if (setup_splitter(&userdata))
    {
        fprintf(stderr, "Error: setup splitter %x\n", status);
        return -1;
    }
    printf("creat resizer\n");
    //setup resize component and connect splitter output to resize component
    if (setup_resizer(&userdata, SNAPSHOT_WIDTH, SNAPSHOT_HEIGHT))
    {
        fprintf(stderr, "Error: can't setup resize component\n");
        return -1;
    }

    //setup h264 video encoder and connect splitter output to h264 encoder commponent
    printf("creat video encoder\n");
    if (setup_video_encoder(&userdata)) {
        fprintf(stderr, "Error: setup encoder %x\n", status);
        return -1;
    }

    //initialize h264 video output
    //mmal_output_init("mmalcam", &userdata->camera_output,
    //        userdata->resize_component->output[0], 0);

    //setup jpeg encoder and connect resizer component to jpeg component
    printf("creat jpeg encoder\n");
    if (setup_jpeg_encoder(&userdata)) {
        fprintf(stderr, "Error: setup encoder %x\n", status);
        return -1;
    }

    if (mmal_port_parameter_set_boolean(
                userdata.camera->output[MMAL_CAMERA_VIDEO_PORT],
                MMAL_PARAMETER_CAPTURE, 1)!= MMAL_SUCCESS)
    {
        printf("%s: Failed to start capture\n", __func__);
        return -1;
    }


    //initialize jpeg snapshot output

    //create a server and handle request, never return
    create_server();

    return 0;
}

