#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <QObject>
#include <QFile>
#include <QTextStream>
#include <QDebug>
#include <iostream>

using namespace std;

class Watchdog: public QObject {
	Q_OBJECT

public slots:
	bool heartbeat() {
		QTextStream out(&file);
		out << "1";
		return true;
	}
	
	bool stop() {
		QTextStream out(&file);
		out << "V";
		return true;
	}

public:
	const QString filename = "/dev/watchdog";
	QFile file;
	
	Watchdog() {
		file.setFileName(filename);
		file.open(QFile::WriteOnly | QFile::Truncate);
	}
	
	~Watchdog() {
		file.close();
	}
};

#endif // WATCHDOG_H
