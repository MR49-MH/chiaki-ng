// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "streamstatsprovider.h"

#include "shmframepublisher.h"

#include <QDateTime>

namespace {
// std::atomic::fetch_max 是 C++26 特性，手写 CAS 循环替代。
inline void atomic_fetch_max(std::atomic<quint32> &slot, quint32 val)
{
	quint32 prev = slot.load(std::memory_order_relaxed);
	while(val > prev && !slot.compare_exchange_weak(prev, val, std::memory_order_relaxed))
	{
	}
}
}

StreamStatsProvider &StreamStatsProvider::instance()
{
	static StreamStatsProvider s_instance;
	return s_instance;
}

StreamStatsProvider::StreamStatsProvider(QObject *parent)
	: QObject(parent)
{
	connect(&refresh_timer_, &QTimer::timeout, this, &StreamStatsProvider::Refresh);
	refresh_timer_.start(500);
	Refresh();
}

void StreamStatsProvider::OnFrame(quint32 width, quint32 height,
		double pull_ms, double xfer_ms, qint32 frames_lost)
{
	frames_total_.fetch_add(1, std::memory_order_relaxed);
	if(frames_lost > 0)
		lost_total_.fetch_add(static_cast<quint64>(frames_lost), std::memory_order_relaxed);
	const auto pull_us = static_cast<quint32>(pull_ms * 1000.0);
	pull_sum_us_.fetch_add(pull_us, std::memory_order_relaxed);
	pull_count_.fetch_add(1, std::memory_order_relaxed);
	pull_last_us_.store(pull_us, std::memory_order_relaxed);
	atomic_fetch_max(pull_max_us_, pull_us);
	last_w_.store(width, std::memory_order_relaxed);
	last_h_.store(height, std::memory_order_relaxed);
	if(xfer_ms >= 0.0)
	{
		const auto xfer_us = static_cast<quint32>(xfer_ms * 1000.0);
		xfer_sum_us_.fetch_add(xfer_us, std::memory_order_relaxed);
		xfer_count_.fetch_add(1, std::memory_order_relaxed);
		xfer_last_us_.store(xfer_us, std::memory_order_relaxed);
		atomic_fetch_max(xfer_max_us_, xfer_us);
	}
}

void StreamStatsProvider::SetStreamInfo(const QString &decoder_name, const QString &codec_name)
{
	decoder_name_ = decoder_name;
	codec_name_ = codec_name;
}

void StreamStatsProvider::Refresh()
{
	auto &pub = ShmFramePublisher::instance();
	const quint64 frames = frames_total_.load(std::memory_order_relaxed);
	const quint64 shm_pub = pub.published_frames();
	const quint64 pull_sum = pull_sum_us_.exchange(0, std::memory_order_relaxed);
	const quint32 pull_cnt = pull_count_.exchange(0, std::memory_order_relaxed);
	const quint32 pull_max = pull_max_us_.exchange(0, std::memory_order_relaxed);
	const quint64 xfer_sum = xfer_sum_us_.exchange(0, std::memory_order_relaxed);
	const quint32 xfer_cnt = xfer_count_.exchange(0, std::memory_order_relaxed);
	const quint32 xfer_max = xfer_max_us_.exchange(0, std::memory_order_relaxed);

	const qint64 tick = QDateTime::currentMSecsSinceEpoch();
	double dt_s = prev_tick_us_ > 0 ? (tick - prev_tick_us_) / 1000.0 : 0.0;
	prev_tick_us_ = tick;

	fps = dt_s > 0.05 ? (frames - prev_frames_) / dt_s : fps;
	shm_fps = dt_s > 0.05 ? (shm_pub - prev_shm_published_) / dt_s : shm_fps;
	prev_frames_ = frames;
	prev_shm_published_ = shm_pub;

	pull_ms_avg = pull_cnt ? (pull_sum / pull_cnt) / 1000.0 : 0.0;
	pull_ms_max = pull_max / 1000.0;
	pull_ms_last = pull_last_us_.load(std::memory_order_relaxed) / 1000.0;
	xfer_ms_max = xfer_max / 1000.0;
	xfer_ms_last = xfer_last_us_.load(std::memory_order_relaxed) / 1000.0;
	frames_lost = lost_total_.load(std::memory_order_relaxed);

	shm_active = pub.IsConfigured();
	shm_dropped = pub.dropped_frames();
	shm_queue = pub.StatQueueLen();
	shm_write_ms = pub.StatWriteLastUs() / 1000.0;

	stream_info = QStringLiteral("%1×%2  %3  %4")
					  .arg(QString::number(last_w_.load(std::memory_order_relaxed)),
						  QString::number(last_h_.load(std::memory_order_relaxed)),
						  codec_name_, decoder_name_);

	emit statsChanged();
}
