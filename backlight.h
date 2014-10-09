#include <QObject>
#include <QFile>
#include <QTextStream>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <queue>
#include <pthread.h>

using namespace std;

class Backlight: public QObject {
	Q_OBJECT

private:
	struct timespec currentwrite;
	struct timespec lastwrite;
	queue<QString> dataQueue;

public slots:
	bool write(const QString& data) {
		// Simply push the data to the queue.
		dataQueue.push(data);

		return true;
	}

public:
	pthread_t thread;

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

	Backlight() {
		dataQueue.empty();

		// Instantiate the thread to process the incoming data
		pthread_create(&thread, NULL, &queueProcessor, (void*) this);
	}
};
