#pragma once
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <cmath>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QBuffer>
#include <QFile>
#include <phonon/MediaObject>
#include "notes.hh"
#include "notegraphwidget.hh"
#include "notelabel.hh"
#ifdef Q_OS_WIN
#include <QTemporaryFile>
#include <QSound>
#endif


#ifndef M_PI
	#define M_PI 3.141592653589793
#endif

struct SynthNote {
	SynthNote(): note(24), begin(), length() {}
	SynthNote(const Note& n): note(n.note), begin(n.begin), length(n.length()) {}
	bool operator<(const SynthNote& rhs) { return begin < rhs.begin; }
	int note;
	double begin;
	double length;
};

typedef QList<SynthNote> SynthNotes;


class Synth: public QThread
{
	Q_OBJECT
public:
	Synth(QObject *parent = NULL) : QThread(parent), m_delay(), m_pos(), m_noteBegin(), m_curBuffer(), m_quit()
	{
		qRegisterMetaType<QByteArray>("QByteArray"); // Register type for use with queued connections
	}

	~Synth() { stop(); wait(); }

	/// Updates the synth
	void tick(qint64 pos, const SynthNotes& notes) {
		QMutexLocker locker(&m_mutex);
		m_pos = pos / 1000.0;
		m_notes = notes;

		if (isRunning()) m_condition.wakeOne();
		else start();
	}

	void stop() {
		QMutexLocker locker(&m_mutex);
		m_quit = true;
		m_condition.wakeOne();
	}

signals:
	void playBuffer(const QByteArray&);

protected:
	/// Thread runs here
	void run() {
		calcNext();
		while (!m_quit) {
			m_mutex.lock();
			// Wait here until wake up or time out
			if (m_condition.wait(&m_mutex, m_delay * 1000)) {
				// We were woken up, so let's see if an update is in order
				m_mutex.unlock();
				if (m_quit) break;
				calcNext();

			} else {
				// Time-out: time to play the music
				m_mutex.unlock();
				if (m_quit) break;
				emit playBuffer(m_soundData[m_curBuffer]);
				m_curBuffer = (m_curBuffer+1) % 2;
				// Slightly hacky stuff follows:
				// We advance the time a bit to make sure we are over the note beginning.
				// Then cache the next note, but put longer delay (which will be corrected
				// with the next tick) so that we don't accidentally play wrong note(
				// in case we advanced the time too much).
				m_pos += 0.2;
				calcNext();
				m_delay = std::max(m_delay, 1.0);
			}
		}
	}

private:
	/// Calculates the next values
	void calcNext() {
		QElapsedTimer timer; timer.start();
		SynthNote n;
		{
			QMutexLocker locker(&m_mutex);
			SynthNotes::const_iterator it = m_notes.begin();
			while (it != m_notes.end() && it->begin < m_pos) ++it;
			if (it == m_notes.end()) { m_delay = ULONG_MAX / 1000.0; return; }
			n = *it;
		}

		m_delay = n.begin - m_pos;
		if (n.begin != m_noteBegin) {
			// Need to create a new buffer
			m_noteBegin = n.begin;
			createBuffer(n.note % 12, n.length);
		}
		// Compensate for the time spent in this function
		m_delay -= timer.elapsed() / 1000.0;
		if (m_delay <= 0.001) m_delay = 0.001;
	}

	/// Creates the sound
	void createBuffer(int note, double length) {
		// This is simple beep, so we use mono, low sample rate and only 8 bits resolution
		// --> quick to create and small memory foot print
		std::string header = writeWavHeader(8, 1, sampleRate, length * sampleRate);
		m_soundData[m_curBuffer] = QByteArray(header.c_str(), header.size());
		double d = (note + 1) / 13.0;
		double freq = MusicalScale().getNoteFreq(note + 12);
		double phase = 0;
		// Synthesize tones
		for (size_t i = 0; i < length * sampleRate; ++i) {
			float fvalue = d * 0.2 * std::sin(phase) + 0.2 * std::sin(2 * phase) + (1.0 - d) * 0.2 * std::sin(4 * phase);
			phase += 2.0 * M_PI * freq / sampleRate;

			// 8-bit
			quint8 value = (fvalue + 1) * 0.5 * 255;
			m_soundData[m_curBuffer].push_back(value);
		}

		//std::ofstream of("/tmp/wavdump.wav");
		//of.write(buf.data(), buf.size());
	}

	/// WAV header writer
	std::string writeWavHeader(unsigned bits, unsigned ch, unsigned sr, unsigned samples) {
		std::ostringstream out;
		unsigned bps = ch * bits / 8; // Bytes per sample
		unsigned datasize = bps * samples;
		unsigned size = datasize + 0x2C;
		out.write("RIFF" ,4); // RIFF chunk
		{ unsigned int tmp=size-0x8 ; out.write((char*)(&tmp),4); } // RIFF chunk size
		out.write("WAVEfmt ",8); // WAVEfmt header
		{ int   tmp=0x00000010 ; out.write((char*)(&tmp),4); } // Always 0x10
		{ short tmp=0x0001     ; out.write((char*)(&tmp),2); } // Always 1
		{ short tmp = ch; out.write((char*)(&tmp),2); } // Number of channels
		{ int   tmp = sr; out.write((char*)(&tmp),4); } // Sample rate
		{ int   tmp = bps * sr; out.write((char*)(&tmp),4); } // Bytes per second
		{ short tmp = bps; out.write((char*)(&tmp),2); } // Bytes per frame
		{ short tmp = bits; out.write((char*)(&tmp),2); } // Bits per sample
		out.write("data",4); // data chunk
		{ int   tmp = datasize; out.write((char*)(&tmp),4); }
		return out.str();
	}

	static const int sampleRate = 8000; ///< Sample rate

	SynthNotes m_notes; ///< Notes to synthesize
	double m_delay; ///< How many seconds until the next sound must be played
	double m_pos; ///< Position where we are now
	double m_noteBegin; ///< Position of the next note
	QByteArray m_soundData[2];
	int m_curBuffer;
	bool m_quit;
	QMutex m_mutex;
	QWaitCondition m_condition;
};


class BufferPlayer: public QObject
{
	Q_OBJECT
	Q_DISABLE_COPY(BufferPlayer);
public:
	BufferPlayer(const QByteArray& ba, QObject *parent): QObject(parent), m_data(ba) {
#ifdef Q_OS_WIN
		QTemporaryFile wavfile;
		if (wavfile.open()) {
			QDataStream stream(&wavfile);
			stream.writeRawData(ba.data(), ba.size());
			QSound::play(wavfile.fileName());
		}

#else
		m_player = Phonon::createPlayer(Phonon::MusicCategory);
		m_player->setParent(this);
		m_buffer = new QBuffer(&m_data, this);
		m_player->setCurrentSource(m_buffer);
		connect(m_player, SIGNAL(finished()), this, SLOT(finished()));
		m_player->play();
#endif
	}

public slots:
	void finished() {
		m_player->clear();
		{
			// This seems a bit strange, but looks like it is the best
			// way to release the resources without an occasional crash.
			QObject deleter;
			m_player->setParent(&deleter);
			m_buffer->setParent(&deleter);
		}
		deleteLater();
	}

private:
	QByteArray m_data;
	QBuffer *m_buffer;
	Phonon::MediaObject *m_player;
};
