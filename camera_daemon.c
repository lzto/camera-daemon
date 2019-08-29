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

#include "bcm_host.h"
#include "interface/vcos/vcos.h"

#include "interface/mmal/mmal.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/util/mmal_util_params.h"

#define MMAL_CAMERA_PREVIEW_PORT 0
#define MMAL_CAMERA_VIDEO_PORT 1
#define MMAL_CAMERA_CAPTURE_PORT 2

#define VIDEO_FPS 30 
#define VIDEO_WIDTH 1280
#define VIDEO_HEIGHT 720

#define IMAGE_BUFFER_SIZE 3*VIDEO_WIDTH*VIDEO_HEIGHT

#define PORT 7777

typedef struct {
    int width;
    int height;
    MMAL_COMPONENT_T *camera;
    MMAL_COMPONENT_T *encoder;
    MMAL_COMPONENT_T *preview;
    MMAL_PORT_T *camera_preview_port;
    MMAL_PORT_T *camera_video_port;
    MMAL_PORT_T *camera_still_port;
    MMAL_POOL_T *camera_video_port_pool;
    MMAL_PORT_T *encoder_input_port;
    MMAL_POOL_T *encoder_input_pool;
    MMAL_PORT_T *encoder_output_port;
    MMAL_POOL_T *encoder_output_pool;
    //producer consumer
    pthread_mutex_t img_lock;
    pthread_cond_t img_cond;
    size_t snapshot_request_n;//number of pending request for image
    uint8_t *image_buffer;
    size_t image_max_size;
    size_t image_size;
    //float fps;
} PORT_USERDATA;

PORT_USERDATA userdata;

static void camera_video_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    static int frame_count = 0;
    static struct timespec t1;
    struct timespec t2;
    uint8_t *local_image_buffer;

    //fprintf(stderr, "INFO:%s\n", __func__);
    if (frame_count == 0) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
    }
    clock_gettime(CLOCK_MONOTONIC, &t2);

    int d = t2.tv_sec - t1.tv_sec;

    MMAL_BUFFER_HEADER_T *new_buffer;
    MMAL_BUFFER_HEADER_T *output_buffer = 0;
    PORT_USERDATA *userdata = (PORT_USERDATA *) port->userdata;

    MMAL_POOL_T *pool = userdata->camera_video_port_pool;

    local_image_buffer = userdata->image_buffer;
    frame_count++;

    output_buffer = mmal_queue_get(userdata->encoder_input_pool->queue);

    if (output_buffer) {
        mmal_buffer_header_mem_lock(buffer);
        memcpy(output_buffer->data, buffer->data, buffer->length);
        userdata->image_size
            = (buffer->length > userdata->image_max_size) ? 
                userdata->image_max_size : buffer->length;

        if (userdata->snapshot_request_n)
        {
            if (pthread_mutex_trylock(&userdata->img_lock))
            {
                memcpy(local_image_buffer, buffer->data, buffer->length);
                userdata->snapshot_request_n = 0;
                pthread_cond_signal(&userdata->img_cond);
                pthread_mutex_unlock(&userdata->img_lock);
            }
        }

        output_buffer->length = buffer->length;
        mmal_buffer_header_mem_unlock(buffer);
        if (mmal_port_send_buffer(userdata->encoder_input_port, output_buffer) != MMAL_SUCCESS) {
            fprintf(stderr, "ERROR: Unable to send buffer \n");
        }
    } else {
        fprintf(stderr, "ERROR: mmal_queue_get (%d)\n", output_buffer);
    }
#if 0
    if (frame_count % 10 == 0) {
        // print framerate every n frame
        clock_gettime(CLOCK_MONOTONIC, &t2);
        float d = (t2.tv_sec + t2.tv_nsec / 1000000000.0) - (t1.tv_sec + t1.tv_nsec / 1000000000.0);
        float fps = 0.0;

        if (d > 0) {
            fps = frame_count / d;
        } else {
            fps = frame_count;
        }
        userdata->fps = fps;
        fprintf(stderr, "  Frame = %d,  Framerate = %.1f fps \n", frame_count, fps);
    }
#endif

    mmal_buffer_header_release(buffer);

    // and send one back to the port (if still open)
    if (port->is_enabled) {
        MMAL_STATUS_T status;

        new_buffer = mmal_queue_get(pool->queue);

        if (new_buffer) {
            status = mmal_port_send_buffer(port, new_buffer);
        }

        if (!new_buffer || status != MMAL_SUCCESS) {
            fprintf(stderr, "Error: Unable to return a buffer to the video port\n");
        }
    }
}
static void encoder_input_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    //fprintf(stderr, "INFO:%s\n", __func__);    
    mmal_buffer_header_release(buffer);
}

static void encoder_output_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
    MMAL_BUFFER_HEADER_T *new_buffer;
    PORT_USERDATA *userdata = (PORT_USERDATA *) port->userdata;
    MMAL_POOL_T *pool = userdata->encoder_output_pool;
    //fprintf(stderr, "INFO:%s\n", __func__);

    mmal_buffer_header_mem_lock(buffer);

    //TODO: send data to connected clients
    //fwrite(buffer->data, 1, buffer->length, stdout);

    mmal_buffer_header_mem_unlock(buffer);

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

int setup_camera(PORT_USERDATA *userdata) {
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T *camera = 0;
    MMAL_ES_FORMAT_T *format;
    MMAL_PORT_T * camera_preview_port;
    MMAL_PORT_T * camera_video_port;
    MMAL_PORT_T * camera_still_port;
    MMAL_POOL_T * camera_video_port_pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &camera);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: create camera %x\n", status);
        return -1;
    }
    userdata->camera = camera;
    userdata->camera_preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
    userdata->camera_video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
    userdata->camera_still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];

    camera_preview_port = camera->output[MMAL_CAMERA_PREVIEW_PORT];
    camera_video_port = camera->output[MMAL_CAMERA_VIDEO_PORT];
    camera_still_port = camera->output[MMAL_CAMERA_CAPTURE_PORT];


    {
        MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
            { MMAL_PARAMETER_CAMERA_CONFIG, sizeof (cam_config)},
            .max_stills_w = 1280,
            .max_stills_h = 720,
            .stills_yuv422 = 0,
            .one_shot_stills = 1,
            .max_preview_video_w = VIDEO_WIDTH,
            .max_preview_video_h = VIDEO_HEIGHT,
            .num_preview_video_frames = 3,
            .stills_capture_circular_buffer_height = 0,
            .fast_preview_resume = 0,
            .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
        };
        mmal_port_parameter_set(camera->control, &cam_config.hdr);
    }

    // Setup camera preview port format 
    format = camera_preview_port->format;
    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;
    format->es->video.width = VIDEO_WIDTH;
    format->es->video.height = VIDEO_HEIGHT;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = VIDEO_WIDTH;
    format->es->video.crop.height = VIDEO_HEIGHT;

    status = mmal_port_format_commit(camera_preview_port);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: camera viewfinder format couldn't be set\n");
        return -1;
    }

    // Setup camera video port format
    mmal_format_copy(camera_video_port->format, camera_preview_port->format);

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

    camera_video_port->buffer_size = format->es->video.width * format->es->video.height * 12 / 8;
    camera_video_port->buffer_num = 2;

    fprintf(stderr, "INFO:camera video buffer_size = %d\n", camera_video_port->buffer_size);
    fprintf(stderr, "INFO:camera video buffer_num = %d\n", camera_video_port->buffer_num);

    status = mmal_port_format_commit(camera_video_port);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to commit camera video port format (%u)\n", status);
        return -1;
    }

    camera_video_port_pool = (MMAL_POOL_T *) mmal_port_pool_create(camera_video_port, camera_video_port->buffer_num, camera_video_port->buffer_size);
    userdata->camera_video_port_pool = camera_video_port_pool;
    camera_video_port->userdata = (struct MMAL_PORT_USERDATA_T *) userdata;


    status = mmal_port_enable(camera_video_port, camera_video_buffer_callback);

    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to enable camera video port (%u)\n", status);
        return -1;
    }

    status = mmal_component_enable(camera);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to enable camera (%u)\n", status);
        return -1;
    }


    fill_port_buffer(userdata->camera_video_port, userdata->camera_video_port_pool);

    if (mmal_port_parameter_set_boolean(camera_video_port, MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS) {
        printf("%s: Failed to start capture\n", __func__);
    }

    fprintf(stderr, "INFO: camera created\n");
    return 0;
}

int setup_encoder(PORT_USERDATA *userdata) {
    MMAL_STATUS_T status;
    MMAL_COMPONENT_T *encoder = 0;
    MMAL_PORT_T *preview_input_port = NULL;

    MMAL_PORT_T *encoder_input_port = NULL, *encoder_output_port = NULL;
    MMAL_POOL_T *encoder_input_port_pool;
    MMAL_POOL_T *encoder_output_port_pool;

    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER, &encoder);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to create preview (%u)\n", status);
        return -1;
    }

    encoder_input_port = encoder->input[0];
    encoder_output_port = encoder->output[0];
    userdata->encoder_input_port = encoder_input_port;
    userdata->encoder_output_port = encoder_input_port;

    mmal_format_copy(encoder_input_port->format, userdata->camera_video_port->format);
    encoder_input_port->buffer_size = encoder_input_port->buffer_size_recommended;
    encoder_input_port->buffer_num = 2;


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
    encoder_output_port->format->bitrate = 2000000;

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

    fprintf(stderr, " encoder input buffer_size = %d\n", encoder_input_port->buffer_size);
    fprintf(stderr, " encoder input buffer_num = %d\n", encoder_input_port->buffer_num);

    fprintf(stderr, " encoder output buffer_size = %d\n", encoder_output_port->buffer_size);
    fprintf(stderr, " encoder output buffer_num = %d\n", encoder_output_port->buffer_num);

    encoder_input_port_pool = (MMAL_POOL_T *) mmal_port_pool_create(encoder_input_port, encoder_input_port->buffer_num, encoder_input_port->buffer_size);
    userdata->encoder_input_pool = encoder_input_port_pool;
    encoder_input_port->userdata = (struct MMAL_PORT_USERDATA_T *) userdata;
    status = mmal_port_enable(encoder_input_port, encoder_input_buffer_callback);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to enable encoder input port (%u)\n", status);
        return -1;
    }
    fprintf(stderr, "INFO:Encoder input pool has been created\n");


    encoder_output_port_pool = (MMAL_POOL_T *) mmal_port_pool_create(encoder_output_port, encoder_output_port->buffer_num, encoder_output_port->buffer_size);
    userdata->encoder_output_pool = encoder_output_port_pool;
    encoder_output_port->userdata = (struct MMAL_PORT_USERDATA_T *) userdata;

    status = mmal_port_enable(encoder_output_port, encoder_output_buffer_callback);
    if (status != MMAL_SUCCESS) {
        fprintf(stderr, "Error: unable to enable encoder output port (%u)\n", status);
        return -1;
    }
    fprintf(stderr, "INFO:Encoder output pool has been created\n");    

    fill_port_buffer(encoder_output_port, encoder_output_port_pool);

    fprintf(stderr, "INFO:Encoder has been created\n");
    return 0;
}


int server_on_url(http_parser *parser, const char *data, size_t length)
{
    int filedes = *(int*)parser->data;
    if (parser->method == HTTP_GET) {
        if (!strncmp(data, "/snapshot", length)) {
            printf("request /snapshot\n");
            //reply with image data
            pthread_mutex_lock(&userdata.img_lock);
            userdata.snapshot_request_n = 1;

            while(userdata.snapshot_request_n)
            {
                pthread_cond_wait(&userdata.img_cond, &userdata.img_lock);
            }

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
        }
    }
    return 0;
}

static http_parser_settings site_setting = {
    .on_url = server_on_url,
    //    .on_body = server_on_body,
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
        exit (EXIT_FAILURE);
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
        http_parser_execute(&parser, &site_setting, buffer, len>MAXMSG?MAXMSG:len);

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


    memset(&userdata, 0, sizeof (PORT_USERDATA));

    userdata.width = VIDEO_WIDTH;
    userdata.height = VIDEO_HEIGHT;
    //userdata.fps = 0.0;

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

    if (1 && setup_camera(&userdata) != 0) {
        fprintf(stderr, "Error: setup camera %x\n", status);
        return -1;
    }

    if (1 && setup_encoder(&userdata) != 0) {
        fprintf(stderr, "Error: setup encoder %x\n", status);
        return -1;
    }

    //create a server and handle request
    create_server();

    return 0;
}

