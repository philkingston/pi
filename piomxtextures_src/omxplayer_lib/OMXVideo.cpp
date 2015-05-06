/*
 *      Copyright (C) 2010 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#if (defined HAVE_CONFIG_H) && (!defined WIN32)
  #include "config.h"
#elif defined(_WIN32)
#include "system.h"
#endif

#include "OMXVideo.h"

#include "OMXStreamInfo.h"
#include "utils/log.h"
#include "linux/XMemUtils.h"

#include <sys/time.h>
#include <inttypes.h>
// lcarlon: keep these directives.
#include <GLES2/gl2.h>

#include "omx_globals.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "lc_logging.h"
#include "omx_videosurfaceelement.h"
#include "omx_textureprovider.h"
// ===

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "COMXVideo"

#define OMX_VIDEO_DECODER       "OMX.broadcom.video_decode"
#define OMX_H264BASE_DECODER    OMX_VIDEO_DECODER
#define OMX_H264MAIN_DECODER    OMX_VIDEO_DECODER
#define OMX_H264HIGH_DECODER    OMX_VIDEO_DECODER
#define OMX_MPEG4_DECODER       OMX_VIDEO_DECODER
#define OMX_MSMPEG4V1_DECODER   OMX_VIDEO_DECODER
#define OMX_MSMPEG4V2_DECODER   OMX_VIDEO_DECODER
#define OMX_MSMPEG4V3_DECODER   OMX_VIDEO_DECODER
#define OMX_MPEG4EXT_DECODER    OMX_VIDEO_DECODER
#define OMX_MPEG2V_DECODER      OMX_VIDEO_DECODER
#define OMX_VC1_DECODER         OMX_VIDEO_DECODER
#define OMX_WMV3_DECODER        OMX_VIDEO_DECODER
#define OMX_VP6_DECODER         OMX_VIDEO_DECODER
#define OMX_VP8_DECODER         OMX_VIDEO_DECODER
#define OMX_THEORA_DECODER      OMX_VIDEO_DECODER
#define OMX_MJPEG_DECODER       OMX_VIDEO_DECODER

// lcarlon: keep during merges.
static OMX_EGLBufferProvider* g_provider;

// lcarlon: needed callback for OMX componenent.
OMX_ERRORTYPE fill_buffer_done_callback(OMX_HANDLETYPE handle, OMX_PTR pAppData, OMX_BUFFERHEADERTYPE* pBuffer)
{
    (void)pAppData;

    if (pBuffer->nFlags & OMX_BUFFERFLAG_EOS)
        return OMX_ErrorNone;

    assert(g_provider);
    g_provider->registerFilledBuffer(pBuffer);

    // Get next empty buffer.
    OMX_TextureData* empty = g_provider->getNextEmptyBuffer();
    assert(empty);

    return OMX_FillThisBuffer(handle, empty->m_omxBuffer);
}

COMXVideo::COMXVideo(OMX_EGLBufferProviderSh provider) : m_video_codec_name("")
{
  m_is_open           = false;
  m_deinterlace       = false;
  m_drop_state        = false;
  m_omx_clock         = NULL;
  m_av_clock          = NULL;
  m_submitted_eos     = false;
  m_failed_eos        = false;
  m_settings_changed  = false;
  m_setStartTime      = false;
  m_transform         = OMX_DISPLAY_ROT0;
  m_pixel_aspect      = 1.0f;
  // lcarlon: keep these inits.
  m_provider          = provider;
  g_provider          = provider.get(); // FIXME: I don't really like this.
}

COMXVideo::~COMXVideo()
{
  Close();
}

bool COMXVideo::SendDecoderConfig()
{
  CSingleLock lock (m_critSection);
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  /* send decoder config */
  if(m_config.hints.extrasize > 0 && m_config.hints.extradata != NULL)
  {
    OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer();

    if(omx_buffer == NULL)
    {
      CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_buffer->nOffset = 0;
    omx_buffer->nFilledLen = std::min((OMX_U32)m_config.hints.extrasize, omx_buffer->nAllocLen);

    memset((unsigned char *)omx_buffer->pBuffer, 0x0, omx_buffer->nAllocLen);
    memcpy((unsigned char *)omx_buffer->pBuffer, m_config.hints.extradata, omx_buffer->nFilledLen);
    omx_buffer->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;

    omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
      m_omx_decoder.DecoderEmptyBufferDone(m_omx_decoder.GetComponent(), omx_buffer);
      return false;
    }
  }
  return true;
}

bool COMXVideo::NaluFormatStartCodes(enum AVCodecID codec, uint8_t *in_extradata, int in_extrasize)
{
  switch(codec)
  {
    case AV_CODEC_ID_H264:
      if (in_extrasize < 7 || in_extradata == NULL)
        return true;
      // valid avcC atom data always starts with the value 1 (version), otherwise annexb
      else if ( *in_extradata != 1 )
        return true;
    default: break;
  }
  return false;
}

bool COMXVideo::PortSettingsChanged()
{
  CSingleLock lock (m_critSection);
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;

  if (m_settings_changed)
  {
    m_omx_decoder.DisablePort(m_omx_decoder.GetOutputPort(), true);
  }

  OMX_PARAM_PORTDEFINITIONTYPE port_image;
  OMX_INIT_STRUCTURE(port_image);
  port_image.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &port_image);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
  }

  OMX_CONFIG_POINTTYPE pixel_aspect;
  OMX_INIT_STRUCTURE(pixel_aspect);
  pixel_aspect.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamBrcmPixelAspectRatio, &pixel_aspect);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - error m_omx_decoder.GetParameter(OMX_IndexParamBrcmPixelAspectRatio) omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
  }

  if (pixel_aspect.nX && pixel_aspect.nY)
  {
    float fAspect = (float)pixel_aspect.nX / (float)pixel_aspect.nY;
    m_pixel_aspect = fAspect / m_config.display_aspect;
  }

  if (m_settings_changed)
  {
    CLog::Log(LOGDEBUG, "%s::%s - %dx%d@%.2f interlace:%d deinterlace:%d anaglyph:%d par:%.2f display:%d layer:%d", CLASSNAME, __func__,
        port_image.format.video.nFrameWidth, port_image.format.video.nFrameHeight,
        port_image.format.video.xFramerate / (float)(1<<16), 0, m_deinterlace, m_config.anaglyph, m_pixel_aspect, m_config.display, m_config.layer);

    printf("V:PortSettingsChanged: %dx%d@%.2f interlace:%d deinterlace:%d anaglyph:%d par:%.2f display:%d layer:%d\n",
        port_image.format.video.nFrameWidth, port_image.format.video.nFrameHeight,
        port_image.format.video.xFramerate / (float)(1<<16), 0, m_deinterlace, m_config.anaglyph, m_pixel_aspect, m_config.display, m_config.layer);

    SetVideoEGL();
    m_omx_decoder.EnablePort(m_omx_decoder.GetOutputPort(), true);
    return true;
  }

  OMX_CONFIG_INTERLACETYPE interlace;
  OMX_INIT_STRUCTURE(interlace);
  interlace.nPortIndex = m_omx_decoder.GetOutputPort();
  omx_err = m_omx_decoder.GetConfig(OMX_IndexConfigCommonInterlace, &interlace);

  if(m_config.deinterlace == VS_DEINTERLACEMODE_FORCE)
    m_deinterlace = true;
  else if(m_config.deinterlace == VS_DEINTERLACEMODE_OFF)
    m_deinterlace = false;
  else
    m_deinterlace = interlace.eMode != OMX_InterlaceProgressive;

  if(!m_omx_render.Initialize("OMX.broadcom.egl_render", OMX_IndexParamVideoInit))
    return false;

  m_omx_render.ResetEos();

  CLog::Log(LOGDEBUG, "%s::%s - %dx%d@%.2f interlace:%d deinterlace:%d anaglyph:%d par:%.2f layer:%d", CLASSNAME, __func__,
      port_image.format.video.nFrameWidth, port_image.format.video.nFrameHeight,
      port_image.format.video.xFramerate / (float)(1<<16), interlace.eMode, m_deinterlace, m_config.anaglyph, m_pixel_aspect, m_config.layer);

  printf("V:PortSettingsChanged: %dx%d@%.2f interlace:%d deinterlace:%d anaglyph:%d par:%.2f layer:%d alpha:%d\n",
      port_image.format.video.nFrameWidth, port_image.format.video.nFrameHeight,
      port_image.format.video.xFramerate / (float)(1<<16), interlace.eMode, m_deinterlace, m_config.anaglyph, m_pixel_aspect, m_config.layer, m_config.alpha);



  if(!m_omx_sched.Initialize("OMX.broadcom.video_scheduler", OMX_IndexParamVideoInit))
    return false;

  if(m_deinterlace || m_config.anaglyph)
  {
    if(!m_omx_image_fx.Initialize("OMX.broadcom.image_fx", OMX_IndexParamImageInit))
      return false;
  }

#if 0
  // lcarlon: this is not valid for egl_render.
  OMX_CONFIG_DISPLAYREGIONTYPE configDisplay;
  OMX_INIT_STRUCTURE(configDisplay);
  configDisplay.nPortIndex = m_omx_render.GetInputPort();

  configDisplay.set = (OMX_DISPLAYSETTYPE)(OMX_DISPLAY_SET_ALPHA | OMX_DISPLAY_SET_TRANSFORM | OMX_DISPLAY_SET_LAYER | OMX_DISPLAY_SET_NUM);
  configDisplay.alpha = m_config.alpha;
  configDisplay.num = m_config.display;
  configDisplay.layer = m_config.layer;
  configDisplay.transform = m_transform;
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGWARNING, "%s::%s - could not set transform : %d", CLASSNAME, __func__, m_transform);
    return false;
  }
#endif

  SetVideoEGL();

  if(m_config.hdmi_clock_sync)
  {
    OMX_CONFIG_LATENCYTARGETTYPE latencyTarget;
    OMX_INIT_STRUCTURE(latencyTarget);
    latencyTarget.nPortIndex = m_omx_render.GetInputPort();
    latencyTarget.bEnabled = OMX_TRUE;
    latencyTarget.nFilter = 2;
    latencyTarget.nTarget = 4000;
    latencyTarget.nShift = 3;
    latencyTarget.nSpeedFactor = -135;
    latencyTarget.nInterFactor = 500;
    latencyTarget.nAdjCap = 20;

    omx_err = m_omx_render.SetConfig(OMX_IndexConfigLatencyTarget, &latencyTarget);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_IndexConfigLatencyTarget omx_err(0%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  if(m_deinterlace || m_config.anaglyph)
  {
    bool advanced_deinterlace = port_image.format.video.nFrameWidth * port_image.format.video.nFrameHeight <= 576 * 720;

    if (m_config.anaglyph != OMX_ImageFilterAnaglyphNone || !advanced_deinterlace)
    {
      // Image_fx assumed 3 frames of context. anaglyph and simple deinterlace don't require this
      OMX_PARAM_U32TYPE extra_buffers;
      OMX_INIT_STRUCTURE(extra_buffers);
      extra_buffers.nU32 = -2;

      omx_err = m_omx_image_fx.SetParameter(OMX_IndexParamBrcmExtraBuffers, &extra_buffers);
      if(omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "%s::%s error OMX_IndexParamBrcmExtraBuffers omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
        return false;
      }
    }

    OMX_CONFIG_IMAGEFILTERPARAMSTYPE image_filter;
    OMX_INIT_STRUCTURE(image_filter);

    image_filter.nPortIndex = m_omx_image_fx.GetOutputPort();
    if (m_config.anaglyph != OMX_ImageFilterAnaglyphNone)
    {
      image_filter.nNumParams = 1;
      image_filter.nParams[0] = m_config.anaglyph;
      image_filter.eImageFilter = OMX_ImageFilterAnaglyph;
    }
    else
    {
      image_filter.nNumParams = 1;
      image_filter.nParams[0] = 3;
      if (!advanced_deinterlace)
        image_filter.eImageFilter = OMX_ImageFilterDeInterlaceFast;
      else
        image_filter.eImageFilter = OMX_ImageFilterDeInterlaceAdvanced;
    }
    omx_err = m_omx_image_fx.SetConfig(OMX_IndexConfigCommonImageFilterParameters, &image_filter);
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - OMX_IndexConfigCommonImageFilterParameters omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  if(m_deinterlace || m_config.anaglyph)
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_image_fx, m_omx_image_fx.GetInputPort());
    m_omx_tunnel_image_fx.Initialize(&m_omx_image_fx, m_omx_image_fx.GetOutputPort(), &m_omx_sched, m_omx_sched.GetInputPort());
  }
  else
  {
    m_omx_tunnel_decoder.Initialize(&m_omx_decoder, m_omx_decoder.GetOutputPort(), &m_omx_sched, m_omx_sched.GetInputPort());
  }
  m_omx_tunnel_sched.Initialize(&m_omx_sched, m_omx_sched.GetOutputPort(), &m_omx_render, m_omx_render.GetInputPort());
  m_omx_tunnel_clock.Initialize(m_omx_clock, m_omx_clock->GetInputPort() + 1, &m_omx_sched, m_omx_sched.GetOutputPort() + 1);

  omx_err = m_omx_tunnel_clock.Establish();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_clock.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_tunnel_decoder.Establish();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_decoder.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  if(m_deinterlace || m_config.anaglyph)
  {
    omx_err = m_omx_tunnel_image_fx.Establish();
    if(omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_image_fx.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }

    omx_err = m_omx_image_fx.SetStateForComponent(OMX_StateExecuting);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "%s::%s - m_omx_image_fx.SetStateForComponent omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
      return false;
    }
  }

  omx_err = m_omx_tunnel_sched.Establish();
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_tunnel_sched.Establish omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  omx_err = m_omx_sched.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_sched.SetStateForComponent omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  SetVideoEGLOutputPort();

  omx_err = m_omx_render.SetStateForComponent(OMX_StateExecuting);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - m_omx_render.SetStateForComponent omx_err(0x%08x)", CLASSNAME, __func__, omx_err);
    return false;
  }

  m_settings_changed = true;

  OMX_TextureData* data = m_provider->getNextEmptyBuffer();
  assert(data);

  if ((omx_err = OMX_FillThisBuffer(m_omx_render.GetComponent(), data->m_omxBuffer)) != OMX_ErrorNone) {
     LOG_ERROR(LOG_TAG, "Error: %x.", (unsigned int)omx_err);
  }

  return true;
}

bool COMXVideo::Open(OMXClock *clock, const OMXVideoConfig &config)
{
  CSingleLock lock (m_critSection);
  bool vflip = false;
  Close();
  OMX_ERRORTYPE omx_err   = OMX_ErrorNone;
  std::string decoder_name;
  m_settings_changed = false;
  m_setStartTime = true;

  m_config = config;
  // lcarlon: unused when using egl_render.
#if 0
  m_src_rect.SetRect(0, 0, 0, 0);
#endif

  m_video_codec_name      = "";
  m_codingType            = OMX_VIDEO_CodingUnused;

  m_submitted_eos = false;
  m_failed_eos    = false;

  if(!m_config.hints.width || !m_config.hints.height)
    return false;

  // TODO: Re-use old textures.
  //m_textureData = textureData; // Used in case not NULL.
  //m_textureData = NULL;

  // lcarlon: it is important that the generation of the texture is done in the rendering
  // thread. Beware that BlockingQueuedConnection hardlocks when on the same thread.
  LOG_VERBOSE(LOG_TAG, "Generating texture of size (%d, %d).", m_config.hints.width, m_config.hints.height);
#if 0
  QMetaObject::invokeMethod(
           m_provider,
           "instantiateTexture",
           Qt::BlockingQueuedConnection,
           Q_RETURN_ARG(OMX_TextureData*, textureData),
           Q_ARG(QSize, QSize(hints.width, hints.height)));
#else
  // lcarlon: there are cases in which I don't want to generate a new texture, but I want to
  // re-use an existing one. For instance seek and restart may want to simply close and reopen
  // the video player without generating a new texture.
  QSize videoSize(m_config.hints.width, m_config.hints.height);
  //if (!m_textureData) {
     //m_textureData = m_provider->instantiateTexture(videoSize);
  m_provider->free();

  if (m_provider->getBufferCount() <= 0) {
     m_provider->instantiateTextures(videoSize, 4);
     LOG_VERBOSE(LOG_TAG, "Texture generated!");
  }
  else
     m_provider->cleanTextures();

  //}
  //else {
     // It means the user wants to re-use the texture. Just double-check the size is correct.
     //if (videoSize != m_textureData->m_textureSize) {
     //   LOG_ERROR(LOG_TAG, "Trying to re-use a texture with a wrong size!");
     //   return false;
     //}
     //LOG_VERBOSE(LOG_TAG, "Well done, reusing existing texture.");
  //}
#endif
  //if (!m_textureData) {
  //   LOG_WARNING(LOG_TAG, "No texture was instantiated. Can't go on.");
  //   return false;
  //}

  //emit textureDataReady(m_textureData);

  if(!m_config.hints.width || !m_config.hints.height)
    return false;

  switch (m_config.hints.codec)
  {
    case AV_CODEC_ID_H264:
    {
      switch(m_config.hints.profile)
      {
        case FF_PROFILE_H264_BASELINE:
          // (role name) video_decoder.avc
          // H.264 Baseline profile
          decoder_name = OMX_H264BASE_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        case FF_PROFILE_H264_MAIN:
          // (role name) video_decoder.avc
          // H.264 Main profile
          decoder_name = OMX_H264MAIN_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        case FF_PROFILE_H264_HIGH:
          // (role name) video_decoder.avc
          // H.264 Main profile
          decoder_name = OMX_H264HIGH_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        case FF_PROFILE_UNKNOWN:
          decoder_name = OMX_H264HIGH_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
        default:
          decoder_name = OMX_H264HIGH_DECODER;
          m_codingType = OMX_VIDEO_CodingAVC;
          m_video_codec_name = "omx-h264";
          break;
      }
    }
    if (m_config.allow_mvc && m_codingType == OMX_VIDEO_CodingAVC)
    {
       m_codingType = OMX_VIDEO_CodingMVC;
       m_video_codec_name = "omx-mvc";
    }
    break;
    case AV_CODEC_ID_MPEG4:
      // (role name) video_decoder.mpeg4
      // MPEG-4, DivX 4/5 and Xvid compatible
      decoder_name = OMX_MPEG4_DECODER;
      m_codingType = OMX_VIDEO_CodingMPEG4;
      m_video_codec_name = "omx-mpeg4";
      break;
    case AV_CODEC_ID_MPEG1VIDEO:
    case AV_CODEC_ID_MPEG2VIDEO:
      // (role name) video_decoder.mpeg2
      // MPEG-2
      decoder_name = OMX_MPEG2V_DECODER;
      m_codingType = OMX_VIDEO_CodingMPEG2;
      m_video_codec_name = "omx-mpeg2";
      break;
    case AV_CODEC_ID_H263:
      // (role name) video_decoder.mpeg4
      // MPEG-4, DivX 4/5 and Xvid compatible
      decoder_name = OMX_MPEG4_DECODER;
      m_codingType = OMX_VIDEO_CodingMPEG4;
      m_video_codec_name = "omx-h263";
      break;
    case AV_CODEC_ID_VP6:
      // this form is encoded upside down
      vflip = true;
      // fall through
    case AV_CODEC_ID_VP6F:
    case AV_CODEC_ID_VP6A:
      // (role name) video_decoder.vp6
      // VP6
      decoder_name = OMX_VP6_DECODER;
      m_codingType = OMX_VIDEO_CodingVP6;
      m_video_codec_name = "omx-vp6";
    break;
    case AV_CODEC_ID_VP8:
      // (role name) video_decoder.vp8
      // VP8
      decoder_name = OMX_VP8_DECODER;
      m_codingType = OMX_VIDEO_CodingVP8;
      m_video_codec_name = "omx-vp8";
    break;
    case AV_CODEC_ID_THEORA:
      // (role name) video_decoder.theora
      // theora
      decoder_name = OMX_THEORA_DECODER;
      m_codingType = OMX_VIDEO_CodingTheora;
      m_video_codec_name = "omx-theora";
    break;
    case AV_CODEC_ID_MJPEG:
    case AV_CODEC_ID_MJPEGB:
      // (role name) video_decoder.mjpg
      // mjpg
      decoder_name = OMX_MJPEG_DECODER;
      m_codingType = OMX_VIDEO_CodingMJPEG;
      m_video_codec_name = "omx-mjpeg";
    break;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
      // (role name) video_decoder.vc1
      // VC-1, WMV9
      decoder_name = OMX_VC1_DECODER;
      m_codingType = OMX_VIDEO_CodingWMV;
      m_video_codec_name = "omx-vc1";
      break;
    default:
      printf("Vcodec id unknown: %x\n", m_config.hints.codec);
      return false;
    break;
  }

  if(!m_omx_decoder.Initialize(decoder_name, OMX_IndexParamVideoInit))
    return false;

  if(clock == NULL)
    return false;

  m_av_clock = clock;
  m_omx_clock = m_av_clock->GetOMXClock();

  if(m_omx_clock->GetComponent() == NULL)
  {
    m_av_clock = NULL;
    m_omx_clock = NULL;
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateIdle);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open m_omx_decoder.SetStateForComponent\n");
    return false;
  }

  OMX_VIDEO_PARAM_PORTFORMATTYPE formatType;
  OMX_INIT_STRUCTURE(formatType);
  formatType.nPortIndex = m_omx_decoder.GetInputPort();
  formatType.eCompressionFormat = m_codingType;

  if (m_config.hints.fpsscale > 0 && m_config.hints.fpsrate > 0)
  {
    formatType.xFramerate = (long long)(1<<16)*m_config.hints.fpsrate / m_config.hints.fpsscale;
  }
  else
  {
    formatType.xFramerate = 25 * (1<<16);
  }

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamVideoPortFormat, &formatType);
  if(omx_err != OMX_ErrorNone)
    return false;

  OMX_PARAM_PORTDEFINITIONTYPE portParam;
  OMX_INIT_STRUCTURE(portParam);
  portParam.nPortIndex = m_omx_decoder.GetInputPort();

  omx_err = m_omx_decoder.GetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  portParam.nPortIndex = m_omx_decoder.GetInputPort();
  portParam.nBufferCountActual = m_config.fifo_size ? m_config.fifo_size * 1024 * 1024 / portParam.nBufferSize : 80;

  portParam.format.video.nFrameWidth  = m_config.hints.width;
  portParam.format.video.nFrameHeight = m_config.hints.height;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamPortDefinition, &portParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexParamPortDefinition omx_err(0x%08x)\n", omx_err);
    return false;
  }

  // request portsettingschanged on aspect ratio change
  OMX_CONFIG_REQUESTCALLBACKTYPE notifications;
  OMX_INIT_STRUCTURE(notifications);
  notifications.nPortIndex = m_omx_decoder.GetOutputPort();
  notifications.nIndex = OMX_IndexParamBrcmPixelAspectRatio;
  notifications.bEnable = OMX_TRUE;

  omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexConfigRequestCallback, &notifications);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open OMX_IndexConfigRequestCallback error (0%08x)\n", omx_err);
    return false;
  }

  OMX_PARAM_BRCMVIDEODECODEERRORCONCEALMENTTYPE concanParam;
  OMX_INIT_STRUCTURE(concanParam);
  if(1)
    concanParam.bStartWithValidFrame = OMX_TRUE;
  else
    concanParam.bStartWithValidFrame = OMX_FALSE;

  omx_err = m_omx_decoder.SetParameter(OMX_IndexParamBrcmVideoDecodeErrorConcealment, &concanParam);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexParamBrcmVideoDecodeErrorConcealment omx_err(0x%08x)\n", omx_err);
    return false;
  }

  // broadcom omx entension:
  // When enabled, the timestamp fifo mode will change the way incoming timestamps are associated with output images.
  // In this mode the incoming timestamps get used without re-ordering on output images.
  // recent firmware will actually automatically choose the timestamp stream with the least variance, so always enable
  {
    OMX_CONFIG_BOOLEANTYPE timeStampMode;
    OMX_INIT_STRUCTURE(timeStampMode);
    timeStampMode.bEnabled = OMX_TRUE;
    omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexParamBrcmVideoTimestampFifo, &timeStampMode);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open OMX_IndexParamBrcmVideoTimestampFifo error (0%08x)\n", omx_err);
      return false;
    }
  }

  if(NaluFormatStartCodes(m_config.hints.codec, (uint8_t *)m_config.hints.extradata, m_config.hints.extrasize))
  {
    OMX_NALSTREAMFORMATTYPE nalStreamFormat;
    OMX_INIT_STRUCTURE(nalStreamFormat);
    nalStreamFormat.nPortIndex = m_omx_decoder.GetInputPort();
    nalStreamFormat.eNaluFormat = OMX_NaluFormatStartCodes;

    omx_err = m_omx_decoder.SetParameter((OMX_INDEXTYPE)OMX_IndexParamNalStreamFormatSelect, &nalStreamFormat);
    if (omx_err != OMX_ErrorNone)
    {
      CLog::Log(LOGERROR, "COMXVideo::Open OMX_IndexParamNalStreamFormatSelect error (0%08x)\n", omx_err);
      return false;
    }
  }

  // Alloc buffers for the omx intput port.
  omx_err = m_omx_decoder.AllocInputBuffers();
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open AllocOMXInputBuffers error (0%08x)\n", omx_err);
    return false;
  }

  omx_err = m_omx_decoder.SetStateForComponent(OMX_StateExecuting);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error m_omx_decoder.SetStateForComponent\n");
    return false;
  }

  SendDecoderConfig();

  m_is_open           = true;
  m_drop_state        = false;
  m_setStartTime      = true;

  switch(m_config.hints.orientation)
  {
    case 90:
      m_transform = OMX_DISPLAY_ROT90;
      break;
    case 180:
      m_transform = OMX_DISPLAY_ROT180;
      break;
    case 270:
      m_transform = OMX_DISPLAY_ROT270;
      break;
    default:
      m_transform = OMX_DISPLAY_ROT0;
      break;
  }
  if (vflip)
      m_transform = OMX_DISPLAY_MIRROR_ROT180;

  if(m_omx_decoder.BadState())
    return false;

  CLog::Log(LOGDEBUG,
    "%s::%s - decoder_component(0x%p), input_port(0x%x), output_port(0x%x) deinterlace %d hdmiclocksync %d\n",
    CLASSNAME, __func__, m_omx_decoder.GetComponent(), m_omx_decoder.GetInputPort(), m_omx_decoder.GetOutputPort(),
    m_config.deinterlace, m_config.hdmi_clock_sync);

  float fAspect = m_config.hints.aspect ? (float)m_config.hints.aspect / (float)m_config.hints.width * (float)m_config.hints.height : 1.0f;
  m_pixel_aspect = fAspect / m_config.display_aspect;

  return true;
}

void COMXVideo::Close()
{
  CSingleLock lock (m_critSection);
  m_omx_tunnel_clock.Deestablish();
  m_omx_tunnel_decoder.Deestablish();
  if(m_deinterlace || m_config.anaglyph)
    m_omx_tunnel_image_fx.Deestablish();
  m_omx_tunnel_sched.Deestablish();

  m_omx_decoder.FlushInput();

  m_omx_sched.Deinitialize();
  m_omx_decoder.Deinitialize();
  if(m_deinterlace || m_config.anaglyph)
    m_omx_image_fx.Deinitialize();
  m_omx_render.Deinitialize();

  m_is_open       = false;

  m_video_codec_name  = "";
  m_deinterlace       = false;
  m_config.anaglyph          = OMX_ImageFilterAnaglyphNone;
  m_av_clock          = NULL;
}

void COMXVideo::SetDropState(bool bDrop)
{
  m_drop_state = bDrop;
}

unsigned int COMXVideo::GetFreeSpace()
{
  CSingleLock lock (m_critSection);
  return m_omx_decoder.GetInputBufferSpace();
}

unsigned int COMXVideo::GetSize()
{
  CSingleLock lock (m_critSection);
  return m_omx_decoder.GetInputBufferSize();
}

int COMXVideo::Decode(uint8_t *pData, int iSize, double pts)
{
  CSingleLock lock (m_critSection);
  OMX_ERRORTYPE omx_err;

  if( m_drop_state || !m_is_open )
    return true;

    unsigned int demuxer_bytes = (unsigned int)iSize;
    uint8_t *demuxer_content = pData;

  if (demuxer_content && demuxer_bytes > 0)
  {
    while(demuxer_bytes)
    {
      // 500ms timeout
      OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(500);
      if(omx_buffer == NULL)
      {
        CLog::Log(LOGERROR, "OMXVideo::Decode timeout\n");
        printf("COMXVideo::Decode timeout\n");
        return false;
      }

      omx_buffer->nFlags = 0;
      omx_buffer->nOffset = 0;

      if(m_setStartTime)
      {
        omx_buffer->nFlags |= OMX_BUFFERFLAG_STARTTIME;
        CLog::Log(LOGDEBUG, "OMXVideo::Decode VDec : setStartTime %f\n", (pts == DVD_NOPTS_VALUE ? 0.0 : pts) / DVD_TIME_BASE);
        m_setStartTime = false;
      }
      else if(pts == DVD_NOPTS_VALUE)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_TIME_UNKNOWN;

      omx_buffer->nTimeStamp = ToOMXTime((uint64_t)(pts == DVD_NOPTS_VALUE) ? 0 : pts);
      omx_buffer->nFilledLen = std::min((OMX_U32)demuxer_bytes, omx_buffer->nAllocLen);
      memcpy(omx_buffer->pBuffer, demuxer_content, omx_buffer->nFilledLen);

      demuxer_bytes -= omx_buffer->nFilledLen;
      demuxer_content += omx_buffer->nFilledLen;

      if(demuxer_bytes == 0)
        omx_buffer->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

      omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
      if (omx_err != OMX_ErrorNone)
      {
        CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
        printf("%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
        m_omx_decoder.DecoderEmptyBufferDone(m_omx_decoder.GetComponent(), omx_buffer);
        return false;
      }
      //CLog::Log(LOGINFO, "VideD: dts:%.0f pts:%.0f size:%d)\n", dts, pts, iSize);

      omx_err = m_omx_decoder.WaitForEvent(OMX_EventPortSettingsChanged, 0);
      if (omx_err == OMX_ErrorNone)
      {
        if(!PortSettingsChanged())
        {
          CLog::Log(LOGERROR, "%s::%s - error PortSettingsChanged omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
          return false;
        }
      }
      omx_err = m_omx_decoder.WaitForEvent(OMX_EventParamOrConfigChanged, 0);
      if (omx_err == OMX_ErrorNone)
      {
        if(!PortSettingsChanged())
        {
          CLog::Log(LOGERROR, "%s::%s - error PortSettingsChanged (EventParamOrConfigChanged) omx_err(0x%08x)\n", CLASSNAME, __func__, omx_err);
        }
      }
    }
    return true;
  }

  return false;
}

void COMXVideo::Reset(void)
{
  CSingleLock lock (m_critSection);
  if(!m_is_open)
    return;

  m_setStartTime      = true;
  m_omx_decoder.FlushInput();
  if(m_deinterlace || m_config.anaglyph)
    m_omx_image_fx.FlushInput();
}

///////////////////////////////////////////////////////////////////////////////////////////
void COMXVideo::SetVideoRect(const CRect& SrcRect, const CRect& DestRect)
{
  CSingleLock lock (m_critSection);
  if(!m_is_open)
    return;

  if ( !((DestRect.x2 > DestRect.x1 && DestRect.y2 > DestRect.y1) || m_pixel_aspect != 0.0f) )
    return;

  OMX_ERRORTYPE omx_err;
  OMX_CONFIG_DISPLAYREGIONTYPE configDisplay;
  OMX_INIT_STRUCTURE(configDisplay);
  configDisplay.nPortIndex = m_omx_render.GetInputPort();

  // configured dest_rect takes precedence
  if (DestRect.x2 > DestRect.x1 && DestRect.y2 > DestRect.y1)
  {
    configDisplay.fullscreen = OMX_FALSE;
    configDisplay.noaspect   = OMX_TRUE;

    configDisplay.set                 = (OMX_DISPLAYSETTYPE)(OMX_DISPLAY_SET_DEST_RECT|OMX_DISPLAY_SET_SRC_RECT|OMX_DISPLAY_SET_FULLSCREEN|OMX_DISPLAY_SET_NOASPECT);
    configDisplay.dest_rect.x_offset  = (int)(DestRect.x1+0.5f);
    configDisplay.dest_rect.y_offset  = (int)(DestRect.y1+0.5f);
    configDisplay.dest_rect.width     = (int)(DestRect.Width()+0.5f);
    configDisplay.dest_rect.height    = (int)(DestRect.Height()+0.5f);

    configDisplay.src_rect.x_offset   = (int)(SrcRect.x1+0.5f);
    configDisplay.src_rect.y_offset   = (int)(SrcRect.y1+0.5f);
    configDisplay.src_rect.width      = (int)(SrcRect.Width()+0.5f);
    configDisplay.src_rect.height     = (int)(SrcRect.Height()+0.5f);
  }
  else /* if (m_pixel_aspect != 0.0f) */
  {
    AVRational aspect = av_d2q(m_pixel_aspect, 100);
    configDisplay.set      = OMX_DISPLAY_SET_PIXEL;
    configDisplay.pixel_x  = aspect.num;
    configDisplay.pixel_y  = aspect.den;
  }
  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::Open error OMX_IndexConfigDisplayRegion omx_err(0x%08x)\n", omx_err);
  }
}

void COMXVideo::SetAlpha(int alpha)
{
  CSingleLock lock (m_critSection);
  if(!m_is_open)
    return;

  OMX_ERRORTYPE omx_err;
  OMX_CONFIG_DISPLAYREGIONTYPE configDisplay;
  OMX_INIT_STRUCTURE(configDisplay);

  configDisplay.nPortIndex = m_omx_render.GetInputPort();
  configDisplay.set = OMX_DISPLAY_SET_ALPHA;
  configDisplay.alpha = alpha;

  omx_err = m_omx_render.SetConfig(OMX_IndexConfigDisplayRegion, &configDisplay);
  if(omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "COMXVideo::ALPHA::Open error OMX_IndexConfigDisplayRegion omx_err(0x%08x)\n", omx_err);
  }

}

// lcarlon: keep during merges.
bool COMXVideo::SetVideoEGL()
{
   // Query output buffer requirements for renderer and provide the native display
   // the renderer will use.
   LOG_DEBUG(LOG_TAG, "%s", Q_FUNC_INFO);

   OMX_PARAM_PORTDEFINITIONTYPE portdef;
   portdef.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
   portdef.nVersion.nVersion = OMX_VERSION;
   portdef.nPortIndex = m_omx_render.GetOutputPort();
   OMX_ERRORTYPE omx_err = m_omx_render.GetParameter(OMX_IndexParamPortDefinition, &portdef);
   if (omx_err != OMX_ErrorNone)
      CLog::Log(LOGERROR, "Failed to get port definition for renderer output port.");

   portdef.nBufferCountActual = 4;
   portdef.format.video.pNativeWindow = get_egl_display();
   omx_err = m_omx_render.SetParameter(OMX_IndexParamPortDefinition, &portdef);
   if (omx_err != OMX_ErrorNone)
      CLog::Log(LOGERROR, "Failed to set port definition for renderer output port.");

   return true;
}

bool COMXVideo::SetVideoEGLOutputPort()
{
   // This should be enabled by default. However, I set this explicitly as discard mode to
   // off is crashing my Pi.
   OMX_CONFIG_PORTBOOLEANTYPE booltype;
   booltype.nSize = sizeof(OMX_CONFIG_PORTBOOLEANTYPE);
   booltype.nVersion.nVersion = OMX_VERSION;
   booltype.nPortIndex = 220;
   booltype.bEnabled = OMX_TRUE;
   OMX_ERRORTYPE omx_err = m_omx_render.SetParameter(OMX_IndexParamBrcmVideoEGLRenderDiscardMode, &booltype);
   if (omx_err != OMX_ErrorNone)
      log_warn("Failed to set OMX_IndexParamBrcmVideoEGLRenderDiscardMode.");

   OMX_PARAM_PORTDEFINITIONTYPE portFormat;
   OMX_INIT_STRUCTURE(portFormat);
   portFormat.nPortIndex = m_omx_render.GetOutputPort();
   omx_err = m_omx_render.GetParameter(OMX_IndexParamPortDefinition, &portFormat);
   if(omx_err != OMX_ErrorNone)
     return omx_err;

   m_omx_render.m_output_alignment     = portFormat.nBufferAlignment;
   m_omx_render.m_output_buffer_count  = portFormat.nBufferCountActual;
   m_omx_render.m_output_buffer_size   = portFormat.nBufferSize;

   log_debug("Trying to enable to output port...");
   m_omx_render.EnablePort(m_omx_render.GetOutputPort(), false);

   // Buffers are freed using another technique.
   m_omx_render.m_omx_output_use_buffers = false;

   // Get buffers for images.
   QList<OMX_TextureData*> datas = m_provider->getBuffers();
   log_verbose("Creating buffers for %d images.", datas.size());
   foreach (OMX_TextureData* data, datas) {
      omx_err = OMX_UseEGLImage(m_omx_render.GetComponent(), &(data->m_omxBuffer), 221, NULL, data->m_eglImage);
      if (omx_err != OMX_ErrorNone) {
         CLog::Log(LOGERROR, "OpenMAXILTextureLoader::decode - OMX_UseEGLImage - failed with omxErr(0x%x)\n", omx_err);
         return false;
      }

      log_info("Buffer created %p", data->m_omxBuffer);
      m_omx_render.m_omx_output_available.push(data->m_omxBuffer);
      m_omx_render.m_omx_output_buffers.push_back(data->m_omxBuffer);
   }

   LOG_VERBOSE(LOG_TAG, "Component renderer: %x.", (unsigned int)m_omx_render.GetComponent());
   m_omx_render.SetCustomDecoderFillBufferDoneHandler(&fill_buffer_done_callback);

   // Taken from OMXCore.
   m_omx_render.m_flush_output = false;

   return true;
}

int COMXVideo::GetInputBufferSize()
{
  CSingleLock lock (m_critSection);
  return m_omx_decoder.GetInputBufferSize();
}

void COMXVideo::SubmitEOS()
{
  CSingleLock lock (m_critSection);
  if(!m_is_open)
    return;

  m_submitted_eos = true;
  m_failed_eos = false;

  OMX_ERRORTYPE omx_err = OMX_ErrorNone;
  OMX_BUFFERHEADERTYPE *omx_buffer = m_omx_decoder.GetInputBuffer(1000);

  if(omx_buffer == NULL)
  {
    CLog::Log(LOGERROR, "%s::%s - buffer error 0x%08x", CLASSNAME, __func__, omx_err);
    m_failed_eos = true;
    return;
  }

  omx_buffer->nOffset     = 0;
  omx_buffer->nFilledLen  = 0;
  omx_buffer->nTimeStamp  = ToOMXTime(0LL);

  omx_buffer->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_EOS | OMX_BUFFERFLAG_TIME_UNKNOWN;

  omx_err = m_omx_decoder.EmptyThisBuffer(omx_buffer);
  if (omx_err != OMX_ErrorNone)
  {
    CLog::Log(LOGERROR, "%s::%s - OMX_EmptyThisBuffer() failed with result(0x%x)\n", CLASSNAME, __func__, omx_err);
    m_omx_decoder.DecoderEmptyBufferDone(m_omx_decoder.GetComponent(), omx_buffer);
    return;
  }
  CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
}

bool COMXVideo::IsEOS()
{
  CSingleLock lock (m_critSection);
  if(!m_is_open)
    return true;
  if (!m_failed_eos && !m_omx_render.IsEOS())
    return false;
  if (m_submitted_eos)
  {
    CLog::Log(LOGINFO, "%s::%s", CLASSNAME, __func__);
    m_submitted_eos = false;
  }
  return true;
}
