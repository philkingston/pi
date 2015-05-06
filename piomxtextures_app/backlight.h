#include <QObject>
#include <QFile>
#include <QTextStream>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <queue>
#include <pthread.h>
#include <QPixmap>
#include <QApplication>
#include <QScreen>
#include <QWindow>
#include <QDebug>
#include <QQuickView>
#include <QQuickItem>
#include <QImage>
#include <QRgb>
#include <QColor>
#include <QSize>
#include <QUrl>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickImageProvider>
#include <QQmlImageProviderBase>

using namespace std;

class Backlight: public QObject {
	Q_OBJECT

private:
	struct timespec currentwrite;
	struct timespec lastwrite;
	queue<QString> dataQueue;

public slots:
	bool write(const QString &data) {
		// Simply push the data to the queue.
		dataQueue.push(data);
		return true;
	}

	bool mapImage(const QString &path) {
		QImage image;
		QString file = path;
		file.remove(0, 7);
		image.load(file);

		QString pixData = "";
		QImage scaledImage = image.scaled(QSize(14, 9));
		QRgb p;
		for (int y = 8; y >= 0; y--) {
			p = scaledImage.pixel(11, y);
			pixData += (char) ((p >> 16 & 0xFF));
			pixData += (char) ((p >> 8 & 0xFF));
			pixData += (char) (p & 0xFF);
		}
		for (int x = 12; x >= 1; x--) {
			p = scaledImage.pixel(x, 0);
			pixData += (char) ((p >> 16 & 0xFF));
			pixData += (char) ((p >> 8 & 0xFF));
			pixData += (char) (p & 0xFF);
		}
		for (int y = 0; y < 9; y++) {
			p = scaledImage.pixel(0, y);
			pixData += (char) ((p >> 16 & 0xFF));
			pixData += (char) ((p >> 8 & 0xFF));
			pixData += (char) (p & 0xFF);
		}
		dataQueue.push(pixData);
		return true;
	}

public:
	pthread_t thread1;
	pthread_t thread2;
	QApplication *app;
	QQuickView *view;

	queue<QString> getQueue() {
		return dataQueue;
	}

	QString queuePop() {
		QString data = dataQueue.front();
		dataQueue.pop();
		return data;
	}

	static void *queueProcessor(void *arg) {
		// Rebuild the class object
		Backlight* q = static_cast<Backlight*>(arg);

		while (true) {
			// Only process if we get have something in the queue
			if (q->getQueue().size() != 0) {
				QString data;

				// Prevent the queue getting too big
				while (q->getQueue().size() > 10) {
					data = q->queuePop();
				}

				// Get the data
				data = q->queuePop();

				// Write it to the port
				QFile file("/dev/spidev0.0");
				if (!file.open(QFile::WriteOnly | QFile::Truncate))
					continue;

				QTextStream out(&file);
				out << data;
				file.close();
			}

			// Sleep, crucial to the thread
			usleep(10000);
		}

		// We'll never get here, but still...
		pthread_exit (NULL);
	}

	Backlight(QQuickView *v) {
		view = v;
		dataQueue.empty();

		// Instantiate the thread to process the incoming data
		pthread_create(&thread1, NULL, &queueProcessor, (void*) this);
	}

	~Backlight() {
		cout << "quit backlight" << endl;
	}
};
