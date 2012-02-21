#include <stdbool.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <TH.h>
#include <luaT.h>

#define torch_(NAME) TH_CONCAT_3(torch_, Real, NAME)
#define torch_string_(NAME) TH_CONCAT_STRING_3(torch., Real, NAME)
#define Lffmpeg_(NAME) TH_CONCAT_3(Lffmpeg_, Real, NAME)

#define FFMPEG_LIBRARY "ffmpeg_lib"
#define FFMPEG_CONTEXT "ffmpeg_ctx"

/*
 * basic structure to represent a video and the frames */
typedef struct {
  const char      *filename;
  AVFormatContext *pFormatCtx;
  int             videoStream;
  AVCodecContext  *pCodecCtx;
  AVCodec         *pCodec;
  AVFrame         *pFrame; 
  AVFrame         *pFrameRGB;
  AVPacket        *packet;
  struct SwsContext *img_convert_ctx;
  int             frameFinished;
  int             numBytes;
  int             dstW;
  int             dstH; 
  uint8_t         *buffer;
} ffmpeg_ctx;

void ffmpeg_ctx_print(ffmpeg_ctx *v) {
  /*
   * dump_format is a rather complicated function, which calls many
   * sub functions for the different modules (for codec, container,
   * video and audio etc.).  Unfortunately all the subfunctions use
   * printf() so it is difficult to get a string for toString()
   * without redirecting the output somehow. */
  av_dump_format(v->pFormatCtx, 0, v->filename, false);   
}

static int Lffmpeg_ctx_open (lua_State *L) {
  /* struct which holds all data about a video stream */
  ffmpeg_ctx *v = (ffmpeg_ctx *)lua_newuserdata(L, sizeof(ffmpeg_ctx));
  
  if (luaL_checkstring(L, 1)) {
    v->filename = luaL_checkstring(L,1);
  } else {
     THError("<ffmeg.vid> must pass file name to open video stream"); 
  }
  /*
   * Initialize the destination sizes for the software re-scaling
   * called in get_frame */
  v->dstW = 0;
  v->dstH = 0;
  if (lua_isnumber(L, 2)) v->dstW = lua_tonumber(L, 2);
  if (lua_isnumber(L, 3)) v->dstH = lua_tonumber(L, 3);

  printf("Opened file to (%d,%d)\n", v->dstW, v->dstH);
  
   int i = 0;
   /* Open video file */

   v->pFormatCtx = avformat_alloc_context();
   if(avformat_open_input(&(v->pFormatCtx), v->filename, NULL, NULL)!=0)
     THError("<ffmeg.vid> Couldn't open file"); 
   /* Retrieve stream information */
   if(avformat_find_stream_info(v->pFormatCtx,NULL)<0)
     THError("<ffmeg.vid> Couldn't find stream information");
   /* Print info about the stream */
   ffmpeg_ctx_print(v);
   /* Find the first video stream */
   /* FIXME should also be able to request a specific channel for stereo vids */
   v->videoStream=-1;
   for(i=0; i<v->pFormatCtx->nb_streams; i++)
     if(v->pFormatCtx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO)
     {
        v->videoStream=i;
        break;
     }
     
   if(v->videoStream==-1)
     THError("<ffmeg.vid> Didn't find a video stream");
   /* Get a pointer to the codec context for the video stream */
   v->pCodecCtx=v->pFormatCtx->streams[v->videoStream]->codec;
   /* Find the decoder for the video stream */
   v->pCodec=avcodec_find_decoder(v->pCodecCtx->codec_id);
   if(v->pCodec==NULL)
     THError("<ffmeg.vid> Codec not found");
   /* Open codec */
   if(avcodec_open2(v->pCodecCtx, v->pCodec, NULL)<0)
     THError("<ffmeg.vid> Could not open codec"); 
   /*
    * Hack to correct wrong frame rates that seem to be generated by
    *  some codecs  */
    if(v->pCodecCtx->time_base.num>1000 &&
       v->pCodecCtx->time_base.den==1)
      v->pCodecCtx->time_base.den=1000;
    /* Allocate video frame */
    v->pFrame=avcodec_alloc_frame();
    /* Allocate an AVFrame structure */
    v->pFrameRGB=avcodec_alloc_frame();
    if(v->pFrameRGB==NULL)
      THError("<ffmeg.vid> could not allocate AVFrame structure");
    /* Determine required buffer size */
    v->numBytes=avpicture_get_size(PIX_FMT_RGB24, v->pCodecCtx->width,
        v->pCodecCtx->height);
    /* Allocate buffer */
    v->buffer=malloc(v->numBytes); 
    /* Assign appropriate parts of buffer to image planes in pFrameRGB */
    avpicture_fill((AVPicture *)v->pFrameRGB, v->buffer, PIX_FMT_RGB24,
        v->pCodecCtx->width, v->pCodecCtx->height);

    luaL_getmetatable(L, FFMPEG_CONTEXT);
    lua_setmetatable(L, -2);
    return 1;
}

/*
 * Clean up.  Free all the parts of the ffmpeg_ctx*/
void ffmpeg_ctx_free (ffmpeg_ctx *v){
  // Free the RGB image
  if(v->buffer) {  
    free(v->buffer);
    printf("free buffer\n");
  }

  if(v->pFrameRGB){
    av_free(v->pFrameRGB);
    printf("free pFrameRGB\n");
  }

  // Free the YUV frame
  if(v->pFrame){
    av_free(v->pFrame);
    printf("free pFrame\n");
  }

  // Close the Software Scaler
  if (v->img_convert_ctx){
    sws_freeContext(v->img_convert_ctx);
    printf("free swsContext\n");
  }

  // Close the codec
  if(v->pCodecCtx){
    avcodec_close(v->pCodecCtx);
    printf("free codec\n");
  }

    // Close the video file
  if(v->pFormatCtx){
    av_close_input_file(v->pFormatCtx);
    printf("free Format\n");
  }
  if(v->packet){
    av_free_packet(v->packet);
    printf("free packet\n");
  }
}

ffmpeg_ctx *Lffmpeg_ctx_check(lua_State *L, int pos){
   ffmpeg_ctx *v = (ffmpeg_ctx *)luaL_checkudata(L, pos, FFMPEG_CONTEXT);
   if (v == NULL) THError("<ffmpeg_ctx> requires a valid ffmpeg_ctx");
   return v;
}


static int Lffmpeg_ctx_rawWidth(lua_State *L) {
  ffmpeg_ctx *v = (ffmpeg_ctx *)luaL_checkudata(L, 1, FFMPEG_CONTEXT);
  int w = v->pCodecCtx->width; 
  lua_pushnumber(L,w);
  return 1;
}

static int Lffmpeg_ctx_rawHeight (lua_State *L) {
  ffmpeg_ctx *v = (ffmpeg_ctx *)luaL_checkudata(L, 1, FFMPEG_CONTEXT);
  int h = v->pCodecCtx->height; 
  lua_pushnumber(L,h);
  return 1;
}

static int Lffmpeg_ctx_dstWidth (lua_State *L) {
  ffmpeg_ctx *v = (ffmpeg_ctx *)luaL_checkudata(L, 1, FFMPEG_CONTEXT);
  int w = v->dstW; 
  lua_pushnumber(L,w);
  return 1;
}

static int Lffmpeg_ctx_dstHeight (lua_State *L) {
  ffmpeg_ctx *v = (ffmpeg_ctx *)luaL_checkudata(L, 1, FFMPEG_CONTEXT);
  int h = v->dstH; 
  lua_pushnumber(L,h);
  return 1;
}

static int Lffmpeg_ctx_filename (lua_State *L) {
  ffmpeg_ctx *v = (ffmpeg_ctx *)luaL_checkudata(L, 1, FFMPEG_CONTEXT);
  const char * s = v->filename; 
  lua_pushstring(L,s);
  return 1;
}

/*
 * Lffmpeg_ctx cleanup function */
static int Lffmpeg_ctx_close(lua_State *L) {
  ffmpeg_ctx *v = Lffmpeg_ctx_check(L, 1);
  ffmpeg_ctx_free(v);
  return 0;
}

static int Lffmpeg_init(lua_State *L){
  /* register all codecs, demux and protocols */
    avcodec_register_all();
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
#if CONFIG_AVFILTER
    avfilter_register_all();
#endif
    av_register_all();
    //    avformat_network_init();
    return 0;
}

/* static int Lffmpeg_ctx_seek (lua_State *L) { */
/*   /\* struct which holds all data about a video stream *\/ */
/*   ffmpeg_ctx *v = (ffmpeg_ctx *)luaL_checkudata(L, 1, FFMPEG_CONTEXT); */
/*   /\* pass timestamp string in HH:MM:SS:sss format *\/ */
/*   const char * timestr = luaL_checkstring(L,2); */
/*   /\* holds timestamp in microseconds *\/ */
/*   int64_t target_timestamp_us;  */
/*   int ret = av_parse_time(&target_timestamp_us, timestr, 0); */
/*   if (ret < 0){ */
/*     printf("WARNING error parsing timestamp for seek()\n"); */
/*   } else { */
/*     printf("%s: seeking to position %0.3f\n", */
/*            v->filename, (double)target_timestamp_us / AV_TIME_BASE); */
/*     ret = avformat_seek_file(v->pFormatCtx, */
/*                              v->videoStream, */
/*                              INT64_MIN, */
/*                              target_timestamp_us, */
/*                              INT64_MAX, */
/*                              0); */
  
/*     if (ret < 0) { */
/*       fprintf(stderr, "%s: could not seek to position %0.3f\n", */
/*               v->filename, (double)target_timestamp_us / AV_TIME_BASE); */
/*     } */
/*   } */
/*   lua_pushnumber(L,ret); */
/*   return 1; */
/* } */

static const struct luaL_reg ffmpeg_ctx_methods [] = {
  {"__gc",       Lffmpeg_ctx_close},
  {"close",      Lffmpeg_ctx_close},
  {"open",       Lffmpeg_ctx_open},
  //{"seek",       Lffmpeg_ctx_seek},
  {"rawWidth",   Lffmpeg_ctx_rawWidth},
  {"rawHeight",  Lffmpeg_ctx_rawHeight}, 
  {"dstWidth",   Lffmpeg_ctx_dstWidth},
  {"dstHeight",  Lffmpeg_ctx_dstHeight},
  {"filename",   Lffmpeg_ctx_filename},
  {NULL, NULL}  /* sentinel */
};

static const struct luaL_reg ffmpeg_lib [] = {
  {"init",       Lffmpeg_init},
  {NULL, NULL} /* sentinel */
};


static const void* torch_FloatTensor_id = NULL;
static const void* torch_DoubleTensor_id = NULL;

#include "generic/ffmpeg.c"
#include "THGenerateFloatTypes.h"

DLL_EXPORT int luaopen_libffmpeglib(lua_State *L)
{
  /* create ffmpeg_ctx metatable */
  luaL_newmetatable(L, FFMPEG_CONTEXT); 
  lua_createtable(L, 0, sizeof(ffmpeg_ctx_methods) / sizeof(luaL_reg) - 1); 
   luaL_register(L,"ffmpeg",ffmpeg_ctx_methods);
   /* lua_setfield(L, -2 , "__index"); */

   luaL_register(L, "ffmpeglib", ffmpeg_lib);
   
   /* load the functions which copy frames to Tensors */
  torch_FloatTensor_id = luaT_checktypename2id(L, "torch.FloatTensor");
  torch_DoubleTensor_id = luaT_checktypename2id(L, "torch.DoubleTensor");

  Lffmpeg_FloatInit(L);
  Lffmpeg_DoubleInit(L);

  luaL_register(L, "ffmpeg.double", Lffmpeg_DoubleMethods); 
  luaL_register(L, "ffmpeg.float", Lffmpeg_FloatMethods);

  return 1;
}
