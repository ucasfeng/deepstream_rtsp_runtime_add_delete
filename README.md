# deepstream_rtsp_runtime_add_delete
add, delete and auto reconnect rtsp during deepstream pipeline runtime, and it's only a demo.

### usage

1. modify Makefile according to your jetson/dgpu cuda version.

2. modify BATCH_SIZE（default=4） and preset "rtsps" array main.c.

3. make and run app

4. using below keyboard commands to add/delete rtsp source manually.

   ```
   Runtime Commands:
   	q: Quit
   	p: Print debug information
   	d: Dump pipeline dot
   	f: restart pipeline
   	r: Remove one rtsp source
   	a: Add one rtsp source
   ```

   

5. if you want to test auto reconnecting,  unplug your rtsp camera, wait for a while and reconnect it.

### features

1. add a rtsp source in runtime by command
2. delete a rtsp source in runtime by command
3. auto reconnecting after EOS/network error.

### Pipeline

rtspsrc ->  nvstreammux -> nvinfer ->  nvtiler -> nvvideoconvert -> nvdsosd -> displaysink

### reference

[deepstream_reference_apps/README.md at master · NVIDIA-AI-IOT/deepstream_reference_apps (github.com)](https://github.com/NVIDIA-AI-IOT/deepstream_reference_apps/blob/master/runtime_source_add_delete/README.md)
