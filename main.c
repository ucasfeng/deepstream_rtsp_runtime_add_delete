#include <stdio.h>
#include <glib.h>
#include <gst/gst.h>
#include <cuda_runtime_api.h>
#include <math.h>
#include <unistd.h>
#include <sys/time.h>
#include <termios.h>

#define BATCH_SIZE 4
#define PIPELINE_WIDTH 1920
#define PIPELINE_HEIGHT 1080


gchar *rtsps[BATCH_SIZE] = {
  "rtsp://user:pwd@192.168.3.18:554/h264/ch1/main/av_stream",
  "rtsp://user:pwd@192.168.3.19:554/h264/ch1/main/av_stream",
  "rtsp://user:pwd@192.168.3.20:554/h264/ch1/main/av_stream",
  "rtsp://user:pwd@192.168.3.21:554/h264/ch1/main/av_stream",
};

typedef struct {
  GstElement *bin;
  GstElement *src_elem;
  GstElement *depay;
  GstElement *parser;
  GstElement *queue1;
  GstElement *decodebin;
  GstElement *queue2;
  GstElement *nvvidconv;
  GstElement *capsfilter;
}SourceBin;

GMainLoop *loop = NULL;
GstElement *pipeline = NULL,
           *streammux = NULL,
           *nvinfer = NULL,
           *nvvidconv = NULL,
           *nvosd = NULL,
           *tiler = NULL,
           *nvtransform = NULL,
           *sink = NULL;

SourceBin *sources[BATCH_SIZE];


void print_runtime_commands(){
  g_print("\nRuntime Commands:\n"
  "\tq: Quit\n"
  "\tp: Print debug information\n"
  "\td: Dump pipeline dot\n"
  "\tf: restart pipeline\n"
  "\tr: Remove one rtsp source\n"
  "\ta: Add one rtsp source\n"
  );
}
static gboolean
kbhit (void)
{
  struct timeval tv;
  fd_set rdfs;

  tv.tv_sec = 0;
  tv.tv_usec = 0;

  FD_ZERO (&rdfs);
  FD_SET (STDIN_FILENO, &rdfs);

  select (STDIN_FILENO + 1, &rdfs, NULL, NULL, &tv);
  return FD_ISSET (STDIN_FILENO, &rdfs);
}
void changemode (int dir){
  static struct termios oldt, newt;

  if (dir == 1) {
    tcgetattr (STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON);
    tcsetattr (STDIN_FILENO, TCSANOW, &newt);
  } else
    tcsetattr (STDIN_FILENO, TCSANOW, &oldt);
}

void print_debug_info(){
  // pipeline state
  GstState cur, pending;
  GstStateChangeReturn ret;
  ret = gst_element_get_state(pipeline, &cur, &pending, GST_SECOND*5);
  g_print("pipeline state cur:%s pending:%s sret:%s\n", gst_element_state_get_name(cur),
                                                        gst_element_state_get_name(pending),
                                                        gst_element_state_change_return_get_name(ret));
}

gboolean select_stream_cb(GstElement *rtspsrc, guint num, GstCaps *caps, gpointer user_data){
  g_print("select_stream_cb\n");
  GstStructure *str = gst_caps_get_structure(caps, 0);
  const gchar *media = gst_structure_get_string (str, "media");
  const gchar *encoding_name = gst_structure_get_string (str, "encoding-name");
  SourceBin *bin = (SourceBin*)user_data;
  gboolean is_video = (!g_strcmp0 (media, "video"));
  if(!is_video) return FALSE;
  if(!bin->depay){
    if(!g_strcmp0(encoding_name, "H264")){
      bin->depay = gst_element_factory_make ("rtph264depay", 0);
      bin->parser = gst_element_factory_make ("h264parse", 0);
    }else if(!g_strcmp0(encoding_name, "H265")){
      bin->depay = gst_element_factory_make ("rtph265depay", 0);
      bin->parser = gst_element_factory_make ("h265parse", 0); 
    }else{
      g_print("unable create depay and parser for %s\n", encoding_name);
      return FALSE;
    }
    gst_bin_add_many(GST_BIN(bin->bin), bin->depay, bin->parser, NULL);
    gst_element_link_many(bin->depay, bin->parser, bin->queue1, NULL);
    gst_element_sync_state_with_parent(bin->depay);
    gst_element_sync_state_with_parent(bin->parser);
  }
  return TRUE;
}

void rtspsrc_pad_added_cb(GstElement *rtspsrc, GstPad *pad, gpointer user_data){
  g_print("rtspsrc_pad_added_cb\n");
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);
  if (g_strrstr (name, "x-rtp")) {
    SourceBin *bin = (SourceBin *) user_data;
    GstPad *sinkpad = gst_element_get_static_pad (bin->depay, "sink");
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {
      g_print ("Failed to link depay loader to rtsp src\n");
    }
    gst_object_unref (sinkpad);
  }
  gst_caps_unref (caps);
}

void decodebin_pad_added_cb(GstElement *decodebin, GstPad *pad, gpointer user_data){
  g_print("decodebin_pad_added_cb\n");
  GstCaps *caps = gst_pad_query_caps (pad, NULL);
  const GstStructure *str = gst_caps_get_structure (caps, 0);
  const gchar *name = gst_structure_get_name (str);

  if (!strncmp (name, "video", 5)) {
    SourceBin *bin = (SourceBin *) user_data;
    GstPad *sinkpad = gst_element_get_static_pad (bin->queue2, "sink");
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {
      g_print ("Failed to link decodebin to pipeline\n");
    }
    gst_object_unref (sinkpad);
  }
  gst_caps_unref (caps);
}

GstPadProbeReturn eos_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer u_data){
  SourceBin *bin = (SourceBin *)u_data;
  if(info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM){
    if(GST_EVENT_TYPE(info->data)==GST_EVENT_EOS){
      g_print("receive eos and drop it..\n");
      GstMessage *msg = gst_message_new_request_state(GST_OBJECT(bin->bin), GST_STATE_PLAYING);
      GstBus *bus = gst_element_get_bus(bin->bin);
      gst_bus_post(bus, msg);
      return GST_PAD_PROBE_DROP;
    }
  }
  return GST_PAD_PROBE_OK;
}

void add_rtsp(){
  gint sid = -1;
  for(guint i=0;i<BATCH_SIZE;i++){
    if(!sources[i]){
      sid = i;
      break;
    }
  }

  if(sid>=0){
    g_print("adding rtsp source to sid:%d\n", sid);
    SourceBin *bin = (SourceBin*)g_malloc0(sizeof(SourceBin));
    gchar *name = g_strdup_printf("source%d", sid);
    bin->bin = gst_bin_new(name);
    bin->src_elem = gst_element_factory_make("rtspsrc", 0);
    bin->queue1 = gst_element_factory_make("queue", 0);
    bin->decodebin = gst_element_factory_make("decodebin", 0);
    bin->queue2 = gst_element_factory_make("queue", 0);
    bin->nvvidconv = gst_element_factory_make("nvvideoconvert", 0);
    bin->capsfilter = gst_element_factory_make("capsfilter", 0);

    // rtspsrc
    g_signal_connect(G_OBJECT(bin->src_elem), "select-stream", G_CALLBACK(select_stream_cb), bin);
    g_signal_connect(G_OBJECT(bin->src_elem), "pad-added", G_CALLBACK(rtspsrc_pad_added_cb), bin);
    g_object_set(G_OBJECT(bin->src_elem), "location", rtsps[sid], 
                                          "drop-on-latency", TRUE, 
                                          "latency", 200, NULL); 

    // decodebin
    g_signal_connect(G_OBJECT(bin->decodebin), "pad-added", G_CALLBACK(decodebin_pad_added_cb), bin);
    

    GstCaps *caps = gst_caps_new_empty_simple("video/x-raw");
    GstCapsFeatures *feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(caps, 0, feature);
    g_object_set(G_OBJECT(bin->capsfilter), "caps", caps, NULL);
    gst_caps_unref(caps);
 
    if(!bin->bin || !bin->src_elem || !bin->queue1 || !bin->decodebin || !bin->queue2
      || !bin->nvvidconv || !bin->capsfilter){
      g_print("faild to create source, some element create failed..\n");
      return;
    }
    
    gst_bin_add_many(GST_BIN(bin->bin), bin->src_elem,
                                        bin->queue1,
                                        bin->decodebin,
                                        bin->queue2,
                                        bin->nvvidconv,
                                        bin->capsfilter, NULL);
    gst_element_link(bin->queue1, bin->decodebin);
    gst_element_link_many(bin->queue2, bin->nvvidconv, bin->capsfilter, NULL);
    GstPad *src_pad = gst_element_get_static_pad(bin->capsfilter, "src");
    gst_element_add_pad(bin->bin, gst_ghost_pad_new("src", src_pad));
    gst_object_unref(src_pad);

    // link to streammux..
    gst_bin_add(GST_BIN(pipeline), bin->bin);
    gchar pad_name[15];
    g_snprintf(pad_name, 16, "sink_%u", sid);
    GstPad *mux_sink_pad = gst_element_get_request_pad(streammux, pad_name);
    GstPad *bin_src_pad = gst_element_get_static_pad(bin->bin, "src");
    if(gst_pad_link(bin_src_pad, mux_sink_pad) != GST_PAD_LINK_OK){
      g_print("failed to link rtsp bin to streammux\n");  
      return;
    }
    gst_object_unref(mux_sink_pad);
    gst_object_unref(bin_src_pad);

    // eos block probe..
    GstPad *queue1_sink_pad = gst_element_get_static_pad(bin->queue1, "sink"); 
    gst_pad_add_probe(queue1_sink_pad, 
                      (GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                      eos_probe_cb, bin, NULL);
    gst_object_unref(queue1_sink_pad); 
    gst_element_sync_state_with_parent(bin->bin);
    sources[sid] = bin;
    g_free(name);
  }else{
    g_print("failed to add source, too much sources yet...\n");
  }
}

void remove_rtsp(){
  gint sid = -1;
  for(guint i=BATCH_SIZE-1;i>=0;i--){
    if(sources[i]){
      sid = i;
      break;
    }
  }

  if(sid==-1){
    g_print("no rtsp source\n");
    return;
  }

  gst_element_set_state(sources[sid]->bin, GST_STATE_NULL); 
  GstPad *src_pad = gst_element_get_static_pad(sources[sid]->bin, "src");
  GstPad *sink_pad = gst_pad_get_peer(src_pad);
  gst_pad_send_event(sink_pad, gst_event_new_flush_stop(FALSE));
  gst_pad_unlink(src_pad, sink_pad);
  gst_element_release_request_pad(streammux, sink_pad);
  gst_bin_remove(GST_BIN(pipeline), sources[sid]->bin);

  /*
      reset pipeline to PLAYING
  */
  GstMessage *req = gst_message_new_request_state(GST_OBJECT(streammux), GST_STATE_PLAYING);
  GstBus *bus = gst_element_get_bus(streammux);
  gst_bus_post(bus, req);

done:
  if(sink_pad) gst_object_unref(sink_pad);
  if(src_pad) gst_object_unref(src_pad);
  g_free(sources[sid]);
  sources[sid] = NULL;
  g_print("\nremoved source:%d\n", sid);
}

gboolean command_thread_cb(){
  if(!kbhit()){
    return TRUE;
  } 

  gchar c = fgetc(stdin);
  switch(c){
    case 'q':
      g_print("\nforce quit demo...\n");
      g_main_loop_quit(loop);
      break; 
    case 'p':
      g_print("\ngetting debug info...\n");
      print_debug_info();
      break;
    case 'd':
      g_print("\ndumping pipeline's dot ...\n");
      GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "debug-pipeline"); 
      g_print("pipeline dot dumped successfully...\n");
      break;
    case 'f':
      g_print("\nRestarting pipelint\n");
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
      break;
    case 'r':
      remove_rtsp();
      break;
    case 'a':
      add_rtsp();
      break;
    default:
      break;
  }
  return TRUE;
}

gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data){
  GstElement *src_elem = (GstElement*) GST_MESSAGE_SRC(msg);
  switch(GST_MESSAGE_TYPE(msg)){
    case GST_MESSAGE_REQUEST_STATE:
    {
      for(guint i=0;i<BATCH_SIZE;i++){
        if(sources[i] && sources[i]->bin == src_elem){
          gst_element_set_state(sources[i]->bin, GST_STATE_NULL);
          gst_element_set_state(sources[i]->bin, GST_STATE_PLAYING);
          g_print("reset source:%d PLAYING->NULL->PLAYING\n",i);
        }
      }
      gst_element_set_state(pipeline, GST_STATE_PLAYING);
      g_print("recover pipeline to playing..\n");
      break;
    }
    case GST_MESSAGE_ERROR:
    {
      gchar *debug = NULL;
      GError *error = NULL;
      gst_message_parse_error (msg, &error, &debug);
      g_print("Error from %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
      if(debug){
        g_print("debug info: %s\n", debug);
      }

      if(!g_strcmp0(error->message, "Unhandled error")
         || !g_strcmp0(error->message, "Could not write to resource.")){
        ;
        // discard error.
      }

      if(!g_strcmp0(error->message, "Could not open resource for reading and writing.")){
        // find source and reset it.
        for(guint i=0;i<BATCH_SIZE;i++){
          if(sources[i] && sources[i]->src_elem == src_elem){
            gst_element_set_state(sources[i]->bin, GST_STATE_NULL);
            gst_element_set_state(sources[i]->bin, GST_STATE_PLAYING);
            g_print("restart source:%d\n", i);
            break;
          }
        }
      }

      g_free(debug);
      g_error_free(error);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

int main(){
  guint bus_id;
  GstBus *bus = NULL;
  GstStateChangeReturn sret;

  int current_device = -1;
  cudaGetDevice(&current_device);
  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, current_device);

  gst_init(0,0);
  loop = g_main_loop_new(0, FALSE);

  /*setup pipeline*/
  pipeline = gst_pipeline_new("test-pipeline");
  // elements.
  streammux = gst_element_factory_make("nvstreammux", 0);
  nvinfer = gst_element_factory_make("nvinfer", 0);
  tiler = gst_element_factory_make("nvmultistreamtiler", 0);
  nvvidconv = gst_element_factory_make("nvvideoconvert", 0);
  if(prop.integrated){
    nvtransform = gst_element_factory_make("nvegltransform", 0);
  }
  nvosd = gst_element_factory_make ("nvdsosd", 0);
  
  sink = gst_element_factory_make ("nveglglessink", 0);
  // properties
  g_object_set(G_OBJECT(streammux), "batched-push-timeout", 25000,
                                    "batch-size", BATCH_SIZE,
                                    "gpu-id", 0,
                                    "live-source", 1, 
                                    "width", PIPELINE_WIDTH,
                                    "height", PIPELINE_HEIGHT,NULL);
  g_object_set(G_OBJECT(nvinfer), "config-file-path", "nvinfer.txt", NULL);
  g_object_set(G_OBJECT(nvinfer), "batch-size", BATCH_SIZE,
                                  "gpu-id",0, NULL);
  guint rows = (guint)sqrt(BATCH_SIZE);
  guint columns = (guint)ceil(1.0 * BATCH_SIZE / rows);
  g_object_set(G_OBJECT(tiler), "rows", rows,
                                "columns", columns,
                                "width", PIPELINE_WIDTH,
                                "height", PIPELINE_HEIGHT,
                                "gpu-id", 0, NULL); 
  g_object_set(G_OBJECT(nvvidconv), "gpu-id", 0, NULL);
  g_object_set(G_OBJECT(nvosd), "gpu-id", 0, NULL);
  if(!prop.integrated){
    g_object_set(G_OBJECT(sink), "gpu-id", 0, NULL);
  }
  g_object_set(G_OBJECT(sink), "async", FALSE, 
                                "sync", FALSE,
                                "window-width", 1280,
                                "window-height", 720,NULL);
  // link
  if(prop.integrated){
    gst_bin_add_many(GST_BIN(pipeline), streammux, nvinfer, tiler, nvosd, nvvidconv, nvtransform, sink, NULL);
    gst_element_link_many(streammux, nvinfer, tiler, nvvidconv, nvosd, nvtransform, sink, NULL);
  }else{
    gst_bin_add_many(GST_BIN(pipeline), streammux, nvinfer, tiler, nvosd, nvvidconv, sink, NULL);
    gst_element_link_many(streammux, nvinfer, tiler, nvvidconv, nvosd, sink, NULL);
  }

  bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
  bus_id = gst_bus_add_watch(bus, bus_call, sources);
  gst_object_unref(bus);

  // set state to playing
  sret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
  g_print("set pipeline to playing, sret=%s", gst_element_state_change_return_get_name(sret));

  for(guint i=0;i<BATCH_SIZE;i++){
    sources[i] = NULL;
  }

  print_runtime_commands();
  changemode(1);
  g_timeout_add(10, command_thread_cb, NULL);
  g_main_loop_run(loop);
  changemode(0);

  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  g_source_remove(bus_id);
  g_main_loop_unref(loop);
  return 1;
}
