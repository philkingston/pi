#ifndef SCREENDIMENSIONS_H
#define SCREENDIMENSIONS_H

#include <QObject>
#include <QRect>
#include <QDebug>
#include <iostream>

using namespace std;

class ScreenDimensions: public QObject {
	Q_OBJECT
	
	Q_PROPERTY(int width READ getWidth CONSTANT);
	Q_PROPERTY(int height READ getHeight CONSTANT);

private:
	QRect *rect;
	
public:
	int getWidth() {
		return rect->width();
	}

	Q_INVOKABLE
	int getHeight() {
		return rect->height();
	}
	
	ScreenDimensions(QRect *r) {
		rect = r;
	}
};

#endif // SCREENDIMENSIONS_H
