#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <stdbool.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

void data_callback(ma_device *device, void *pOutput, const void *pInput,
                   ma_uint32 frameCount) {

  AVAudioFifo *fifo = (AVAudioFifo *)device->pUserData;
  //printf("Fifo Size; %d\n", av_audio_fifo_size(fifo));
  av_audio_fifo_read(fifo, &pOutput, frameCount);

  (void)pInput;
}

#define MAX_AUDIO_FRAME_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFIL_THRESH 4096

static void decode(AVCodecContext *context, AVPacket *packet, AVFrame *frame,
                   AVAudioFifo *fifo) {
  int ret, data_size;
  SwrContext *resampler = NULL;
  ret = swr_alloc_set_opts2(&resampler, &context->ch_layout, AV_SAMPLE_FMT_FLT,
                                 44100, &context->ch_layout, AV_SAMPLE_FMT_FLT,
                                 44100, 0, NULL);
  if (ret != 0) {
    fprintf(stderr, "could not allocate and set resampler\n\n");
    return;
  }

  ret = avcodec_send_packet(context, packet);
  if (ret < 0) {
    fprintf(stderr, "cannot submit packet to the decoder\n\n");
    return;
  }

  while (ret >= 0) {
    ret = avcodec_receive_frame(context, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return;
    } else if (ret < 0) {
      fprintf(stderr, "error during decoding");
      exit(1);
    }
    data_size = av_get_bytes_per_sample(context->sample_fmt);
    //printf("%i bytes per sample. ", data_size);
    if (data_size < 0) {
      fprintf(stderr, "could not calculate data size");
      exit(1);
    }

    AVFrame *resampled = av_frame_alloc();
    resampled->sample_rate = frame->sample_rate;
    resampled->ch_layout = frame->ch_layout;
    resampled->format = AV_SAMPLE_FMT_FLT;

    ret = swr_convert_frame(resampler, resampled, frame);

    int num_samples = av_audio_fifo_write(fifo, (void **)resampled->data, frame->nb_samples);
    printf("Frame contained %i samples and we inserted %i samples into fifo\n", frame->nb_samples, num_samples);
    av_frame_unref(frame);
  }
}

int main() {
  const AVCodec *codec = NULL;
  AVCodecContext *context = NULL;
  int len = 0;
  FILE *f = NULL;
  uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
  AVPacket *packet;
  AVFrame *decoded_frame = NULL;
  AVCodecParserContext *parser = NULL;
  int ret;

  codec = avcodec_find_decoder(AV_CODEC_ID_MP3);
  if (!codec) {
    fprintf(stderr, "could not find codec");
    return -1;
  }

  parser = av_parser_init(codec->id);
  if (!parser) {
    fprintf(stderr, "could not initialize parser");
    return -1;
  }

  context = avcodec_alloc_context3(codec);

  if (avcodec_open2(context, codec, NULL) < 0) {
    fprintf(stderr, "could not open codec");
    return -1;
  }
  char *filename = "output.mp3";
  f = fopen(filename, "rb");
  if (f == NULL) {
    fprintf(stderr, "could not open MP3 file %s", filename);
    return -1;
  }

  AVAudioFifo *fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, 1);
  if (!fifo) {
    fprintf(stderr, "could not allocate fifo");
    return -1;
  }

  packet = av_packet_alloc();

  uint8_t *data = inbuf;
  size_t data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, f);
  while (data_size > 0) {
    if (!decoded_frame) {
      if (!(decoded_frame = av_frame_alloc())) {
        fprintf(stderr, "could not allocate audio frame");
        return -1;
      }
    }

    ret = av_parser_parse2(parser, context, &packet->data, &packet->size, data,
                           data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
    if (ret < 0) {
      fprintf(stderr, "error while parsing");
      return -1;
    }
    data += ret;
    data_size -= ret;

    if (packet->size) {
      decode(context, packet, decoded_frame, fifo);
    }

    if (data_size < AUDIO_REFIL_THRESH) {
      memmove(inbuf, data, data_size);
      data = inbuf;
      len = fread(data + data_size, 1, AUDIO_INBUF_SIZE - data_size, f);
      if (len < 0) {
        data_size += len;
      }
    }
  }

  packet->data = NULL;
  packet->size = 0;
  decode(context, packet, decoded_frame, fifo);

  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 2;
  config.sampleRate = 44100;
  config.dataCallback = data_callback;
  config.pUserData = (void *)fifo;

  ma_device device;
  if (ma_device_init(NULL, &config, &device) != MA_SUCCESS) {
    fprintf(stderr, "could not initialize output device");
    return -1;
  }

  ma_device_start(&device);

  getchar();

  fclose(f);
  avcodec_close(context);
  av_free(context);
  av_frame_free(&decoded_frame);
  av_audio_fifo_free(fifo);

  ma_device_uninit(&device);
  return 0;
}
