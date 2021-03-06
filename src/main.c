/*
 * Copyright (c) 2018-2020, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <cuda_runtime_api.h>
#include <glib.h>
#include <gst/gst.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gstnvdsmeta.h"

#define PGIE_CONFIG_FILE "../pgie_config.txt"
#define SGIE1_CONFIG_FILE "../sgie1_config.txt"
#define SGIE2_CONFIG_FILE "../sgie2_config.txt"
#define SGIE3_CONFIG_FILE "../sgie3_config.txt"
#define MAX_DISPLAY_LEN 64

#define TRACKER_CONFIG_FILE "../tracker_config.txt"
#define MAX_TRACKING_ID_LEN 16

#define PGIE_CLASS_ID_VEHICLE 0
#define PGIE_CLASS_ID_PERSON 2

/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 40000

gint frame_number = 0;
/* These are the strings of the labels for the respective models */
gchar sgie1_classes_str[12][32] = {"black", "blue",   "brown",  "gold",
                                   "green", "grey",   "maroon", "orange",
                                   "red",   "silver", "white",  "yellow"};

gchar sgie2_classes_str[20][32] = {
    "Acura", "Audi",     "BMW",     "Chevrolet", "Chrysler", "Dodge",     "Ford",
    "GMC",   "Honda",    "Hyundai", "Infiniti",  "Jeep",     "Kia",       "Lexus",
    "Mazda", "Mercedes", "Nissan",  "Subaru",    "Toyota",   "Volkswagen"};

gchar sgie3_classes_str[6][32] = {"coupe", "largevehicle", "sedan",
                                  "suv",   "truck",        "van"};

gchar pgie_classes_str[4][32] = {"Vehicle", "TwoWheeler", "Person", "RoadSign"};

/* gie_unique_id is one of the properties in the above dstest2_sgiex_config.txt
 * files. These should be unique and known when we want to parse the Metadata
 * respective to the sgie labels. Ideally these should be read from the config
 * files but for brevity we ensure they are same. */

guint sgie1_unique_id = 2;
guint sgie2_unique_id = 3;
guint sgie3_unique_id = 4;

/* Wrapper function for handling CUDA runtime calls */
void cudaCheckError(cudaError_t err) {
    if (err != cudaSuccess) {
        g_printerr("%s\n", cudaGetErrorString(err));
    }
}

#define SIZE 64
static double x_history[SIZE];
static double avg_x_movement = 0.0;
double threshold = 5.0;
static bool is_loitering = FALSE;

double process_history(double buf[SIZE]) {
    // get pixel deltas
    for (int i = SIZE - 1; i > 0; i--) {
        buf[i] = fabs(buf[i] - buf[i - 1]);
        g_printf("%.1lf\t", buf[i]);
    }
    g_printf("\r");
    // get avg
    double sum = 0.0;
    for (int i = 1; i < SIZE; i++) {
        sum += buf[i];
    }
    return sum / ((double)SIZE - 1);
}

double avg(double buf[SIZE]) {
    double sum = 0.0;
    for (int i = 0; i < SIZE; i++) {
        sum += buf[i];
    }
    return sum / ((double)SIZE);
}
/* This is the buffer probe function that we have registered on the sink pad
 * of the OSD element. All the infer elements in the pipeline shall attach
 * their metadata to the GstBuffer, here we will iterate & process the metadata
 * forex: class ids to strings, counting of class_id objects etc. */
static GstPadProbeReturn osd_sink_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info,
                                                   gpointer u_data) {
    GstBuffer *buf = (GstBuffer *)info->data;
    guint num_rects = 0;
    NvDsObjectMeta *obj_meta = NULL;
    NvDsMetaList *l_frame = NULL;
    NvDsMetaList *l_obj = NULL;
    NvDsDisplayMeta *display_meta = NULL;
    double temp;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL;
         l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);
        int offset = 0;
        for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
            obj_meta = (NvDsObjectMeta *)(l_obj->data);
            if (obj_meta->class_id == PGIE_CLASS_ID_VEHICLE) {
                num_rects++;
            }
            if (obj_meta->class_id == PGIE_CLASS_ID_PERSON) {
                num_rects++;
                x_history[frame_meta->frame_num % SIZE] = obj_meta->rect_params.left;
                temp = obj_meta->rect_params.left;

                if (frame_meta->frame_num % SIZE == 0 &&
                    frame_meta->frame_num > (SIZE - 1)) {
                    avg_x_movement = process_history(x_history);
                    is_loitering = fabs(avg_x_movement) < threshold;
                }
                if (is_loitering) {
                    obj_meta->rect_params.border_color.red = 0.0;
                    obj_meta->rect_params.border_color.blue = 1.0;
                }
            }
        }
        display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        NvOSD_TextParams *txt_params = &display_meta->text_params[0];
        display_meta->num_labels = 1;
        txt_params->display_text = g_malloc0(MAX_DISPLAY_LEN);
        offset = snprintf(txt_params->display_text, MAX_DISPLAY_LEN, "left = %lf ",
                          avg_x_movement);
        offset = snprintf(txt_params->display_text + offset, MAX_DISPLAY_LEN,
                          "top = %lf ", temp);

        /* Now set the offsets where the string should appear */
        txt_params->x_offset = 10;
        txt_params->y_offset = 12;

        /* Font , font-color and font-size */
        txt_params->font_params.font_name = "Serif";
        txt_params->font_params.font_size = 10;
        txt_params->font_params.font_color.red = 1.0;
        txt_params->font_params.font_color.green = 1.0;
        txt_params->font_params.font_color.blue = 1.0;
        txt_params->font_params.font_color.alpha = 1.0;

        /* Text background color */
        txt_params->set_bg_clr = 1;
        txt_params->text_bg_clr.red = 0.0;
        txt_params->text_bg_clr.green = 0.0;
        txt_params->text_bg_clr.blue = 0.0;
        txt_params->text_bg_clr.alpha = 1.0;

        nvds_add_display_meta_to_frame(frame_meta, display_meta);
    }

    return GST_PAD_PROBE_OK;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(msg->src),
                       error->message);
            if (debug) g_printerr("Error details: %s\n", debug);
            g_free(debug);
            g_error_free(error);
            g_main_loop_quit(loop);
            break;
        }
        default:
            break;
    }
    return TRUE;
}

/* Tracker config parsing */

#define CHECK_ERROR(error)                                                   \
    if (error) {                                                             \
        g_printerr("Error while parsing config file: %s\n", error->message); \
        goto done;                                                           \
    }

#define CONFIG_GROUP_TRACKER "tracker"
#define CONFIG_GROUP_TRACKER_WIDTH "tracker-width"
#define CONFIG_GROUP_TRACKER_HEIGHT "tracker-height"
#define CONFIG_GROUP_TRACKER_LL_CONFIG_FILE "ll-config-file"
#define CONFIG_GROUP_TRACKER_LL_LIB_FILE "ll-lib-file"
#define CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS "enable-batch-process"
#define CONFIG_GPU_ID "gpu-id"

// Certain components dynamically generate their sources, so they can't be linked
// immediately
static void link_source_pad_to_pipe(GstElement *src, GstPad *new_src_pad,
                                    GstElement *sink_elem) {
    GstPad *sink_pad = gst_element_get_static_pad(sink_elem, "sink");
    g_print("Source Pad was created with name %s\n", gst_pad_get_name(new_src_pad));
    GstPadLinkReturn status = gst_pad_link(new_src_pad, sink_pad);
    if (GST_PAD_LINK_FAILED(status)) {
        g_printerr("Type is %s but link failed",
                   gst_structure_get_name(gst_caps_get_structure(
                       gst_pad_get_current_caps(new_src_pad), 0)));
    }
}

static gchar *get_absolute_file_path(gchar *cfg_file_path, gchar *file_path) {
    gchar abs_cfg_path[PATH_MAX + 1];
    gchar *abs_file_path;
    gchar *delim;

    if (file_path && file_path[0] == '/') {
        return file_path;
    }

    if (!realpath(cfg_file_path, abs_cfg_path)) {
        g_free(file_path);
        return NULL;
    }

    // Return absolute path of config file if file_path is NULL.
    if (!file_path) {
        abs_file_path = g_strdup(abs_cfg_path);
        return abs_file_path;
    }

    delim = g_strrstr(abs_cfg_path, "/");
    *(delim + 1) = '\0';

    abs_file_path = g_strconcat(abs_cfg_path, file_path, NULL);
    g_free(file_path);

    return abs_file_path;
}

static gboolean set_tracker_properties(GstElement *nvtracker) {
    gboolean ret = FALSE;
    GError *error = NULL;
    gchar **keys = NULL;
    gchar **key = NULL;
    GKeyFile *key_file = g_key_file_new();

    if (!g_key_file_load_from_file(key_file, TRACKER_CONFIG_FILE, G_KEY_FILE_NONE,
                                   &error)) {
        g_printerr("Failed to load config file: %s\n", error->message);
        return FALSE;
    }

    keys = g_key_file_get_keys(key_file, CONFIG_GROUP_TRACKER, NULL, &error);
    CHECK_ERROR(error);

    for (key = keys; *key; key++) {
        if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_WIDTH)) {
            gint width = g_key_file_get_integer(key_file, CONFIG_GROUP_TRACKER,
                                                CONFIG_GROUP_TRACKER_WIDTH, &error);
            CHECK_ERROR(error);
            g_object_set(G_OBJECT(nvtracker), "tracker-width", width, NULL);
        } else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_HEIGHT)) {
            gint height = g_key_file_get_integer(key_file, CONFIG_GROUP_TRACKER,
                                                 CONFIG_GROUP_TRACKER_HEIGHT, &error);
            CHECK_ERROR(error);
            g_object_set(G_OBJECT(nvtracker), "tracker-height", height, NULL);
        } else if (!g_strcmp0(*key, CONFIG_GPU_ID)) {
            guint gpu_id = g_key_file_get_integer(key_file, CONFIG_GROUP_TRACKER,
                                                  CONFIG_GPU_ID, &error);
            CHECK_ERROR(error);
            g_object_set(G_OBJECT(nvtracker), "gpu_id", gpu_id, NULL);
        } else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_LL_CONFIG_FILE)) {
            char *ll_config_file = get_absolute_file_path(
                TRACKER_CONFIG_FILE,
                g_key_file_get_string(key_file, CONFIG_GROUP_TRACKER,
                                      CONFIG_GROUP_TRACKER_LL_CONFIG_FILE, &error));
            CHECK_ERROR(error);
            g_object_set(G_OBJECT(nvtracker), "ll-config-file", ll_config_file, NULL);
        } else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_LL_LIB_FILE)) {
            char *ll_lib_file = get_absolute_file_path(
                TRACKER_CONFIG_FILE,
                g_key_file_get_string(key_file, CONFIG_GROUP_TRACKER,
                                      CONFIG_GROUP_TRACKER_LL_LIB_FILE, &error));
            CHECK_ERROR(error);
            g_object_set(G_OBJECT(nvtracker), "ll-lib-file", ll_lib_file, NULL);
        } else if (!g_strcmp0(*key, CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS)) {
            gboolean enable_batch_process = g_key_file_get_integer(
                key_file, CONFIG_GROUP_TRACKER,
                CONFIG_GROUP_TRACKER_ENABLE_BATCH_PROCESS, &error);
            CHECK_ERROR(error);
            g_object_set(G_OBJECT(nvtracker), "enable_batch_process",
                         enable_batch_process, NULL);
        } else {
            g_printerr("Unknown key '%s' for group [%s]", *key, CONFIG_GROUP_TRACKER);
        }
    }

    ret = TRUE;
done:
    if (error) {
        g_error_free(error);
    }
    if (keys) {
        g_strfreev(keys);
    }
    if (!ret) {
        g_printerr("%s failed", __func__);
    }
    return ret;
}

int main(int argc, char *argv[]) {
    GMainLoop *loop = NULL;
    GstElement *pipeline = NULL, *source = NULL, *depay = NULL, *h264parser = NULL,
               *decoder = NULL, *streammux = NULL, *sink = NULL, *pgie = NULL,
               *nvvidconv = NULL, *nvosd = NULL, *sgie1 = NULL, *sgie2 = NULL,
               *sgie3 = NULL, *nvtracker = NULL;
    g_print("With tracker\n");
    GstElement *transform = NULL;
    GstBus *bus = NULL;
    guint bus_watch_id = 0;
    GstPad *osd_sink_pad = NULL;

    int current_device = -1;
    cudaCheckError(cudaGetDevice(&current_device));
    struct cudaDeviceProp prop;
    cudaCheckError(cudaGetDeviceProperties(&prop, current_device));

    /* Check input arguments */
    if (argc != 2) {
        g_printerr("Usage: %s <elementary H265 filename>\n", argv[0]);
        return -1;
    }

    /* Standard GStreamer initialization */
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    /* Create gstreamer elements */

    /* Create Pipeline element that will be a container of other elements */
    pipeline = gst_pipeline_new("dstest2-pipeline");

    /* Source element for reading from the RTSP Stream*/
    source = gst_element_factory_make("rtspsrc", "source");

    depay = gst_element_factory_make("rtph264depay", "depay");

    h264parser = gst_element_factory_make("h264parse", "parser");

    /* Use nvdec_h264 for hardware accelerated decode on GPU */
    decoder = gst_element_factory_make("nvv4l2decoder", "nvv4l2-decoder");

    /* Create nvstreammux instance to form batches from one or more sources. */
    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");

    if (!pipeline || !streammux) {
        g_printerr("One element could not be created. Exiting.\n");
        return -1;
    }
    g_print("Got this far\n");

    /* Use nvinfer to run inferencing on decoder's output,
     * behaviour of inferencing is set through config file */
    pgie = gst_element_factory_make("nvinfer", "primary-nvinference-engine");

    /* We need to have a tracker to track the identified objects */
    nvtracker = gst_element_factory_make("nvtracker", "tracker");

    /* We need three secondary gies so lets create 3 more instances of
       nvinfer */
    sgie1 = gst_element_factory_make("nvinfer", "secondary1-nvinference-engine");

    sgie2 = gst_element_factory_make("nvinfer", "secondary2-nvinference-engine");

    sgie3 = gst_element_factory_make("nvinfer", "secondary3-nvinference-engine");

    /* Use convertor to convert from NV12 to RGBA as required by nvosd */
    nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");

    /* Create OSD to draw on the converted RGBA buffer */
    nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay");

    /* Finally render the osd output */
    if (prop.integrated) {
        transform = gst_element_factory_make("nvegltransform", "nvegl-transform");
    }
    sink = gst_element_factory_make("nveglglessink", "nvvideo-renderer");

    if (!source || !depay || !h264parser || !decoder || !pgie || !nvtracker || !sgie1 ||
        !sgie2 || !sgie3 || !nvvidconv || !nvosd || !sink) {
        g_printerr("One element could not be created. Exiting.\n");
        return -1;
    }

    if (!transform && prop.integrated) {
        g_printerr("One tegra element could not be created. Exiting.\n");
        return -1;
    }

    /* Set the input filename to the source element */
    g_object_set(G_OBJECT(source), "location", argv[1], NULL);

    g_object_set(G_OBJECT(streammux), "batch-size", 1, NULL);

    g_object_set(G_OBJECT(streammux), "width", MUXER_OUTPUT_WIDTH, "height",
                 MUXER_OUTPUT_HEIGHT, "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC,
                 NULL);

    g_signal_connect(source, "pad-added", G_CALLBACK(link_source_pad_to_pipe), depay);

    /* Set all the necessary properties of the nvinfer element,
     * the necessary ones are : */
    g_object_set(G_OBJECT(pgie), "config-file-path", PGIE_CONFIG_FILE, NULL);
    g_object_set(G_OBJECT(sgie1), "config-file-path", SGIE1_CONFIG_FILE, NULL);
    g_object_set(G_OBJECT(sgie2), "config-file-path", SGIE2_CONFIG_FILE, NULL);
    g_object_set(G_OBJECT(sgie3), "config-file-path", SGIE3_CONFIG_FILE, NULL);

    /* Set necessary properties of the tracker element. */
    if (!set_tracker_properties(nvtracker)) {
        g_printerr("Failed to set tracker properties. Exiting.\n");
        return -1;
    }

    /* we add a message handler */
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
    gst_object_unref(bus);

    /* Set up the pipeline */
    /* we add all elements into the pipeline */
    /* decoder | pgie1 | nvtracker | sgie1 | sgie2 | sgie3 | etc.. */
    if (prop.integrated) {
        gst_bin_add_many(GST_BIN(pipeline), source, depay, h264parser, decoder,
                         streammux, pgie, nvtracker, sgie1, sgie2, sgie3, nvvidconv,
                         nvosd, transform, sink, NULL);
    } else {
        gst_bin_add_many(GST_BIN(pipeline), source, depay, h264parser, decoder,
                         streammux, pgie, nvtracker, sgie1, sgie2, sgie3, nvvidconv,
                         nvosd, sink, NULL);
    }

    gchar pad_name_sink[16] = "sink_0";
    gchar pad_name_src[16] = "src";

    GstPad *sinkpad = gst_element_get_request_pad(streammux, pad_name_sink);
    if (!sinkpad) {
        g_printerr("Streammux request sink pad failed. Exiting.\n");
        return -1;
    }

    GstPad *srcpad = gst_element_get_static_pad(decoder, pad_name_src);
    if (!srcpad) {
        g_printerr("Decoder request src pad failed. Exiting.\n");
        return -1;
    }

    if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
        g_printerr("Failed to link decoder to stream muxer. Exiting.\n");
        return -1;
    }

    gst_object_unref(sinkpad);
    gst_object_unref(srcpad);

    /* Link the elements together */
    if (!gst_element_link_many(depay, h264parser, decoder, NULL)) {
        g_printerr("Elements could not be linked: 1. Exiting.\n");
        return -1;
    }

    if (prop.integrated) {
        if (!gst_element_link_many(streammux, pgie, nvtracker, sgie1, sgie2, sgie3,
                                   nvvidconv, nvosd, transform, sink, NULL)) {
            g_printerr("Elements could not be linked. Exiting.\n");
            return -1;
        }
    } else {
        if (!gst_element_link_many(streammux, pgie, nvtracker, sgie1, sgie2, sgie3,
                                   nvvidconv, nvosd, sink, NULL)) {
            g_printerr("Elements could not be linked. Exiting.\n");
            return -1;
        }
    }

    /* Lets add probe to get informed of the meta data generated, we add probe to
     * the sink pad of the osd element, since by that time, the buffer would have
     * had got all the metadata. */
    osd_sink_pad = gst_element_get_static_pad(nvosd, "sink");
    if (!osd_sink_pad)
        g_print("Unable to get sink pad\n");
    else
        gst_pad_add_probe(osd_sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
                          osd_sink_pad_buffer_probe, NULL, NULL);
    gst_object_unref(osd_sink_pad);

    /* Set the pipeline to "playing" state */
    g_print("Now playing: %s\n", argv[1]);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* Iterate */
    g_print("Running...\n");
    g_main_loop_run(loop);

    /* Out of the main loop, clean up nicely */
    g_print("Returned, stopping playback\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_print("Deleting pipeline\n");
    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);
    return 0;
}
