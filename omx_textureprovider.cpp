/*
 * Project: PiOmxTextures
 * Author:  Luca Carlon
 * Date:    11.01.2012
 *
 * Copyright (c) 2012, 2013 Luca Carlon. All rights reserved.
 *
 * This file is part of PiOmxTextures.
 *
 * PiOmxTextures is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * PiOmxTextures is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with PiOmxTextures.  If not, see <http://www.gnu.org/licenses/>.
 */

/*------------------------------------------------------------------------------
|    includes
+-----------------------------------------------------------------------------*/
#include <QQuickWindow>
#include <QOpenGLContext>
#include <QGuiApplication>

// Private headers.
#include <private/qguiapplication_p.h>
#include <qpa/qplatformintegration.h>
#include <qpa/qplatformnativeinterface.h>

#include <IL/OMX_Broadcom.h>

#include "lc_logging.h"
#include "omx_textureprovider.h"
#include "omx_globals.h"

/*------------------------------------------------------------------------------
|    OMX_TextureData::OMX_TextureData
+-----------------------------------------------------------------------------*/
OMX_TextureData::OMX_TextureData() :
   m_textureId(0),
   m_textureData(NULL),
   m_eglImage(NULL),
   m_textureSize(QSize(0, 0))
{
   // Do nothing.
}

/*------------------------------------------------------------------------------
|    OMX_TextureData::OMX_TextureData
+-----------------------------------------------------------------------------*/
OMX_TextureData::OMX_TextureData(const OMX_TextureData& textureData) :
   m_textureId(textureData.m_textureId),
   m_textureData(textureData.m_textureData),
   m_eglImage(textureData.m_eglImage),
   m_textureSize(textureData.m_textureSize),
   m_omxBuffer(textureData.m_omxBuffer)
{
   // Do nothing.
}

/*------------------------------------------------------------------------------
|    OMX_TextureData::~OMX_TextureData
+-----------------------------------------------------------------------------*/
OMX_TextureData::~OMX_TextureData()
{
   if (m_textureData || m_textureId || m_eglImage)
      log_warn("Loosing pointers to GPU data.");
}

/*------------------------------------------------------------------------------
|    OMX_TextureData::freeData
+-----------------------------------------------------------------------------*/
void OMX_TextureData::freeData()
{
   if (!m_textureData || !m_textureId || !m_eglImage)
      log_warn("Double free of OMX texture data requested.");

   EGLDisplay eglDisplay = get_egl_display();

   // Destroy texture, EGL image and free the buffer.
   log_info("Freeing KHR image...");
   if (m_eglImage && eglDestroyImageKHR(eglDisplay, m_eglImage) == EGL_SUCCESS) {
      EGLint err = eglGetError();
      LOG_ERROR(LOG_TAG, "Failed to destroy EGLImageKHR: %d.", err);
   }

   m_eglImage = NULL;

   if (m_textureId) {
      log_info("Freeing texture...");
      glDeleteTextures(1, &m_textureId);
      m_textureId = 0;
   }

   if (m_textureData) {
      log_info("Freeing texture data...");
      delete m_textureData;
      m_textureData = NULL;
   }
}

/*------------------------------------------------------------------------------
|    OpenMAXILTextureLoader::instantiateTexture
+-----------------------------------------------------------------------------*/
OMX_TextureData* OMX_TextureProviderQQuickItem::instantiateTexture(QSize size)
{
   EGLDisplay eglDisplay = get_egl_display();
   if (!eglDisplay)
      return (OMX_TextureData*)log_critical("Failed to get EGLDisplay.");

   EGLContext eglContext = get_egl_context();
   if (!eglContext)
      return (OMX_TextureData*)log_critical("Failed to get EGLContext.");

   EGLint attr[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};

   GLuint textureId;
   glGenTextures(1, &textureId);
   glBindTexture(GL_TEXTURE_2D, textureId);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

   // It seems that only 4byte pixels is supported here.
   int pixelSize = size.width() * size.height() * 4;
   GLubyte* pixel = new GLubyte[pixelSize];
   memset(pixel, 0, pixelSize);
   glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, size.width(), size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);

   log_info("Creating EGLImageKHR...");
   EGLImageKHR eglImage = eglCreateImageKHR(
            eglDisplay,
            eglContext,
            EGL_GL_TEXTURE_2D_KHR,
            (EGLClientBuffer)textureId,
            attr
            );
   log_verbose("EGL image %d created...", eglImage);

   EGLint eglErr = eglGetError();
   if (eglErr != EGL_SUCCESS) {
      LOG_ERROR(LOG_TAG, "Failed to create KHR image: %d.", eglErr);
      return 0;
   }

   log_verbose("Creating OMX_TextureData...");
   OMX_TextureData* textureData = new OMX_TextureData;
   textureData->m_textureId   = textureId;
   textureData->m_textureData = pixel;
   textureData->m_eglImage    = eglImage;
   textureData->m_textureSize = size;
   return textureData;
}

/*------------------------------------------------------------------------------
|    OpenMAXILTextureLoader::freeTexture
+-----------------------------------------------------------------------------*/
void OMX_TextureProviderQQuickItem::freeTexture(OMX_TextureData* textureData)
{
   if (!textureData) {
      log_warn("Trying to free a NULL texture data object.");
      return;
   }

   textureData->freeData();
   delete textureData;
}
