/*
* Project: PiOmxTextures
* Author:  Luca Carlon
* Date:    08.24.2015
*
* Copyright (c) 2012, 2013, 2014 Luca Carlon. All rights reserved.
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
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with PiOmxTextures. If not, see <http://www.gnu.org/licenses/>.
*/

import QtQuick 2.0
import QtMultimedia 5.0

Rectangle {
	property int index: 0
	property int count: 0
	property var playlist

	anchors.fill: parent

	Component.onCompleted: {
		var arguments = Qt.application.arguments;
		if (arguments.length < 3) {
			console.log("Too few arguments.");
			Qt.quit();
		}

		mediaPlayer.source = arguments[2];
		mediaPlayer.play();
	}

	MediaPlayer {
		id: mediaPlayer
		loops: MediaPlayer.Infinite
	}

	VideoOutput {
		anchors.fill: parent
		source: mediaPlayer
		fillMode: VideoOutput.Stretch
	}
}
