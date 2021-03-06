/*
 * Project: PiOmxTextures
 * Author:  Luca Carlon
 * Date:    12.03.2012
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

/*----------------------------------------------------------------------
 |    includes
 +---------------------------------------------------------------------*/
#include <QApplication>
#include <QDesktopWidget>
#include <QQuickView>
#include <QTimer>
#include <QDebug>

#include <bcm_host.h>
#include <signal.h>
#include <execinfo.h>

extern "C" {
#include "libavformat/avformat.h"
}

#include "lc_logging.h"
#include "omx_imageelement.h"
#include "omx_videosurfaceelement.h"
#include "omx_camerasurfaceelement.h"
#include "omx_mediaprocessorelement.h"
#include "omx_audioprocessor.h"
#include "omx_mediaprocessor.h"
#include "fileio.h"
#include "watchdog.h"
#include "backlight.h"
#include "screendimensions.h"

int main(int argc, char *argv[]) {
	if (argc != 2) {
		cout << "Error: view20 requires a QML file" << endl;
		return -1;
	}
	QApplication a(argc, argv);

	// Registers all the codecs.
	av_register_all();

	qRegisterMetaType < GLuint > ("GLuint");
	qRegisterMetaType<OMX_TextureData*>("OMX_TextureData*");
	qmlRegisterType < OMX_ImageElement > ("com.luke.qml", 1, 0, "OMXImage");
	qmlRegisterType < OMX_VideoSurfaceElement
			> ("com.luke.qml", 1, 0, "OMXVideoSurface");
	qmlRegisterType < OMX_CameraSurfaceElement
			> ("com.luke.qml", 1, 0, "OMXCameraSurface");
	qmlRegisterType < OMX_MediaProcessorElement
			> ("com.luke.qml", 1, 0, "OMXMediaProcessor");

	QQuickView view;

	// Additional modules
	FileIO fileIO;
	Watchdog watchdog;
	Backlight backlight(&view);
	QRect rect = QApplication::desktop()->screenGeometry();
	ScreenDimensions screenDimensions(&rect);
	view.rootContext()->setContextProperty("fileio", &fileIO);
	view.rootContext()->setContextProperty("watchdog", &watchdog);
	view.rootContext()->setContextProperty("backlight", &backlight);
	view.rootContext()->setContextProperty("screensize", &screenDimensions);

	// QML file to run (command line argument)
	view.setSource(QUrl(argv[1]));

	QQmlEngine *engine = QtQml::qmlEngine(view.rootObject());
	QObject::connect((QObject*) engine, SIGNAL(quit()), &a, SLOT(quit()));
	view.showFullScreen();

	return a.exec();
}
