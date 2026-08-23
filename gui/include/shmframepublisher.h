// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_SHM_FRAME_PUBLISHER_H
#define CHIAKI_SHM_FRAME_PUBLISHER_H

#include <QtGlobal>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

struct AVFrame;

// Windows-only shared memory frame publisher.
//
// Publishes decoded frames into a named shared memory ring buffer so external
// tools (e.g. mhrs_autotool) can read raw frames without screen capture.
//
// Binary layout must stay in sync with:
//   - mhrs_autotool/src/stream/shm_reader.py
//   - mhrs_autotool/docs/chiaki-shm-integration-plan.md
//
// Shared memory name : Local\chiaki_ng_frames
// Event name         : Local\chiaki_ng_frame_event (auto-reset)
//
// On non-Windows platforms every method is a no-op so call sites need no
// preprocessor guards.
class ShmFramePublisher
{
	public:
		static ShmFramePublisher &instance();

		// Enabling does not allocate; the mapping is created lazily on the
		// first published frame once size/format are known.
		void set_enabled(bool enabled);
		bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

		// Publish one frame. Enqueues a new reference to the (refcounted)
		// frame and returns immediately; the actual GPU->CPU transfer and
		// shared-memory write happen on the internal publisher thread, so the
		// decode->present chain is never blocked and never races mapping
		// teardown. Never throws; failures are counted only.
		//
		// Thread-safety: publish() runs on the frame thread while
		// set_enabled(false) may arrive on the GUI thread at session quit;
		// set_enabled(false) drains the queue, joins the worker and only then
		// unmaps, so no write can outlive the mapping.
		void publish(const AVFrame *frame, int64_t pts_us);

		uint64_t published_frames() const { return published_frames_; }
		uint64_t dropped_frames() const { return dropped_frames_; }

	private:
		ShmFramePublisher() = default;
		~ShmFramePublisher();
		ShmFramePublisher(const ShmFramePublisher &) = delete;
		ShmFramePublisher &operator=(const ShmFramePublisher &) = delete;

		// Both called with mutex_ held.
		bool configure_locked(uint32_t width, uint32_t height, uint32_t pix_fmt);
		void shutdown_locked();

		// StopWorkerAndShutdown: drain the queue, set the exit flag, join the
		// worker thread and only then unmap. Must be called with NO locks
		// held (it joins the worker, which needs mutex_ to finish).
		void StopWorkerAndShutdown();

		std::atomic<bool> enabled_{false};
		bool configured_ = false;
		uint32_t width_ = 0;
		uint32_t height_ = 0;
		uint32_t pix_fmt_ = 0;
		uint32_t plane_count_ = 0;
		uint32_t plane_offset_[4] = {};
		uint32_t plane_stride_[4] = {};
		uint32_t plane_rows_[4] = {};
		uint32_t plane_copy_bytes_[4] = {};
		uint64_t published_frames_ = 0;
		uint64_t dropped_frames_ = 0;

		// Guards mapping lifetime and the seqlock write section. Held across
		// configure/shutdown and the per-frame copy so a concurrent
		// set_enabled(false) can never unmap a view being written.
		std::mutex mutex_;
		// Geometry of the last failed configure attempt: retrying every frame
		// would spam the log at frame rate, so retries wait for a geometry
		// change or a 5s cooldown (transient causes may clear, e.g. a stale
		// external reader holding the previous named section).
		uint32_t last_fail_w_ = 0;
		uint32_t last_fail_h_ = 0;
		uint32_t last_fail_fmt_ = UINT32_MAX;
		uint64_t last_fail_qpc_us_ = 0;
		uint64_t last_transfer_fail_qpc_us_ = 0; // throttle GPU-transfer warnings

#if defined(Q_OS_WINDOWS)
		void *mapping_handle_ = nullptr;
		void *view_ = nullptr;
		void *event_handle_ = nullptr;
		uint8_t *base_ = nullptr;       // mapped base (256-byte header)
		uint8_t *slots_base_ = nullptr; // base_ + header_size
		uint32_t slot_stride_ = 0;
		uint64_t write_index_ = 0;

		// Async publisher worker. publish() enqueues under mutex_; the worker
		// performs transfer + slot write. thread_exit_ is guarded by mutex_;
		// join happens outside the lock (StopWorkerAndShutdown).
		struct QueuedFrame
		{
			AVFrame *frame;
			int64_t pts_us;
		};
		static constexpr size_t QUEUE_MAX = 2; // drop-oldest beyond this
		std::thread publisher_thread_;
		std::condition_variable queue_cv_;
		std::deque<QueuedFrame> queue_;
		bool thread_exit_ = false;

		void PublisherLoop();
		void PublishSync(const AVFrame *frame, int64_t pts_us);
#endif
};

#endif // CHIAKI_SHM_FRAME_PUBLISHER_H
