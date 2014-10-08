#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <QObject>
#include <QFile>
#include <QTextStream>

class Backlight: public QObject {
	Q_OBJECT

public slots:
	bool write(const QString& data)
	{
		QFile file("/dev/spidev0.0");
		if (!file.open(QFile::WriteOnly | QFile::Truncate))
		return false;

		QTextStream out(&file);
		out << data;
		file.close();
		return true;
	}

public:
	Backlight() {
	}
};

#endif // BACKLIGHT_H
