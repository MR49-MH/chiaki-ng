// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_STREAM_STATS_PROVIDER_H
#define CHIAKI_STREAM_STATS_PROVIDER_H

#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>

// Live stream diagnostics exposed to QML as the "StreamStats" singleton.
//
// Feeding:
//   - QmlBackend's FfmpegFrameAvailable handler calls OnFrame() once per
//     decoded frame (frame thread; atomics only).
//   - ShmFramePublisher exposes its own counters; the refresh timer polls
//     them together with the frame counters and derives rates.
//
// Presentation:
//   - A 500 ms QTimer snapshots the counters into Q_PROPERTY members and
//     emits statsChanged(); StreamView.qml binds its debug rows to them.
class StreamStatsProvider : public QObject
{
	Q_OBJECT
	Q_PROPERTY(double fps MEMBER fps NOTIFY statsChanged)
	Q_PROPERTY(quint64 framesLost MEMBER frames_lost NOTIFY statsChanged)
	Q_PROPERTY(double pullMsLast MEMBER pull_ms_last NOTIFY statsChanged)
	Q_PROPERTY(double pullMsAvg MEMBER pull_ms_avg NOTIFY statsChanged)
	Q_PROPERTY(double pullMsMax MEMBER pull_ms_max NOTIFY statsChanged)
	// < 0 in the current refresh window means no GPU->CPU transfer happened
	// (software decode or zero-copy hw path) — QML shows "n/a" instead of a
	// misleading 0.0 ms.
	Q_PROPERTY(double xferMsLast MEMBER xfer_ms_last NOTIFY statsChanged)
	Q_PROPERTY(double xferMsMax MEMBER xfer_ms_max NOTIFY statsChanged)
	Q_PROPERTY(bool shmActive MEMBER shm_active NOTIFY statsChanged)
	Q_PROPERTY(double shmFps MEMBER shm_fps NOTIFY statsChanged)
	Q_PROPERTY(quint64 shmDropped MEMBER shm_dropped NOTIFY statsChanged)
	Q_PROPERTY(quint32 shmQueue MEMBER shm_queue NOTIFY statsChanged)
	Q_PROPERTY(double shmWriteMs MEMBER shm_write_ms NOTIFY statsChanged)
	// Largest interval between consecutive decoded frames within a refresh
	// window — frame pacing spike detector (the average is just 1000/fps).
	Q_PROPERTY(double gapMsMax MEMBER gap_ms_max NOTIFY statsChanged)
	Q_PROPERTY(QString streamInfo MEMBER stream_info NOTIFY statsChanged)

	public:
		// Global instance, created on first use (app-lifetime, no parent).
		static StreamStatsProvider &instance();

		// Frame thread: one call per decoded frame pulled off the decoder.
		// decode_ms is the avcodec_send_packet duration measured in the lib
		// (the real decode cost); xfer_ms < 0 when no hw->sw copy happened.
		void OnFrame(quint32 width, quint32 height,
				double decode_ms, double xfer_ms, qint32 frames_lost);
		// GUI thread, at session creation.
		void SetStreamInfo(const QString &decoder_name, const QString &codec_name);

	signals:
		void statsChanged();

	private:
		explicit StreamStatsProvider(QObject *parent = nullptr);

		void Refresh();

		QTimer refresh_timer_;
		QString decoder_name_ = QStringLiteral("?");
		QString codec_name_;

		// ---- fed from the frame thread (cumulative, relaxed atomics) ----
		std::atomic<quint64> frames_total_{0};
		std::atomic<quint64> lost_total_{0};
		std::atomic<quint64> pull_sum_us_{0};
		std::atomic<quint32> pull_count_{0};
		std::atomic<quint32> pull_max_us_{0};
		std::atomic<quint32> pull_last_us_{0};
		std::atomic<quint64> xfer_sum_us_{0};
		std::atomic<quint32> xfer_count_{0};
		std::atomic<quint32> xfer_max_us_{0};
		std::atomic<quint32> xfer_last_us_{0};
		std::atomic<quint64> last_frame_us_{0};
		std::atomic<quint32> gap_max_us_{0};
		std::atomic<quint32> last_w_{0};
		std::atomic<quint32> last_h_{0};

		// ---- previous-tick snapshots for rate computation ----
		quint64 prev_frames_ = 0;
		quint64 prev_shm_published_ = 0;
		quint64 prev_pull_sum_ = 0;
		quint32 prev_pull_count_ = 0;
		qint64 prev_tick_us_ = 0;

		// ---- presented values ----
		double fps = 0.0;
		quint64 frames_lost = 0;
		double pull_ms_last = 0.0;
		double pull_ms_avg = 0.0;
		double pull_ms_max = 0.0;
		double xfer_ms_last = 0.0;
		double xfer_ms_max = 0.0;
		bool shm_active = false;
		double shm_fps = 0.0;
		quint64 shm_dropped = 0;
		quint32 shm_queue = 0;
		double shm_write_ms = 0.0;
		double gap_ms_max = 0.0;
		QString stream_info;
};

#endif
