#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <signal.h>


#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>



typedef struct _GstDataStruct{
    GstElement *pipeline;
    GstElement *app_source;
    GstElement *video_convert;
    GstElement *h264_encoder;
    GstElement *app_sink;
    guint app_sink_index;
    GMainLoop *loop;  /* GLib's Main Loop */
} GstDataStruct;
 
char out_file_name[32];
static FILE *fp;            // 打开或保存文件的的指针
static GstBus *bus;
static guint bus_watch_id;

static GstDataStruct GstData;
static  int screen_width;
static  int screen_height;
static  int frame_rate;
static  int frame_bitrate;
static 	Window desktop;
static 	Display* dsp;


static void new_h264_sample_on_appsink (GstElement *sink, GstDataStruct *pGstData);
static void eos_on_appsink (GstElement *sink, GstDataStruct *pGstData);
gboolean bus_msg_call(GstBus *bus, GstMessage *msg, GstDataStruct *pGstData);
static gboolean rj_link_convert_and_encode (GstElement *convert_element, GstElement *encode_element);
static int xDispalyInit();

/* use to link ximagesrc and vaapipostproc element*/
static gboolean rj_link_source_and_convert (GstElement *source_element, GstElement *convert_element)
{
  gboolean link_ok;
  GstCaps *caps;

  caps = gst_caps_new_simple("video/x-raw",
                "width", G_TYPE_INT, screen_width,
                "height", G_TYPE_INT, screen_height,
                "framerate",GST_TYPE_FRACTION, frame_rate, 1, NULL);
  link_ok = gst_element_link_filtered (source_element, convert_element, caps);
  gst_caps_unref (caps);

  if (!link_ok) {
    g_warning ("Failed to link source_element and convert_element!");
  }
  return link_ok;
}

static void rj_create_gst_element (const gchar *type_name, GstElement **element, const gchar *element_name)
{
    GstElementFactory *factory;
    factory = gst_element_factory_find (type_name);
    if (!factory) {
        g_print ("Failed to find factory of type '%s'\n",type_name);
        exit(-1);
    }
    *element = gst_element_factory_create (factory, element_name);
    if (!element) {
        g_print ("Failed to create element, even though its factory exists!\n");
        exit(-1);
    }
}

static void init_parameters (int *argc, char *argv[])
{
    frame_rate = 30;
    if(*argc == 2) {
        frame_rate = atoi(argv[1]);
        printf("use user new define frame_rate:%d \n", frame_rate);
    }
    
    memset (out_file_name, 0, sizeof(out_file_name));
    sprintf(out_file_name,"gst-vaencode_out_%d.h264",frame_rate);
    printf("demo output file name is :%s\n", out_file_name);

    fp = fopen(out_file_name, "wb");
    if(!fp) {
        printf("can not open file %s", out_file_name);
        exit(-1);
    }

	int ret = 0;
	ret = xDispalyInit();
	if (ret < 0) {
	    printf("xDispalyInit exe failed !\n");
		exit(-1);
	}

    gst_init (argc, &argv);
}

static void init_gst_vaecode ()
{
    /* Initialize cumstom data structure */
    printf("\n============= gst init start ============\n");
    memset (&GstData, 0, sizeof (GstDataStruct));

    /* Create gstreamer elements */
    GstData.pipeline = gst_pipeline_new ("encode_test");
    
    rj_create_gst_element("ximagesrc",&GstData.app_source,"video-source");
    rj_create_gst_element("vaapipostproc",&GstData.video_convert,"video-convert");
    rj_create_gst_element("vaapih264enc",&GstData.h264_encoder,"video-encoder");
    rj_create_gst_element("appsink",&GstData.app_sink,"video-sink");

    if (!GstData.pipeline || !GstData.app_source || !GstData.h264_encoder ||
        !GstData.video_convert || !GstData.app_sink)
    {
        g_printerr ("One element could not be created... Exit\n");
        fclose(fp);
        exit(-1);
    }
    
    printf("============ link pipeline ==============\n");

    /* Configure ximagesrc */

    g_object_set(G_OBJECT(GstData.app_source), "display-name", ":0", NULL);
    g_object_set(G_OBJECT(GstData.app_source), "use-damage", FALSE, NULL);
    g_object_set(G_OBJECT(GstData.app_source), "startx", 0, NULL);
    g_object_set(G_OBJECT(GstData.app_source), "starty", 0, NULL);
    g_object_set(G_OBJECT(GstData.app_source), "endx", screen_width, NULL);
    g_object_set(G_OBJECT(GstData.app_source), "endy", screen_height, NULL);

    /* Configure appsink */
    GstCaps *caps_app_sink;
    caps_app_sink = gst_caps_new_simple("video/x-h264", "stream-format", G_TYPE_STRING, "byte-stream",
                                        "alignment", G_TYPE_STRING, "au", NULL);
    g_object_set(G_OBJECT(GstData.app_sink), "emit-signals", TRUE, "caps", caps_app_sink, "sync", FALSE, NULL);
    g_signal_connect(GstData.app_sink, "new-sample", G_CALLBACK(new_h264_sample_on_appsink), &GstData);
    g_signal_connect(GstData.app_sink, "eos", G_CALLBACK(eos_on_appsink), &GstData);

    /* Configure other elements */
    g_object_set(G_OBJECT(GstData.h264_encoder), "bitrate", frame_bitrate, NULL);

    bus = gst_pipeline_get_bus(GST_PIPELINE(GstData.pipeline));
    bus_watch_id = gst_bus_add_watch(bus, (GstBusFunc)bus_msg_call, (gpointer)&GstData);
    gst_object_unref(bus);

    /* link elements*/
    gst_bin_add_many(GST_BIN(GstData.pipeline), GstData.app_source, GstData.video_convert,
            GstData.h264_encoder, GstData.app_sink, NULL);

    if(rj_link_source_and_convert(GstData.app_source,GstData.video_convert) != TRUE)

    {
        g_printerr ("GstData.app_source could not link GstData.video_convert\n");
        gst_object_unref (GstData.pipeline);
        fclose(fp);
        exit(-1);
    }

    if( rj_link_convert_and_encode(GstData.video_convert, GstData.h264_encoder) != TRUE)
    {
        g_printerr ("GstData.video_convert could not link GstData.h264_encoder\n");
        gst_object_unref (GstData.pipeline);
        fclose(fp);
        exit(-1);
    }

    if(gst_element_link_filtered(GstData.h264_encoder, GstData.app_sink, caps_app_sink) != TRUE)
    {
        g_printerr ("GstData.h264_encoder could not link GstData.file_sink\n");
        gst_object_unref (GstData.pipeline);
        fclose(fp);
        exit(-1);
    }
    gst_caps_unref (caps_app_sink);
}

int main(int argc, char *argv[])
{
    
 
    init_parameters (&argc, argv);
    init_gst_vaecode();

    /* start gstpipe run*/
    GstData.app_sink_index = 0;
    gst_element_set_state (GstData.pipeline, GST_STATE_PLAYING);
    g_print ("start GST_STATE_PLAYING\n");

    /* start g_main_loop_run*/
    GstData.loop = g_main_loop_new(NULL, FALSE);    // Create gstreamer loop
    g_main_loop_run(GstData.loop);                  // Loop will run until receiving EOS (end-of-stream), will block here
    fprintf(stderr, "g_main_loop_run returned, stopping record\n");
    
    // Free resources
    gst_element_set_state (GstData.pipeline, GST_STATE_NULL);       // Stop pipeline to be released
    fprintf(stderr, "Deleting pipeline\n");
    gst_object_unref (GstData.pipeline);                            // THis will also delete all pipeline elements
    g_source_remove(bus_watch_id);
    g_main_loop_unref(GstData.loop);
    fclose(fp);
    return 0;
}
 
static void eos_on_appsink (GstElement *sink, GstDataStruct *pGstData)
{
    printf("appsink get signal eos !!!\n");
}
 
static void new_h264_sample_on_appsink (GstElement *sink, GstDataStruct *pGstData)
{
    GstSample *sample = NULL;
    g_signal_emit_by_name (sink, "pull-sample", &sample);
    if(sample)
    {
        pGstData->app_sink_index++;
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo info;
        if(gst_buffer_map((buffer), &info, GST_MAP_READ))
        {
          g_print ("h264 Streaming Buffer is Comming\n");
          // Here to get h264 buffer data with info.data and get h264 buffer size with info.size
          //gst_util_dump_mem (info.data, info.size);
          fwrite(info.data, info.size, 1, fp);
          g_print ("h264 Streaming Buffer is wrote, len:%d, index:%d\n", (int)info.size, pGstData->app_sink_index);
          gst_buffer_unmap(buffer, &info);
          gst_sample_unref (sample);
        }
    }
}



 
// Bus messages processing, similar to all gstreamer examples
gboolean bus_msg_call(GstBus *bus, GstMessage *msg, GstDataStruct *pGstData)
{
    gchar *debug;
    GError *error;
    GMainLoop *loop = pGstData->loop;
    GST_DEBUG ("got message %s",gst_message_type_get_name (GST_MESSAGE_TYPE (msg)));
    switch (GST_MESSAGE_TYPE(msg))
    {
        case GST_MESSAGE_EOS:
            fprintf(stderr, "End of stream\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_ERROR:
            gst_message_parse_error(msg, &error, &debug);
            g_free(debug);
            g_printerr("Error: %s\n", error->message);
            g_error_free(error);
            g_main_loop_quit(loop);
            break;
        default:
            break;
    }
    return TRUE;
}


/* use to link vaapipostproc and vaapih264enc element*/
static gboolean
rj_link_convert_and_encode (GstElement *convert_element, GstElement *encode_element)
{
  gboolean link_ok;
  GstCaps *caps;
  
  caps = gst_caps_new_simple ("video/x-raw",
        "format", G_TYPE_STRING,"NV12",
        "width", G_TYPE_INT, screen_width,
        "height", G_TYPE_INT, screen_height,
        "framerate", GST_TYPE_FRACTION, frame_rate, 1,
        NULL);
  link_ok = gst_element_link_filtered (convert_element, encode_element, caps);
  gst_caps_unref (caps);

  if (!link_ok) {
    g_warning ("Failed to link convert_element and encode_element!");
  }
  return link_ok;
}


/* the x11 init function*/
static int xDispalyInit()
{
    dsp = XOpenDisplay(":0");/* Connect to a local display */
    if (NULL==dsp) {
        //sprintf(err_str,"Error:Cannot connect to local display\n");
        printf("Error:Cannot connect to local display\n");
        return -1;
    }
    desktop = RootWindow(dsp,0);/* Refer to the root window */
    if (0==desktop) {
        printf("cannot get root window\n");
        return -1;
    }
    
    /* Retrive the width and the height of the screen */
    screen_width = DisplayWidth(dsp,0);
    screen_height = DisplayHeight(dsp,0);
    //frame_bitrate = screen_width * screen_height * 12 * frame_rate / 50;
    frame_bitrate = 0;//0 means auto-calculate by vaapih264enc
    printf("frame_bitrate=%d (0 means auto-calculate by vaapih264enc) \n",frame_bitrate);
    return 0;
}

